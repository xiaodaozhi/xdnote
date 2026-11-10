#pragma once
#include "WorkspaceReferenceScan.h"
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QPointer>
#include <QTimer>

namespace xdnote {
class FileDocument;
class FileManager;
class WorkspaceReferenceController final : public QObject {
    Q_OBJECT
public:
    explicit WorkspaceReferenceController(FileManager *manager, QObject *parent = nullptr);
    ~WorkspaceReferenceController() override;
    void setRoot(std::shared_ptr<WorkspaceResourceReader> root, const QString &path);
    void setDocuments(const QList<FileDocument *> &documents);
    void refresh();
    void cancel();
    quint64 generation() const { return m_generation; }
    bool current(quint64 generation) const { return m_ready && generation == m_generation && m_root && m_root->active(); }
    bool running() const { return m_pending || m_worker.isRunning(); }
    QString statusText() const;
    QString relativePath(const QString &path) const;
    const ReferenceScanResult &result() const { return m_result; }
signals:
    void changed();
private:
    void invalidate();
    void observeDocuments();
    void start();
    std::shared_ptr<WorkspaceResourceReader> m_root;
    QString m_path;
    QUuid m_rootId;
    quint64 m_rootGeneration = 0, m_generation = 0, m_runningGeneration = 0, m_pathGeneration = 0;
    bool m_pending = false, m_paused = false, m_ready = false, m_observing = false;
    QTimer m_debounce;
    QFileSystemWatcher m_watch;
    QFutureWatcher<ReferenceScanResult> m_worker;
    std::shared_ptr<std::atomic_bool> m_cancelled;
    QList<QPointer<FileDocument>> m_documents;
    QList<QMetaObject::Connection> m_connections;
    QHash<QUuid, QString> m_documentStates, m_documentPaths;
    QHash<QUuid, quint64> m_pathVersions;
    ReferenceScanResult m_result;
};
}
