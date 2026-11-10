#pragma once
#include "ResourceMaintenance.h"
#include <QObject>
#include <QPointer>
#include <QFutureWatcher>

namespace xdnote {
class FileManager;
class FileDocument;
class WorkspaceWindow;
class ResourceMaintenanceController final : public QObject {
    Q_OBJECT
public:
    ResourceMaintenanceController(FileManager *manager, WorkspaceWindow *window);
    ~ResourceMaintenanceController() override;
    bool busy() const { return m_busy; }
    bool canRecover() const { return !m_busy && m_canRecover; }
    const ResourceMaintenancePreview &preview() const { return m_preview; }
    const ResourceMaintenanceResult &result() const { return m_result; }
    void prepare(const QString &from, const QString &to, bool retargetOnly = false);
    bool apply(bool maintain = true);
    bool recover();
    void cancel();
    void show(const QString &from = {});
signals:
    void previewReady();
    void completed();
private:
    bool start(bool maintain);
    void finish(ResourceMaintenanceResult result);
    QString relativePath(const QString &absolute) const;
    QPointer<FileManager> m_manager;
    QPointer<WorkspaceWindow> m_window;
    std::shared_ptr<WorkspaceResourceReader> m_root;
    ResourceMaintenancePreview m_preview, m_receipt;
    ResourceMaintenanceResult m_result, m_receiptResult;
    QHash<QString, QPointer<FileDocument>> m_open;
    std::shared_ptr<std::atomic_bool> m_cancelled;
    QFutureWatcher<ResourceMaintenancePreview> m_prepare;
    QFutureWatcher<ResourceMaintenanceResult> m_execute;
    QUuid m_operation;
    bool m_busy = false, m_canRecover = false, m_recovering = false;
};
}
