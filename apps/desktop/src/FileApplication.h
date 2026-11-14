#pragma once
#include "FileManager.h"
#include "Appearance.h"
#include "WindowGeometry.h"
#include "NavigationStore.h"
#include <QSet>
#include <QJsonArray>
namespace xdnote {
class SingleFileWindow;
class WorkspaceWindow;
class SettingsWindow;
class AutoSaveController;
class RecoveryStore;
class FileApplication final : public QObject {
    Q_OBJECT
public:
    explicit FileApplication(QObject *parent = nullptr, QString settingsPath = {});
    ~FileApplication() override;
    void open(const QString &path, SingleFileWindow *empty = nullptr);
    void openExternal(const QString &path);
    SettingsWindow *settingsWindow();
    SingleFileWindow *emptyWindow();
    WorkspaceWindow *workspaceWindow(const QString &root = {});
    void openInWorkspace(const QString &path, WorkspaceWindow *workspace);
    bool transferToWorkspace(SingleFileWindow *pane, WorkspaceWindow *target = nullptr);
    void activate();
    void requestQuit();
protected:
    bool eventFilter(QObject *target, QEvent *event) override;
private:
    SingleFileWindow *createWindow();
    WorkspaceWindow *createWorkspace();
    void activatePane(SingleFileWindow *pane);
    void trackWorkspaceRequest(const QUuid &intent, SingleFileWindow *pane, WorkspaceWindow *workspace);
    void chooseDirectory(WorkspaceWindow *owner);
    void chooseFile(WorkspaceWindow *owner, bool workspaceTab);
    void retryTransfers();
    void advanceQuit();
    void captureRecovery();
    void restoreSession();
    void refreshRecoveryActions();
    void applyRestoredState(SingleFileWindow *pane,const QJsonObject &entry);
    RecoveryStore *m_recovery = nullptr;
    QTimer m_recoveryTimer;
    QHash<QString,QJsonObject> m_unrestored,m_liveRecovery;
    QHash<QUuid,QJsonObject> m_restoreRequests;
    QJsonArray m_lastSession;
    QStringList m_recoveryOrder;
    bool m_restoring = false;
    QString m_recoveryError;
    void openDocumentLink(SingleFileWindow *source, const QString &target, quint64 revision);
    struct LinkNavigation {
        QPointer<SingleFileWindow> source;
        QPointer<WorkspaceWindow> workspace;
        std::shared_ptr<WorkspaceResourceReader> root;
        QString sourcePath, sourceIdentity, fragment;
        quint64 revision = 0;
        bool embedded = false, hasFragment = false;
        bool sourceLocation = false;
        QByteArray sourceSha256;
        QString locationIdentity;
        qsizetype locationStart = 0, locationEnd = 0;
    };
    bool validLinkSource(const LinkNavigation &navigation) const;
    Appearance m_appearance;
    WindowGeometryStore m_geometryStore;
    NavigationStore m_navigation;
    FileManager m_manager;
    QPointer<SettingsWindow> m_settings;
    AutoSaveController *m_autoSave = nullptr;
    QSet<SingleFileWindow *> m_windows;
    QSet<WorkspaceWindow *> m_workspaces;
    struct Transfer { QPointer<SingleFileWindow> pane; QPointer<WorkspaceWindow> target; bool explicitTarget; };
    QHash<QUuid, Transfer> m_transfers;
    QHash<QUuid, QPointer<SingleFileWindow>> m_requests;
    QHash<QUuid, QPointer<WorkspaceWindow>> m_workspaceRequests;
    QHash<QUuid, QPointer<SingleFileWindow>> m_documents;
    QHash<QUuid, LinkNavigation> m_linkNavigations;
    bool m_quitting = false;
    bool m_destroying = false;
};
}
