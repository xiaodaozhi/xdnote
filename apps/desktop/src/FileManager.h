#pragma once

#include "FileService.h"
#include "SessionRegistry.h"
#include <QFutureWatcher>
#include <QSet>
#include <QTimer>
#include <QtConcurrentRun>
#include <deque>

namespace xdnote {
class FileManager;
class FileDocument final : public QObject {
    Q_OBJECT
public:
    DocumentSession *session() const { return m_session; }
    QString path() const { return m_baseline->file()->path(); }
    QString identity() const { return m_baseline->file()->identity(); }
    bool dirty() const { return m_session && m_session->isDirty(); }
    bool saving() const { return m_saving; }
    bool loading() const { return m_loading; }
    bool conflict() const { return m_conflict; }
    bool readOnly() const { return m_readOnly; }
    bool closed() const { return m_closed; }
    bool maintainingResources() const { return m_maintenance; }
    const std::shared_ptr<const FileBaseline> &baseline() const { return m_baseline; }
    bool hasError() const { return m_error; }
    bool bom() const { return m_baseline->hasUtf8Bom(); }
    QString message() const { return m_message; }
    QString statusText() const;
    const FileSaveResult &lastSave() const { return m_lastSave; }
    bool save();
    bool saveAs(const QString &path);
    bool reload();
    void checkExternal();
signals:
    void stateChanged();
    void operationFinished(bool success);
    void saveFinished(xdnote::SaveStatus status);
private:
    friend class FileManager;
    FileDocument(FileManager *manager, DocumentSession *session, std::shared_ptr<const FileBaseline> baseline);
    void permissions();
    FileManager *m_manager;
    QPointer<DocumentSession> m_session;
    std::shared_ptr<const FileBaseline> m_baseline;
    bool m_saving = false, m_loading = false, m_conflict = false, m_error = false;
    bool m_closed = false, m_readOnly = false, m_checkPending = false, m_savedOnce = false;
    bool m_maintenance = false;
    QString m_message;
    QUuid m_operation;
    FileSaveResult m_lastSave;
    QStringList m_recoveries;
};

// Disk operations are serialized through completion on the GUI thread. Editing
// stays live; opens cannot race the file-ID change made by save/Save As.
class FileManager final : public QObject {
    Q_OBJECT
public:
    explicit FileManager(QObject *parent = nullptr, std::function<void(SaveStage)> observer = {}, bool monitoring = true);
    void open(const QString &path, const QUuid &intent = QUuid::createUuid());
    void openWithin(const std::shared_ptr<WorkspaceResourceReader> &root, const QString &relativePath, const QUuid &intent, const QString &sourcePath = {}, const QString &sourceIdentity = {});
    QList<FileDocument *> documents() const;
    void cancelOpen(const QUuid &intent) { m_cancelled.insert(intent); }
    bool closeDocument(FileDocument *document);
    int documentCount() const { return int(m_documents.size()); }
    bool idle() const { return !m_running && m_jobs.empty(); }
    QUuid beginWorkspaceOperation();
    bool protectWorkspaceDocument(FileDocument *document, const QUuid &operation, bool protect);
    bool relocateWorkspaceDocument(FileDocument *document, const QString &path, const QUuid &operation);
    void reportWorkspaceProblem(FileDocument *document, const QString &message, const QUuid &operation);
    void endWorkspaceOperation(const QUuid &operation);
signals:
    void documentsChanged();
    void resourcesChanged();
    void opened(QUuid intent, xdnote::FileDocument *document, bool reused);
    void openFailed(QUuid intent, QString path, QString message);
private:
    friend class FileDocument;
    void openRequest(const QString &path, const QUuid &intent, std::shared_ptr<WorkspaceResourceReader> root = {}, QString relativePath = {}, QString sourcePath = {}, QString sourceIdentity = {});
    bool save(FileDocument *document, const QString &path, bool saveAs = false);
    bool reload(FileDocument *document);
    void check(FileDocument *document);
    void enqueue(std::function<void()> job);
    void pump();
    void finish();
    void performSave(QPointer<FileDocument> document, DocumentSnapshot snapshot, QString path,
                     std::shared_ptr<const FileBaseline> target, QUuid operation);
    template<class T> void worker(std::function<T()> job, std::function<void(T)> completed) {
        auto *watcher = new QFutureWatcher<T>(this);
        connect(watcher, &QFutureWatcher<T>::finished, this, [watcher, completed] {
            const auto value = watcher->result();
            watcher->deleteLater();
            completed(value);
        });
        watcher->setFuture(QtConcurrent::run(std::move(job)));
    }
    SessionRegistry m_registry;
    QHash<QUuid, QPointer<FileDocument>> m_documents;
    QSet<QUuid> m_cancelled;
    std::deque<std::function<void()>> m_jobs;
    bool m_running = false;
    QUuid m_workspaceOperation;
    QTimer m_poll;
    std::function<void(SaveStage)> m_observer;
};
}
Q_DECLARE_METATYPE(xdnote::FileDocument *)
