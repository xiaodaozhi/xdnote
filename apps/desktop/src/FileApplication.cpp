#include "FileApplication.h"
#include "SingleFileWindow.h"
#include "WorkspaceWindow.h"
#include "WorkspaceTree.h"
#include "SourceEditor.h"
#include "MarkdownPreview.h"
#include "InputModality.h"
#include "WorkspaceResourceReader.h"
#include "WorkspacePath.h"
#include "WorkspaceReferenceController.h"
#include <QApplication>
#include <QFileOpenEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>

namespace xdnote {
namespace {
QString rootKey(const QString &path) { const QFileInfo info(path); auto key = info.canonicalFilePath(); if (key.isEmpty()) key = info.absoluteFilePath();
#ifdef Q_OS_WIN
    key = key.toCaseFolded();
#endif
    return QDir::cleanPath(key);
}
}
FileApplication::FileApplication(QObject *parent, QString settingsPath)
    : QObject(parent), m_appearance(this, settingsPath), m_geometryStore(this, settingsPath.isEmpty() ? QString() : settingsPath + ".windows"), m_manager(this) {
    qApp->installEventFilter(this);
    connect(&m_manager, &FileManager::resourcesChanged, this, [this] {
        for (auto pane : m_documents) if (pane && pane->preview()) pane->preview()->refresh();
    });
    connect(&m_manager, &FileManager::opened, this, [this](QUuid intent, FileDocument *document, bool reused) {
        auto placeholder = m_requests.take(intent);
        const auto navigation = m_linkNavigations.take(intent);
        if (navigation.root && !validLinkSource(navigation)) {
            if (!reused) m_manager.closeDocument(document);
            if (placeholder) { placeholder->finishOpen(); placeholder->close(); }
            if (navigation.source) navigation.source->reportHostMessage(QStringLiteral("链接来源或工作区已变化，本次打开已取消。"));
            return;
        }
        if (!placeholder) { if (!reused) m_manager.closeDocument(document); return; }
        const auto id = document->session()->documentId(); auto pane = m_documents.value(id);
        if (pane) { placeholder->finishOpen(); if (pane != placeholder) placeholder->close(); activatePane(pane); if (navigation.hasFragment) pane->navigateToHeading(navigation.fragment); if (navigation.sourceLocation) pane->navigateToReference(navigation.locationIdentity, navigation.sourceSha256, navigation.locationStart, navigation.locationEnd); return; }
        m_documents.insert(id, placeholder); placeholder->setDocument(document);
        connect(placeholder, &QObject::destroyed, this, [this, id] { m_documents.remove(id); });
        connect(document, &FileDocument::stateChanged, this, [this] { QTimer::singleShot(0, this, &FileApplication::retryTransfers); });
        connect(document->session(), &DocumentSession::stateChanged, this, [this] { QTimer::singleShot(0, this, &FileApplication::retryTransfers); }); activatePane(placeholder);
        if (navigation.hasFragment) placeholder->navigateToHeading(navigation.fragment);
        if (navigation.sourceLocation) placeholder->navigateToReference(navigation.locationIdentity, navigation.sourceSha256, navigation.locationStart, navigation.locationEnd);
    });
    connect(&m_manager, &FileManager::openFailed, this, [this](QUuid intent, const QString &path, const QString &message) {
        auto pane = m_requests.take(intent); const auto navigation = m_linkNavigations.take(intent);
        if (navigation.root) {
            if (pane) { pane->finishOpen(); pane->close(); }
            if (navigation.source) { navigation.source->reportHostMessage(QStringLiteral("无法打开文档链接：") + message); activatePane(navigation.source); }
        } else if (pane) pane->showOpenError(path, message);
    });
}
FileApplication::~FileApplication() {
    m_destroying = true; qApp->removeEventFilter(this);
    QList<QPointer<WorkspaceWindow>> workspaces; for (auto *workspace : m_workspaces) workspaces.append(workspace);
    QList<QPointer<SingleFileWindow>> panes; for (auto *pane : m_windows) panes.append(pane);
    for (auto workspace : workspaces) if (workspace) delete workspace;
    for (auto pane : panes) if (pane) delete pane;
}
SingleFileWindow *FileApplication::createWindow() {
    auto *pane = new SingleFileWindow(&m_manager, &m_appearance, &m_geometryStore); m_windows.insert(pane);
    connect(pane, &SingleFileWindow::copyPathRequested, this, [](const QString &path) { QGuiApplication::clipboard()->setText(path); });
    connect(pane, &SingleFileWindow::copyTextRequested, this, [](const QString &text) { QGuiApplication::clipboard()->setText(text); });
    connect(pane, &SingleFileWindow::revealRequested, this, [](const QString &path) { QDesktopServices::openUrl(QUrl::fromLocalFile(path)); });
    connect(pane, &SingleFileWindow::openRequested, this, [this, pane](const QString &path) { if (auto *workspace = qobject_cast<WorkspaceWindow *>(pane->window())) openInWorkspace(path, workspace); else open(path, pane->document() ? nullptr : pane); });
    connect(pane, &SingleFileWindow::workspaceRequested, this, [this, pane] { transferToWorkspace(pane); });
    connect(pane, &SingleFileWindow::openDirectoryRequested, this, [this, pane] { chooseDirectory(qobject_cast<WorkspaceWindow *>(pane->window())); });
    connect(pane, &SingleFileWindow::cancelOpenRequested, this, [this](QUuid intent) { m_requests.remove(intent); m_linkNavigations.remove(intent); m_manager.cancelOpen(intent); });
    connect(pane, &SingleFileWindow::documentLinkRequested, this, [this, pane](const QString &target, quint64 revision) { openDocumentLink(pane, target, revision); });
    connect(pane, &SingleFileWindow::quitRequested, this, &FileApplication::requestQuit); connect(pane, &SingleFileWindow::closeCancelled, this, [this] { m_quitting = false; });
    connect(pane, &QObject::destroyed, this, [this, pane, id = pane->paneId()] { m_windows.remove(pane); m_transfers.remove(id); if (m_destroying) return; if (m_windows.isEmpty() && m_workspaces.isEmpty()) QCoreApplication::quit(); else if (m_quitting) QTimer::singleShot(0, this, &FileApplication::advanceQuit); }); return pane;
}
WorkspaceWindow *FileApplication::createWorkspace() {
    auto *workspace = new WorkspaceWindow(&m_appearance, &m_geometryStore, &m_manager); m_workspaces.insert(workspace);
    connect(workspace, &WorkspaceWindow::referenceSourceRequested, this, [this, workspace](const QString &path, const QString &identity, const QByteArray &sha, qsizetype start, qsizetype end) {
        LinkNavigation navigation; navigation.sourceLocation = true; navigation.workspace = workspace; navigation.root = workspace->resourceRoot();
        navigation.source = workspace->activePane(); navigation.sourceSha256 = sha; navigation.locationIdentity = identity; navigation.locationStart = start; navigation.locationEnd = end;
        if (!validLinkSource(navigation)) return;
        auto *placeholder = createWindow(); const auto intent = QUuid::createUuid(); placeholder->setLoading(QDir(navigation.root->rootPath()).filePath(path), intent);
        if (!workspace->addPane(placeholder)) { delete placeholder; return; }
        m_requests.insert(intent, placeholder); m_linkNavigations.insert(intent, navigation); m_manager.openWithin(navigation.root, path, intent);
    });
    connect(workspace, &WorkspaceWindow::openRequested, this, [this, workspace](const QString &path) { openInWorkspace(path, workspace); });
    connect(workspace, &WorkspaceWindow::chooseFileRequested, this, [this, workspace] { const auto path = QFileDialog::getOpenFileName(workspace, QStringLiteral("打开 Markdown 文件"), workspace->rootPath(), QStringLiteral("Markdown 文档 (*.md *.markdown);;所有文件 (*)")); InputModality::forApplication()->clear(); if (!path.isEmpty()) openInWorkspace(path, workspace); });
    connect(workspace, &WorkspaceWindow::chooseDirectoryRequested, this, [this, workspace] { chooseDirectory(workspace); });
    connect(workspace, &WorkspaceWindow::closeCancelled, this, [this] { m_quitting = false; });
    connect(workspace, &WorkspaceWindow::quitRequested, this, &FileApplication::requestQuit);
    connect(workspace, &QObject::destroyed, this, [this, workspace] { m_workspaces.remove(workspace); if (m_destroying) return; if (m_windows.isEmpty() && m_workspaces.isEmpty()) QCoreApplication::quit(); else if (m_quitting) QTimer::singleShot(0, this, &FileApplication::advanceQuit); }); return workspace;
}
WorkspaceWindow *FileApplication::workspaceWindow(const QString &root) {
    for (auto *workspace : m_workspaces) if (!workspace->closingWorkspace() && ((root.isEmpty() && workspace->rootPath().isEmpty()) || (!root.isEmpty() && !workspace->rootPath().isEmpty() && rootKey(root) == rootKey(workspace->rootPath())))) { workspace->show(); workspace->raise(); workspace->activateWindow(); return workspace; }
    auto *workspace = createWorkspace(); workspace->setRootPath(root); workspace->show(); return workspace;
}
void FileApplication::chooseDirectory(WorkspaceWindow *owner) {
    const auto root = QFileDialog::getExistingDirectory(owner, QStringLiteral("打开工作区目录"), owner ? owner->rootPath() : QString()); InputModality::forApplication()->clear(); if (root.isEmpty()) return;
    if (owner && owner->rootPath().isEmpty() && !owner->closingWorkspace()) owner->setRootPath(root); else workspaceWindow(root);
}
SingleFileWindow *FileApplication::emptyWindow() { auto *pane = createWindow(); pane->show(); return pane; }
void FileApplication::open(const QString &path, SingleFileWindow *empty) {
    m_quitting = false; if (empty && empty->embedded()) { openInWorkspace(path, qobject_cast<WorkspaceWindow *>(empty->window())); return; }
    auto *pane = empty && !empty->document() ? empty : createWindow();
    for (auto it = m_requests.begin(); it != m_requests.end();) { if (it.value() == pane) { m_manager.cancelOpen(it.key()); it = m_requests.erase(it); } else ++it; }
    const auto intent = QUuid::createUuid(); m_requests.insert(intent, pane); pane->setLoading(path, intent); pane->show(); m_manager.open(path, intent);
}
void FileApplication::openInWorkspace(const QString &path, WorkspaceWindow *workspace) {
    if (!workspace || workspace->closingWorkspace()) return; m_quitting = false;
    auto *pane = createWindow(); const auto intent = QUuid::createUuid(); pane->setLoading(path, intent);
    if (!workspace->addPane(pane)) { delete pane; workspace->showNotice(QStringLiteral("无法准备文档标签，原有文档保持不变。")); return; }
    m_requests.insert(intent, pane); m_manager.open(path, intent);
}
void FileApplication::activatePane(SingleFileWindow *pane) { if (auto *workspace = qobject_cast<WorkspaceWindow *>(pane->window())) workspace->activatePane(pane->paneId()); else { if (pane->isMinimized()) pane->showNormal(); pane->show(); pane->raise(); pane->activateWindow(); } }
bool FileApplication::validLinkSource(const LinkNavigation &navigation) const {
    if (navigation.sourceLocation) return navigation.workspace && !navigation.workspace->closingWorkspace()
        && navigation.root && navigation.root->active() && navigation.workspace->resourceRoot() == navigation.root;
    const auto source = navigation.source;
    if (!source || !navigation.root || !navigation.root->active() || !source->document() || source->document()->closed()
        || source->resourceRoot() != navigation.root || source->resourceDocumentPath() != navigation.sourcePath
        || source->document()->identity() != navigation.sourceIdentity || source->embedded() != navigation.embedded) return false;
    if (navigation.embedded && (!navigation.workspace || navigation.workspace->closingWorkspace()
        || source->window() != navigation.workspace || navigation.workspace->resourceRoot() != navigation.root)) return false;
    const auto snapshot = source->document()->session()->snapshot();
    return snapshot.result == EditResult::Applied && snapshot.revision == navigation.revision;
}
void FileApplication::openDocumentLink(SingleFileWindow *source, const QString &target, quint64 revision) {
    if (!source || !source->document() || source->document()->closed()) return;
    LinkNavigation navigation; navigation.source = source; navigation.workspace = qobject_cast<WorkspaceWindow *>(source->window());
    navigation.root = source->resourceRoot(); navigation.sourcePath = source->resourceDocumentPath();
    navigation.sourceIdentity = source->document()->identity(); navigation.revision = revision; navigation.embedded = source->embedded();
    if (!navigation.root) {
#if !defined(Q_OS_WIN) && !defined(Q_OS_LINUX)
        source->reportHostMessage(QStringLiteral("此平台暂不支持受控跨文档链接打开。"));
#else
        source->reportHostMessage(QStringLiteral("此文档没有可用的目录授权，请先打开所属工作区或重新打开文档。"));
#endif
        return;
    }
    if (!validLinkSource(navigation)) { source->reportHostMessage(QStringLiteral("文档或目录授权不可用，请完成输入或重新打开目录后重试。")); return; }
    const auto resolved = resolveWorkspaceTarget(navigation.sourcePath, target);
    if (!resolved.success) { source->reportHostMessage(QStringLiteral("此文档链接超出当前授权范围，或不是支持的相对路径。")); return; }
    const auto suffix = QFileInfo(resolved.path).suffix().toLower();
    if (suffix != "md" && suffix != "markdown") { source->reportHostMessage(QStringLiteral("此入口仅打开 Markdown 文档，不执行其他文件。")); return; }
    navigation.fragment = resolved.fragment; navigation.hasFragment = resolved.hasFragment && !resolved.fragment.isEmpty();
    m_quitting = false;
    auto *placeholder = createWindow(); const auto intent = QUuid::createUuid();
    placeholder->setLoading(QDir(navigation.root->rootPath()).filePath(resolved.path), intent);
    if (navigation.workspace) {
        if (!navigation.workspace->addPane(placeholder)) { delete placeholder; source->reportHostMessage(QStringLiteral("无法准备目标文档标签。")); return; }
    } else placeholder->show();
    m_requests.insert(intent, placeholder); m_linkNavigations.insert(intent, navigation);
    m_manager.openWithin(navigation.root, resolved.path, intent, navigation.sourcePath, navigation.sourceIdentity);
}
bool FileApplication::transferToWorkspace(SingleFileWindow *pane, WorkspaceWindow *target) {
    if (!pane || !pane->document() || pane->document()->closed()) return false;
    if (pane->embedded()) { activatePane(pane); return true; }
    if (target && target->closingWorkspace()) { m_transfers.remove(pane->paneId()); pane->reportHostMessage(QStringLiteral("工作区正在关闭，文档仍保留在独立窗口。")); return false; }
    if (!pane->canTransfer()) {
        const auto *document = pane->document();
        const bool pending = document->saving() || document->loading() || document->session()->isBusy() || document->session()->isComposing() || (pane->editor() && pane->editor()->dispatching());
        if (pending) { m_transfers.insert(pane->paneId(), {pane, target, target != nullptr}); pane->reportHostMessage(QStringLiteral("当前输入或文件操作结束后，将在工作区继续处理。")); }
        else pane->reportHostMessage(QStringLiteral("请先完成当前对话框，再选择在工作区打开。"));
        return false;
    }
    m_transfers.remove(pane->paneId());
    if (!target) {
        auto *active = qobject_cast<WorkspaceWindow *>(QApplication::activeWindow()); if (active && m_workspaces.contains(active) && !active->closingWorkspace()) target = active;
        if (!target) for (auto *workspace : m_workspaces) if (!workspace->closingWorkspace() && workspace->tree()->acceptsFile(pane->document()->path())) { target = workspace; break; }
        if (!target) target = workspaceWindow();
    }
    if (!target->addPane(pane)) { pane->reportHostMessage(QStringLiteral("工作区未能接管文档，独立窗口与编辑内容已保留。")); return false; }
    pane->reportHostMessage({}); return true;
}
void FileApplication::retryTransfers() {
    const auto transfers = m_transfers;
    for (auto it = transfers.begin(); it != transfers.end(); ++it) { const auto pending = it.value(); if (!pending.pane) { m_transfers.remove(it.key()); continue; } if (pending.explicitTarget && !pending.target) { m_transfers.remove(it.key()); pending.pane->reportHostMessage(QStringLiteral("目标工作区已关闭，文档仍保留在独立窗口。")); } else if (pending.pane->canTransfer()) transferToWorkspace(pending.pane, pending.target); }
}
void FileApplication::activate() { for (auto *workspace : m_workspaces) { if (workspace->isMinimized()) workspace->showNormal(); workspace->raise(); workspace->activateWindow(); return; } if (!m_windows.isEmpty()) activatePane(*m_windows.begin()); else workspaceWindow(); }
void FileApplication::requestQuit() { if (!m_quitting) { m_quitting = true; advanceQuit(); } }
void FileApplication::advanceQuit() { if (!m_quitting) return; for (auto *workspace : m_workspaces) if (workspace->isVisible()) { workspace->close(); return; } for (auto *pane : m_windows) if (pane->isWindow() && pane->isVisible()) { pane->close(); return; } if (m_windows.isEmpty() && m_workspaces.isEmpty()) QCoreApplication::quit(); }
bool FileApplication::eventFilter(QObject *target, QEvent *event) {
    if (target == qApp && event->type() == QEvent::FileOpen) { const auto *file = static_cast<QFileOpenEvent *>(event); if (!file->file().isEmpty()) open(file->file()); else if (file->url().isLocalFile()) open(file->url().toLocalFile()); return true; }
    if (target == qApp && event->type() == QEvent::Quit && (!m_windows.isEmpty() || !m_workspaces.isEmpty())) { requestQuit(); return true; }
    return QObject::eventFilter(target, event);
}
}
