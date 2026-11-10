#include "WorkspaceReferenceController.h"
#include "FileManager.h"
#include <QDir>
#include <QScopedValueRollback>
#include <QtConcurrentRun>

namespace xdnote {
WorkspaceReferenceController::WorkspaceReferenceController(FileManager *manager, QObject *parent) : QObject(parent) {
    m_debounce.setSingleShot(true); m_debounce.setInterval(180);
    connect(&m_debounce, &QTimer::timeout, this, &WorkspaceReferenceController::start);
    connect(&m_watch, &QFileSystemWatcher::directoryChanged, this, [this] { invalidate(); });
    connect(&m_watch, &QFileSystemWatcher::fileChanged, this, [this] { invalidate(); });
    connect(&m_worker, &QFutureWatcher<ReferenceScanResult>::finished, this, [this] {
        auto result = m_worker.result();
        if (m_root && m_root->active() && !m_paused && m_runningGeneration == m_generation && !result.cancelled) {
            QStringList watches;
            for (const auto &path : result.watchedDirectories) watches.append(QDir(m_path).filePath(path));
            for (const auto &source : result.index.sources) watches.append(QDir(m_path).filePath(source.path));
            watches.removeDuplicates();
            const auto previous = m_watch.directories() + m_watch.files();
            if (!previous.isEmpty()) m_watch.removePaths(previous);
            if (!watches.isEmpty() && !m_watch.addPaths(watches).isEmpty()) {
                result.index.complete = false;
                result.index.gaps.append(QStringLiteral("部分位置无法监测变化，请手动重新检查"));
            }
            m_result = std::move(result); m_ready = true; emit changed();
        }
        if (m_pending && !m_paused) m_debounce.start();
    });
    if (manager) {
        connect(manager, &FileManager::documentsChanged, this, [this, manager] { setDocuments(manager->documents()); });
        setDocuments(manager->documents());
    }
}
WorkspaceReferenceController::~WorkspaceReferenceController() { if (m_cancelled) m_cancelled->store(true, std::memory_order_release); }
void WorkspaceReferenceController::setRoot(std::shared_ptr<WorkspaceResourceReader> root, const QString &path) {
    if (m_cancelled) m_cancelled->store(true, std::memory_order_release);
    const auto watches = m_watch.directories() + m_watch.files(); if (!watches.isEmpty()) m_watch.removePaths(watches);
    m_root = std::move(root); m_path = path; m_rootId = QUuid::createUuid(); ++m_rootGeneration;
    m_paused = false; m_documentStates.clear(); m_documentPaths.clear(); m_pathVersions.clear(); observeDocuments(); invalidate();
    if (m_root && m_root->active()) m_watch.addPath(m_path);
}
QString WorkspaceReferenceController::relativePath(const QString &path) const {
    if (m_path.isEmpty() || path.isEmpty()) return {};
    const auto relative = QDir(m_path).relativeFilePath(path);
    return resolveWorkspaceTarget(relative, QStringLiteral("#")).success ? relative : QString();
}
void WorkspaceReferenceController::setDocuments(const QList<FileDocument *> &documents) {
    for (const auto &connection : m_connections) disconnect(connection);
    m_connections.clear(); m_documents.clear();
    for (auto *document : documents) {
        if (!document || document->closed()) continue;
        m_documents.append(document);
        const auto observe = [this] { observeDocuments(); };
        m_connections.append(connect(document, &FileDocument::stateChanged, this, observe));
        m_connections.append(connect(document->session(), &DocumentSession::changed, this, observe));
        m_connections.append(connect(document->session(), &DocumentSession::stateChanged, this, observe));
        m_connections.append(connect(document, &QObject::destroyed, this, observe));
    }
    observeDocuments();
}
void WorkspaceReferenceController::observeDocuments() {
    if (m_observing) return;
    QScopedValueRollback<bool> observing(m_observing, true);
    QHash<QUuid, QString> states;
    for (auto document : m_documents) {
        if (!document || document->closed()) continue;
        auto *session = document->session(); const auto id = session->documentId();
        const auto path = document->path() + '\n' + document->identity();
        if (m_documentPaths.value(id) != path) { m_documentPaths[id] = path; m_pathVersions[id] = ++m_pathGeneration; }
        if (relativePath(document->path()).isEmpty()) { states[id] = path; continue; }
        states[id] = path + QStringLiteral("\n%1:%2").arg(session->revision()).arg(session->isComposing());
    }
    for (auto it = m_documentPaths.begin(); it != m_documentPaths.end();) {
        if (!states.contains(it.key())) { m_pathVersions.remove(it.key()); it = m_documentPaths.erase(it); } else ++it;
    }
    if (states != m_documentStates) { m_documentStates = std::move(states); invalidate(); }
}
void WorkspaceReferenceController::invalidate() {
    ++m_generation; m_ready = false; m_result = {};
    if (m_cancelled) m_cancelled->store(true, std::memory_order_release);
    m_pending = !m_paused && m_root && m_root->active();
    if (m_pending) m_debounce.start(); else m_debounce.stop();
    emit changed();
}
void WorkspaceReferenceController::refresh() { m_paused = false; observeDocuments(); invalidate(); }
void WorkspaceReferenceController::cancel() { m_paused = true; invalidate(); }
QString WorkspaceReferenceController::statusText() const {
    if (m_path.isEmpty()) return QStringLiteral("请先打开工作区目录");
    if (!m_root) return QStringLiteral("此平台暂不支持受控引用检查");
    if (!m_root->active()) return QStringLiteral("工作区授权已失效，请重新打开目录");
    if (m_paused) return QStringLiteral("引用检查已取消");
    if (!m_ready) return QStringLiteral("正在检查工作区引用…");
    return m_result.index.complete ? QStringLiteral("已检查 %1 篇文档、%2 处引用").arg(m_result.index.sources.size()).arg(m_result.index.references.size())
        : QStringLiteral("检查不完整：仅显示已确认的引用");
}
void WorkspaceReferenceController::start() {
    if (!m_pending || m_paused || !m_root || !m_root->active() || m_worker.isRunning()) return;
    ReferenceIndexInput input{m_rootId, m_rootGeneration, m_generation, false, {}, {}};
    qsizetype capturedBytes = 0;
    const auto overLimit = [this] {
        m_pending = false; m_ready = true; m_result = {};
        m_result.index.gaps.append(QStringLiteral("打开会话数量或文本超过引用检查上限")); emit changed();
    };
    if (m_documents.size() > 1024) { overLimit(); return; }
    for (auto document : m_documents) {
        if (!document || document->closed()) continue;
        const auto path = relativePath(document->path());
        if (path.isEmpty()) { input.sessionsOutsideRoot.insert(document->identity()); continue; }
        const auto id = document->session()->documentId();
        if (!m_pathVersions.contains(id)) m_pathVersions[id] = ++m_pathGeneration;
        auto snapshot = document->session()->snapshot(); capturedBytes += snapshot.utf8.size();
        if (snapshot.result == EditResult::Busy) { m_debounce.start(); return; }
        if (capturedBytes > 32 * 1024 * 1024) { overLimit(); return; }
        input.sources.append({path, document->identity(), m_pathVersions.value(id), std::move(snapshot), ReferenceIndexSource::Origin::Session});
    }
    // Capturing a snapshot can flush pending edits and invalidate this request.
    if (input.scanGeneration != m_generation) { m_debounce.start(); return; }
    m_pending = false; m_runningGeneration = m_generation;
    m_cancelled = std::make_shared<std::atomic_bool>(false);
    const auto root = m_root; const auto cancelled = m_cancelled;
    m_worker.setFuture(QtConcurrent::run([root, input = std::move(input), cancelled] { return scanWorkspaceReferences(root, input, {}, cancelled); }));
}
}
