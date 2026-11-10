#include "ResourceMaintenanceController.h"
#include "FileManager.h"
#include "WorkspaceWindow.h"
#include "WorkspaceTree.h"
#include "WorkspaceReferenceController.h"
#include "SingleFileWindow.h"
#include "SourceEditor.h"
#include "Scintilla.h"
#include "SharedWidgets.h"
#include "Appearance.h"
#include "VisualTokens.h"
#include <QtConcurrentRun>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLineEdit>
#include <QTreeWidget>
#include <QLabel>
#include <QCheckBox>
#include <QScrollArea>
#include <QScreen>
#include <QDir>
#include <QCryptographicHash>
#include <algorithm>

namespace xdnote {
ResourceMaintenanceController::ResourceMaintenanceController(FileManager *manager, WorkspaceWindow *window)
    : QObject(window), m_manager(manager), m_window(window) {
    connect(&m_prepare, &QFutureWatcher<ResourceMaintenancePreview>::finished, this, [this] {
        m_preview = m_prepare.result(); m_busy = false;
        QHash<QString, QPointer<FileDocument>> canonical;
        for (auto it = m_open.cbegin(); it != m_open.cend(); ++it) canonical.insert(m_preview.actualPaths.value(it.key(), it.key()), it.value());
        m_open = std::move(canonical);
        for (auto &document : m_preview.plan.documents) {
            const auto opened = m_open.value(document.from);
            if (opened && (opened->readOnly() || opened->conflict() || opened->hasError())) {
                document.problem = QStringLiteral("打开文档只读或存在文件冲突，保留原正文。");
                m_preview.plan.gaps.append(document.from + QStringLiteral("：") + document.problem);
            }
        }
        if (!m_window || m_window->resourceRoot() != m_root || !m_root || !m_root->active()) m_preview.plan.problem = QStringLiteral("工作区已变化，计划失效。");
        emit previewReady();
    });
    connect(&m_execute, &QFutureWatcher<ResourceMaintenanceResult>::finished, this, [this] { finish(m_execute.result()); });
}
ResourceMaintenanceController::~ResourceMaintenanceController() {
    cancel();
    // The GUI normally refuses close while busy. Teardown still joins workers
    // before releasing the directory and unfreezes surviving sessions.
    m_prepare.waitForFinished(); m_execute.waitForFinished();
    if (m_manager && !m_operation.isNull()) m_manager->endWorkspaceOperation(m_operation);
}
void ResourceMaintenanceController::cancel() { if (m_cancelled) m_cancelled->store(true, std::memory_order_release); }
QString ResourceMaintenanceController::relativePath(const QString &path) const {
    if (!m_root) return {};
    const auto relative = QDir(m_root->rootPath()).relativeFilePath(path);
    return relative == ".." || relative.startsWith("../") || QDir::isAbsolutePath(relative) ? QString{} : relative;
}
void ResourceMaintenanceController::prepare(const QString &from, const QString &to, bool retargetOnly) {
    if (m_busy || !m_window || !m_manager) return;
    m_root = m_window->resourceRoot(); m_open.clear(); m_preview = {};
    if (!m_root || !m_root->active()) { m_preview.plan.problem = QStringLiteral("请先打开有效工作区。"); emit previewReady(); return; }
    ReferenceIndexInput input{QUuid::createUuid(), 1, 1, false, {}, {}};
    QHash<QString, std::shared_ptr<const FileBaseline>> baselines;
    qsizetype bytes = 0;
    for (auto *document : m_manager->documents()) {
        const auto path = relativePath(document->path());
        if (path.isEmpty()) { input.sessionsOutsideRoot.insert(document->identity()); continue; }
        auto snapshot = document->session()->snapshot(); bytes += snapshot.utf8.size();
        if (input.sources.size() >= 1024 || bytes > 32 * 1024 * 1024 || snapshot.result != EditResult::Applied) {
            m_preview.plan.problem = QStringLiteral("打开会话正在输入或超过本轮上限，请完成输入后重试。"); emit previewReady(); return;
        }
        input.sources.append({path, document->identity(), 1, snapshot, ReferenceIndexSource::Origin::Session});
        m_open.insert(path, document); baselines.insert(path, document->baseline());
    }
    m_busy = true; const auto root = m_root;
    m_cancelled = std::make_shared<std::atomic_bool>(false); const auto cancelled = m_cancelled;
    m_prepare.setFuture(QtConcurrent::run([root, input, from, to, baselines, retargetOnly, cancelled] { return prepareResourceMove(root, input, {from, to, {}, false, retargetOnly}, baselines, cancelled); }));
}
bool ResourceMaintenanceController::apply(bool maintain) { m_recovering = false; return start(maintain); }
bool ResourceMaintenanceController::start(bool maintain) {
    const auto reject = [&](const QString &message) { m_result.problem = message; return false; };
    if (m_busy || !m_manager || !m_window || m_window->resourceRoot() != m_root || !m_root || !m_root->active() || !m_preview.plan.valid()) return reject(QStringLiteral("工作区、预览或操作状态已变化。"));
    // Compare every live source, including sources unaffected in the preview:
    // a newly inserted inbound link invalidates the old impact plan too.
    const auto captured = m_preview.live.sources;
    int count = 0;
    for (auto *document : m_manager->documents()) {
        const auto path = relativePath(document->path()); if (path.isEmpty()) continue;
        ++count;
        const auto found = std::find_if(captured.cbegin(), captured.cend(), [&](const auto &source) { return source.path == path; });
        const auto current = document->session()->snapshot();
        if (found == captured.cend() || found->identity != document->identity() || current.result != EditResult::Applied
            || current.documentId != found->snapshot.documentId || current.revision != found->snapshot.revision || current.utf8 != found->snapshot.utf8) return reject(QStringLiteral("预览后打开会话已变化：%1").arg(path));
    }
    if (count != captured.size()) return reject(QStringLiteral("预览后打开会话数量已变化。"));
    m_operation = m_manager->beginWorkspaceOperation(); if (m_operation.isNull()) return reject(QStringLiteral("文件服务正在执行其他操作，请稍后重试。"));
    for (auto *document : m_manager->documents()) {
        if (relativePath(document->path()).isEmpty()) continue;
        if (!m_manager->protectWorkspaceDocument(document, m_operation, true)) {
            m_manager->endWorkspaceOperation(m_operation); m_operation = {}; return reject(QStringLiteral("会话正在输入、文件有冲突或无法暂时保护：%1").arg(document->path()));
        }
    }
    m_window->references()->cancel(); m_busy = true; m_canRecover = false;
    m_cancelled = std::make_shared<std::atomic_bool>(false);
    const auto root = m_root; const auto preview = m_preview; const auto cancelled = m_cancelled;
    m_execute.setFuture(QtConcurrent::run([root, preview, maintain, cancelled] { return executeResourceMove(root, preview, maintain, cancelled); }));
    return true;
}
void ResourceMaintenanceController::finish(ResourceMaintenanceResult result) {
    if (!m_manager || !m_window) return;
    const auto &request = m_preview.plan.request;
    struct Selection { QPointer<SourceEditor> editor; QString path; qsizetype anchor, caret; };
    QList<Selection> selections;
    for (auto *pane : m_window->panes()) if (pane->document() && pane->editor()) {
        const auto path = relativePath(pane->document()->path());
        selections.append({pane->editor(), m_preview.actualPaths.value(path, path), qsizetype(pane->editor()->send(SCI_GETANCHOR)), qsizetype(pane->editor()->send(SCI_GETCURRENTPOS))});
    }
    if (result.move.status == WorkspaceMoveStatus::Moved) {
        for (auto document : m_open) {
            if (!document || document->closed()) continue;
            const auto lexical = relativePath(document->path());
            const auto oldPath = m_preview.actualPaths.value(lexical, lexical), newPath = request.retargetOnly ? oldPath : movedResourcePath(oldPath, request);
            if (oldPath == newPath) continue;
            const auto actual = m_root->inspect(newPath);
            if (!actual.ok() || actual.identity != document->identity()
                || !m_manager->relocateWorkspaceDocument(document, QDir(m_root->rootPath()).filePath(newPath), m_operation))
                m_manager->reportWorkspaceProblem(document, QStringLiteral("路径已移动，无法确认会话新位置；当前编辑已保留，请检查并另存为。"), m_operation);
        }
        for (auto &row : result.rows) {
            if (row.status != ResourceMaintenanceRow::PendingSession) continue;
            const auto found = std::find_if(m_preview.plan.documents.cbegin(), m_preview.plan.documents.cend(), [&](const auto &doc) { return doc.from == row.from; });
            const auto document = m_open.value(row.from);
            if (found == m_preview.plan.documents.cend() || !document || document->closed()
                || document->session()->replaceResourceText(found->before, found->after) != EditResult::Applied) {
                row.status = ResourceMaintenanceRow::Failed; row.message = QStringLiteral("会话已变化或只读，保留原正文。");
            } else {
                const auto mapPosition = [&](qsizetype position) {
                    qsizetype delta = 0;
                    for (const auto &edit : found->edits) {
                        if (position < edit.range.start) break;
                        if (position <= edit.range.end) return edit.range.start + delta + qMin(position - edit.range.start, edit.replacement.size());
                        delta += edit.replacement.size() - (edit.range.end - edit.range.start);
                    }
                    return position + delta;
                };
                for (const auto &selection : selections) if (selection.editor && selection.path == row.from)
                    selection.editor->send(SCI_SETSEL, uptr_t(mapPosition(selection.anchor)), sptr_t(mapPosition(selection.caret)));
            }
        }
    } else if (result.move.status == WorkspaceMoveStatus::Uncertain) {
        for (auto document : m_open) if (document && movedResourcePath(relativePath(document->path()), request) != relativePath(document->path()))
            m_manager->reportWorkspaceProblem(document, QStringLiteral("移动结果不确定，当前编辑已保留。请检查原位置、目标位置与恢复位置后另存为。"), m_operation);
    }
    m_manager->endWorkspaceOperation(m_operation); m_operation = {}; m_busy = false; m_result = std::move(result);
    if (!m_recovering && m_result.move.status == WorkspaceMoveStatus::Moved && !m_result.movedIdentity.isEmpty() && m_result.problem.isEmpty()
        && std::none_of(m_result.rows.cbegin(), m_result.rows.cend(), [](const auto &row) { return row.status == ResourceMaintenanceRow::Uncertain; })) { m_receipt = m_preview; m_receiptResult = m_result; m_canRecover = true; }
    else m_canRecover = false;
    m_window->tree()->refresh(); m_window->references()->refresh();
    QStringList messages;
    if (!m_result.problem.isEmpty()) messages.append(m_result.problem);
    else messages.append(request.retargetOnly ? QStringLiteral("已按确认目标处理引用。各文档结果如下；打开文档需要自行保存。") : QStringLiteral("路径已移动。各文档结果如下；打开的文档需要自行保存。"));
    int saved = 0, pending = 0, failed = 0, skipped = 0;
    for (const auto &row : m_result.rows) {
        if (row.status == ResourceMaintenanceRow::Saved) ++saved;
        else if (row.status == ResourceMaintenanceRow::PendingSession) ++pending;
        else if (row.status == ResourceMaintenanceRow::Skipped) ++skipped;
        else ++failed;
    }
    if (!m_result.rows.isEmpty()) messages.append(QStringLiteral("写入 %1，待保存 %2，跳过 %3，失败或待确认 %4。可在“移动或重命名资源”查看逐项结果及恢复入口。").arg(saved).arg(pending).arg(skipped).arg(failed));
    if (!m_result.move.recoveryPath.isEmpty()) messages.append(QStringLiteral("恢复位置：") + m_result.move.recoveryPath);
    m_window->showNotice(messages.join('\n')); emit completed();
}
bool ResourceMaintenanceController::recover() {
    if (!canRecover() || !m_root || !m_root->active() || !m_manager) return false;
    const auto actual = m_root->entry(m_receipt.plan.request.to); if (!actual.ok() || actual.identity != m_receiptResult.movedIdentity) return false;
    m_preview = reverseResourceMove(m_receipt, m_receiptResult, actual.identity);
    m_open.clear(); m_preview.live = {m_preview.plan.rootId, m_preview.plan.rootGeneration, m_preview.plan.scanGeneration, false, {}, {}}; qsizetype bytes = 0;
    for (auto *document : m_manager->documents()) {
        const auto path = relativePath(document->path()); if (path.isEmpty()) continue;
        const auto snapshot = document->session()->snapshot(); bytes += snapshot.utf8.size();
        if (snapshot.result != EditResult::Applied || bytes > 32 * 1024 * 1024 || m_open.size() >= 1024) return false;
        const auto canonical = m_receipt.actualPaths.value(path, path); m_preview.actualPaths.insert(path, canonical);
        m_open.insert(canonical, document); m_preview.live.sources.append({path, document->identity(), 1, snapshot, ReferenceIndexSource::Origin::Session});
    }
    for (auto &document : m_preview.plan.documents) {
        if (document.source.origin != ReferenceIndexSource::Origin::Session) continue;
        const auto live = m_open.value(document.from); if (!live) return false;
        const auto snapshot = live->session()->snapshot();
        // Later edits, even to another range, require a fresh manual plan.
        if (snapshot.utf8 != document.before.utf8) return false;
        document.before = snapshot;
        m_preview.baselines.insert(document.from, live->baseline());
    }
    m_recovering = true; return start(true);
}
void ResourceMaintenanceController::show(const QString &from) {
    if (m_busy || !m_window) return;
    auto *appearance = m_window->windowAppearance();
    const int width = visualTokens().value("windows").toObject().value("largeDialog").toObject().value("preferredWidth").toInt();
    SharedDialog dialog(appearance, m_window, width); dialog.setObjectName("resourceMaintenanceDialog"); dialog.setWindowTitle(QStringLiteral("移动或重命名资源"));
    auto *outer = new QVBoxLayout(dialog.surface()); outer->setContentsMargins(24, 24, 24, 24); outer->setSpacing(16);
    auto *scroll = new QScrollArea(dialog.surface()); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    const int bodyHeight = qBound(160, m_window->screen()->availableGeometry().height() - 200, 520);
    scroll->setMaximumHeight(bodyHeight); scroll->setMinimumHeight(qMin(400, bodyHeight)); scroll->setStyleSheet(themedScrollBars(appearance));
    auto *body = new QWidget; scroll->setWidget(body); outer->addWidget(scroll, 1);
    auto *layout = new QVBoxLayout(body); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(16);
    auto *title = new QLabel(QStringLiteral("移动或重命名资源"), dialog.surface()); layout->addWidget(title);
    auto *form = new QFormLayout; auto *source = new QLineEdit(from), *target = new QLineEdit;
    source->setObjectName("resourceMoveSource"); target->setObjectName("resourceMoveTarget");
    form->addRow(QStringLiteral("原位置"), source); form->addRow(QStringLiteral("新位置"), target); layout->addLayout(form);
    auto *description = new QLabel(QStringLiteral("使用工作区相对路径；目标父目录须已存在。先预览影响，再确认移动。")); description->setWordWrap(true); description->setTextFormat(Qt::PlainText); layout->addWidget(description);
    auto *externalMove = new QCheckBox(QStringLiteral("原位置已在应用外移动：仅把引用改到我确认的新位置")); layout->addWidget(externalMove);
    auto *tree = new QTreeWidget; tree->setHeaderLabels({QStringLiteral("文件／引用位置"), QStringLiteral("处理结果或新目标")}); tree->setMinimumHeight(200); tree->setMaximumHeight(280); tree->setObjectName("resourceMoveImpact"); layout->addWidget(tree);
    const auto fields = QStringLiteral("QLineEdit,QTreeWidget{background:%1;color:%2;border:1px solid %3;border-radius:6px} QLineEdit{padding:4px 8px} QTreeWidget{outline:0} QTreeWidget::item{padding:4px} QHeaderView::section{background:%1;color:%2;border:0;border-bottom:1px solid %3;padding:6px} QTreeWidget::item:selected{background:%4}")
        .arg(appearance->color("surface").name(), appearance->color("text").name(), appearance->color("border").name(), appearance->color("sidebar").name());
    source->setStyleSheet(fields); target->setStyleSheet(fields); tree->setStyleSheet(fields + themedScrollBars(appearance));
    auto *maintain = new QCheckBox(QStringLiteral("维护已识别的引用（打开文档保留未保存状态）")); maintain->setChecked(true); layout->addWidget(maintain);
    auto *ack = new QCheckBox(QStringLiteral("已了解未覆盖或跳过项目，以及仅移动可能造成的断链")); layout->addWidget(ack);
    auto *buttons = new QHBoxLayout; auto *preview = new SharedButton(QStringLiteral("预览影响"), "previewResourceMove", appearance);
    auto *restore = new SharedButton(QStringLiteral("恢复上次已完成项"), "recoverResourceMove", appearance); restore->setEnabled(canRecover());
    auto *cancel = new SharedButton(QStringLiteral("取消"), "cancelResourceMove", appearance);
    auto *apply = new SharedButton(QStringLiteral("确认移动"), "applyResourceMove", appearance); apply->setProperty("primary", true); apply->setEnabled(false);
    buttons->addWidget(preview); buttons->addWidget(restore); buttons->addStretch(); buttons->addWidget(cancel); buttons->addWidget(apply); outer->addLayout(buttons);
    if (!m_result.rows.isEmpty()) {
        description->setText(QStringLiteral("上次维护的逐项结果。可恢复已完成项，或填写路径后重新预览新的操作。"));
        for (const auto &row : m_result.rows) new QTreeWidgetItem(tree, {row.from + QStringLiteral(" → ") + row.to, row.message});
        if (!m_result.problem.isEmpty()) new QTreeWidgetItem(tree, {QStringLiteral("操作说明"), m_result.problem});
        if (!m_result.move.recoveryPath.isEmpty()) new QTreeWidgetItem(tree, {QStringLiteral("恢复位置"), m_result.move.recoveryPath});
        tree->resizeColumnToContents(0);
    }
    const auto invalidate = [&] { apply->setEnabled(false); };
    connect(source, &QLineEdit::textChanged, &dialog, invalidate); connect(target, &QLineEdit::textChanged, &dialog, invalidate);
    connect(externalMove, &QCheckBox::toggled, &dialog, invalidate);
    connect(preview, &QPushButton::clicked, &dialog, [&, this] { tree->clear(); description->setText(QStringLiteral("正在核验来源与影响…")); preview->setEnabled(false); source->setEnabled(false); target->setEnabled(false); externalMove->setEnabled(false); prepare(source->text(), target->text(), externalMove->isChecked()); });
    connect(this, &ResourceMaintenanceController::previewReady, &dialog, [&, this] {
        preview->setEnabled(true); source->setEnabled(true); target->setEnabled(true); externalMove->setEnabled(true); tree->clear(); const auto &plan = m_preview.plan;
        description->setText(plan.valid() ? plan.request.from + QStringLiteral(" → ") + plan.request.to : plan.problem);
        for (const auto &document : plan.documents) {
            auto *item = new QTreeWidgetItem(tree, {document.from, document.problem.isEmpty() ? document.source.origin == ReferenceIndexSource::Origin::Session ? QStringLiteral("更新会话，等待保存") : QStringLiteral("写入文件") : document.problem});
            for (const auto &edit : document.edits) new QTreeWidgetItem(item, {QStringLiteral("第 %1 行：%2").arg(document.before.utf8.left(edit.range.start).count('\n') + 1).arg(edit.originalTarget), edit.newTarget});
        }
        for (const auto &gap : plan.gaps) new QTreeWidgetItem(tree, {QStringLiteral("未覆盖／跳过"), gap});
        tree->expandAll(); tree->resizeColumnToContents(0); apply->setEnabled(plan.valid());
    });
    connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(&dialog, &QDialog::rejected, this, [this] { if (m_prepare.isRunning()) this->cancel(); });
    connect(apply, &QPushButton::clicked, &dialog, [&, this] {
        if ((!maintain->isChecked() || !m_preview.plan.gaps.isEmpty()) && !ack->isChecked()) { description->setText(QStringLiteral("请先确认已了解未覆盖项目及断链影响。")); return; }
        if (this->apply(maintain->isChecked())) dialog.accept();
        else { apply->setEnabled(false); description->setText(m_result.problem + QStringLiteral(" 请重新预览。")); }
    });
    connect(restore, &QPushButton::clicked, &dialog, [&, this] { if (recover()) dialog.accept(); else description->setText(QStringLiteral("恢复基线已变化，请检查当前文件后重新预览反向移动。")); });
    dialog.exec();
}
}
