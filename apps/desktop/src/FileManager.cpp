#include "FileManager.h"
#include "WorkspaceResourceReader.h"
#include <QDir>
#include <QFileInfo>

namespace xdnote {
namespace {
ReadResult readPath(const QString &path, const QUuid &operation) {
    auto probe = FileService::probe(path);
    return probe.file ? FileService::read({probe.file, operation}) : ReadResult{{}, probe.problem};
}
}
FileDocument::FileDocument(FileManager *manager, DocumentSession *session, std::shared_ptr<const FileBaseline> baseline)
    : QObject(manager), m_manager(manager), m_session(session), m_baseline(std::move(baseline)) {
    permissions();
    connect(session, &DocumentSession::changed, this, [this] { emit stateChanged(); });
    connect(session, &DocumentSession::stateChanged, this, [this] { permissions(); emit stateChanged(); });
    connect(session, &DocumentSession::closed, this, &QObject::deleteLater);
}
void FileDocument::permissions() {
    m_readOnly = !QFileInfo(path()).isWritable();
    if (m_session && !m_session->isClosing() && !m_session->isBusy() && !m_session->isComposing()
        && m_session->isReadOnly() != m_readOnly) m_session->setReadOnly(m_readOnly);
}
QString FileDocument::statusText() const {
    if (m_saving) return QStringLiteral("保存中…");
    if (m_loading) return QStringLiteral("重新载入中…");
    if (m_conflict) return QStringLiteral("磁盘内容已变化");
    if (m_error) return QStringLiteral("保存或读取失败，内容已保留");
    if (dirty()) return QStringLiteral("未保存");
    if (m_readOnly) return QStringLiteral("只读");
    return m_savedOnce ? QStringLiteral("已保存") : QStringLiteral("未修改");
}
bool FileDocument::save() { return m_manager->save(this, path()); }
bool FileDocument::saveAs(const QString &path) { return m_manager->save(this, path, true); }
bool FileDocument::reload() { return m_manager->reload(this); }
void FileDocument::checkExternal() { m_manager->check(this); }

FileManager::FileManager(QObject *parent, std::function<void(SaveStage)> observer, bool monitoring)
    : QObject(parent), m_registry(this), m_observer(std::move(observer)) {
    m_poll.setInterval(2000);
    connect(&m_poll, &QTimer::timeout, this, [this] {
        for (auto document : m_documents) if (document) check(document);
    });
    if (monitoring) m_poll.start();
}
void FileManager::enqueue(std::function<void()> job) { m_jobs.push_back(std::move(job)); pump(); }
void FileManager::pump() {
    if (m_running || m_jobs.empty()) return;
    m_running = true;
    auto job = std::move(m_jobs.front()); m_jobs.pop_front(); job();
}
void FileManager::finish() { m_running = false; QTimer::singleShot(0, this, &FileManager::pump); }

void FileManager::open(const QString &input, const QUuid &intent) {
    const auto path = QDir::isAbsolutePath(input) ? input : QDir::currentPath() + '/' + input;
    openRequest(path, intent);
}
void FileManager::openWithin(const std::shared_ptr<WorkspaceResourceReader> &root, const QString &path, const QUuid &intent, const QString &sourcePath, const QString &sourceIdentity) {
    if (!root) { emit openFailed(intent, path, QStringLiteral("当前入口没有目录授权")); return; }
    openRequest(QDir(root->rootPath()).filePath(path), intent, root, path, sourcePath, sourceIdentity);
}
QList<FileDocument *> FileManager::documents() const {
    QList<FileDocument *> result;
    for (auto document : m_documents) if (document && !document->closed()) result.append(document);
    return result;
}
void FileManager::openRequest(const QString &path, const QUuid &intent, std::shared_ptr<WorkspaceResourceReader> root, QString relativePath, QString sourcePath, QString sourceIdentity) {
    enqueue([this, path, intent, root, relativePath, sourcePath, sourceIdentity] {
        if (m_cancelled.remove(intent)) { finish(); return; }
        worker<ProbeResult>([path, root, relativePath, sourcePath, sourceIdentity] {
            if (root && !sourcePath.isEmpty()) {
                const auto source = root->inspect(sourcePath);
                if (!source.ok() || source.identity != sourceIdentity) return ProbeResult{{}, {FileError::Changed}};
            }
            return root ? FileService::probeWithin(root, relativePath) : FileService::probe(path);
        }, [this, path, intent, root](ProbeResult probe) {
            if (m_cancelled.remove(intent)) { finish(); return; }
            if (root && !root->active()) { emit openFailed(intent, path, QStringLiteral("工作区授权已失效")); finish(); return; }
            if (!probe.file) { emit openFailed(intent, path, probe.problem.description()); finish(); return; }
            auto opening = m_registry.beginOpen(probe.file);
            if (opening.result == RegistryResult::Reused) {
                auto document = m_documents.value(opening.session->documentId());
                if (document && !document->closed()) emit opened(intent, document, true);
                else emit openFailed(intent, path, QStringLiteral("文档正在关闭，请稍后重试"));
                finish(); return;
            }
            if (!opening.startRead) { emit openFailed(intent, path, QStringLiteral("文档正在打开或关闭，请稍后重试")); finish(); return; }
            const auto request = opening.read;
            worker<ReadResult>([request] { return FileService::read(request); }, [this, path, intent, request, root](ReadResult loaded) {
                if (m_cancelled.remove(intent)) { m_registry.cancel(request); finish(); return; }
                if (root && !root->active()) { m_registry.cancel(request); emit openFailed(intent, path, QStringLiteral("工作区授权已失效")); finish(); return; }
                if (!loaded.baseline) {
                    m_registry.cancel(request); emit openFailed(intent, path, loaded.problem.description()); finish(); return;
                }
                auto registered = m_registry.complete(request, loaded.baseline);
                if (!registered.session) { emit openFailed(intent, path, QStringLiteral("打开请求已失效，请重试")); finish(); return; }
                auto *document = new FileDocument(this, registered.session, loaded.baseline);
                const auto id = registered.session->documentId();
                m_documents.insert(id, document);
                connect(document, &QObject::destroyed, this, [this, id] { m_documents.remove(id); emit documentsChanged(); });
                emit documentsChanged();
                emit opened(intent, document, false);
                finish();
            });
        });
    });
}

bool FileManager::save(FileDocument *document, const QString &path, bool saveAs) {
    if (!document || document->closed() || document->saving() || document->loading() || !document->session()) return false;
    const bool original = !saveAs && path == document->path();
    if (original && document->conflict()) {
        document->m_message = QStringLiteral("磁盘版本已变化，请选择保留编辑、重新载入或另存为");
        emit document->stateChanged(); return false;
    }
    const auto snapshot = document->session()->snapshot();
    if (snapshot.result != EditResult::Applied) {
        document->m_error = true; document->m_message = resultDescription(snapshot.result); emit document->stateChanged(); return false;
    }
    const QPointer<FileDocument> guard(document);
    const auto source = document->m_baseline;
    const auto operation = QUuid::createUuid();
    document->m_operation = operation;
    document->m_saving = true;
    document->m_error = false;
    document->m_message.clear();
    emit document->stateChanged();
    enqueue([this, guard, snapshot, path, source, operation, original] {
        if (!guard || guard->closed()) { finish(); return; }
        if (original) { performSave(guard, snapshot, path, source, operation); return; }
        worker<ProbeResult>([path] { return FileService::probe(path); }, [this, guard, snapshot, path, source, operation](ProbeResult target) {
            if (!guard || guard->closed()) { finish(); return; }
            if (target.file) {
                auto existing = m_registry.lookup(target.file);
                if (existing.session && existing.session != guard->session()) {
                    guard->m_saving = false; guard->m_error = true;
                    guard->m_message = QStringLiteral("目标文件已在另一窗口打开，请切换该窗口处理");
                    emit guard->stateChanged(); emit guard->operationFinished(false); finish(); return;
                }
                if (target.file->identity() == source->file()->identity()) { performSave(guard, snapshot, path, source, operation); return; }
                const FileReadRequest readRequest{target.file, operation};
                worker<ReadResult>([readRequest] { return FileService::read(readRequest); }, [this, guard, snapshot, path, operation](ReadResult read) {
                    if (!guard || guard->closed()) { finish(); return; }
                    if (read.baseline) { performSave(guard, snapshot, path, read.baseline, operation); return; }
                    guard->m_saving = false; guard->m_error = true; guard->m_message = read.problem.description();
                    emit guard->stateChanged(); emit guard->operationFinished(false); finish();
                });
                return;
            }
            if (target.problem.code == FileError::Missing) { performSave(guard, snapshot, path, {}, operation); return; }
            guard->m_saving = false; guard->m_error = true; guard->m_message = target.problem.description();
            emit guard->stateChanged(); emit guard->operationFinished(false); finish();
        });
    });
    return true;
}

void FileManager::performSave(QPointer<FileDocument> document, DocumentSnapshot snapshot, QString path,
                              std::shared_ptr<const FileBaseline> target, QUuid operation) {
    if (!document || document->closed()) { finish(); return; }
    FileSaveRequest request;
    request.path = path; request.baseline = std::move(target); request.utf8 = snapshot.utf8;
    request.bom = document->m_baseline->hasUtf8Bom(); request.documentId = snapshot.documentId;
    request.revision = snapshot.revision; request.requestId = operation;
    const auto observer = m_observer;
    worker<FileSaveResult>([request, observer] { return FileService::save(request, observer); },
        [this, document, snapshot, operation, path](FileSaveResult result) {
        if (!document || document->closed() || document->m_operation != operation || result.documentId != snapshot.documentId
            || result.requestId != operation) { finish(); return; }
        document->m_lastSave = result;
        for (const auto &path : {result.recoveryPath, result.stagedPath})
            if (!path.isEmpty() && !document->m_recoveries.contains(path)) document->m_recoveries.append(path);
        document->m_saving = false;
        if (result.status == SaveStatus::Committed && result.baseline
            && m_registry.rebind(document->session(), result.baseline) == RegistryResult::Reused) {
            document->m_baseline = result.baseline;
            document->m_savedOnce = true; document->m_conflict = false; document->m_error = false; document->m_message.clear();
            document->session()->confirmSaved(snapshot);
            document->permissions();
            if (!document->m_recoveries.isEmpty()) document->m_message = QStringLiteral("先前保存操作的恢复副本仍保留：\n") + document->m_recoveries.join('\n');
        } else {
            const bool original = path == document->path();
            document->m_conflict = document->m_conflict || (original && (result.status == SaveStatus::Conflict || result.status == SaveStatus::Uncertain));
            document->m_error = true;
            document->m_message = result.status == SaveStatus::Uncertain || result.status == SaveStatus::Committed
                ? QStringLiteral("保存结果未能确认，当前编辑已保留。请检查磁盘并重新载入或另存为。") : result.problem.description();
            if (!result.recoveryPath.isEmpty()) document->m_message += QStringLiteral("\n磁盘版本保留位置：") + result.recoveryPath;
            if (!result.stagedPath.isEmpty() && result.stagedPath != result.recoveryPath)
                document->m_message += QStringLiteral("\n暂存位置：") + result.stagedPath;
        }
        emit document->stateChanged();
        emit document->saveFinished(result.status);
        emit document->operationFinished(result.status == SaveStatus::Committed && !document->m_error);
        finish();
    });
}

bool FileManager::reload(FileDocument *document) {
    if (!document || document->closed() || document->saving() || document->loading() || !document->session()) return false;
    if (document->session()->isComposing() || document->session()->isBusy()) {
        document->m_message = QStringLiteral("请先完成当前输入或编辑操作"); document->m_error = true;
        emit document->stateChanged(); return false;
    }
    const auto revision = document->session()->revision();
    const auto path = document->path(), operation = QUuid::createUuid().toString();
    const QPointer<FileDocument> guard(document);
    document->m_loading = true;
    emit document->stateChanged();
    enqueue([this, guard, revision, path, operation] {
        if (!guard || guard->closed()) { finish(); return; }
        worker<ReadResult>([path, operation] { return readPath(path, QUuid(operation)); }, [this, guard, revision](ReadResult read) {
            if (!guard || guard->closed()) { finish(); return; }
            guard->m_loading = false;
            auto existing = read.baseline ? m_registry.lookup(read.baseline->file()) : RegisteredSession{};
            const bool other = existing.session && existing.session != guard->session();
            const auto applied = read.baseline && !other ? guard->session()->reloadText(read.baseline->utf8(), revision) : EditResult::Unchanged;
            const bool ok = read.baseline && !other && applied == EditResult::Applied
                && m_registry.rebind(guard->session(), read.baseline) == RegistryResult::Reused;
            if (ok) {
                guard->m_baseline = read.baseline; guard->m_conflict = false; guard->m_error = false; guard->m_savedOnce = false;
                guard->m_message.clear(); guard->permissions();
            } else {
                guard->m_error = true;
                guard->m_message = other ? QStringLiteral("磁盘版本已在另一窗口打开，当前编辑已保留")
                    : !read.baseline ? read.problem.description() : QStringLiteral("重新载入期间文档又有变化，当前编辑已保留");
            }
            emit guard->stateChanged(); emit guard->operationFinished(ok); finish();
        });
    });
    return true;
}

void FileManager::check(FileDocument *document) {
    if (!document || document->closed() || document->saving() || document->loading() || document->m_checkPending) return;
    document->m_checkPending = true;
    const QPointer<FileDocument> guard(document);
    enqueue([this, guard] {
        if (!guard || guard->closed()) { finish(); return; }
        const auto baseline = guard->m_baseline;
        const auto path = guard->path();
        worker<ReadResult>([path] { return readPath(path, QUuid::createUuid()); }, [this, guard, baseline](ReadResult read) {
            if (guard && !guard->closed()) {
                guard->m_checkPending = false;
                if (!read.baseline || read.baseline->file()->identity() != baseline->file()->identity()
                    || read.baseline->diskBytes() != baseline->diskBytes()) {
                    guard->m_conflict = true;
                    guard->m_message = read.baseline ? QStringLiteral("文件已被其他程序修改，当前内容已保留")
                        : QStringLiteral("磁盘文件暂时无法读取，当前内容已保留：") + read.problem.description();
                } else {
                    guard->m_conflict = false;
                    if (!guard->m_error) guard->m_message.clear();
                }
                guard->permissions(); emit guard->stateChanged();
            }
            finish();
        });
    });
}
bool FileManager::closeDocument(FileDocument *document) {
    if (!document || document->closed() || document->saving() || document->loading() || !document->session()) return false;
    if (document->session()->requestClose() != EditResult::Applied) return false;
    document->m_closed = true;
    emit documentsChanged();
    return true;
}
}
