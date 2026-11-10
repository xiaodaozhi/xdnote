#pragma once
#include "FileManager.h"
#include "Appearance.h"
#include "WindowGeometry.h"
#include "NavigationStore.h"
#include <QSet>
namespace xdnote {
class SingleFileWindow;
class WorkspaceWindow;
class FileApplication final : public QObject {
    Q_OBJECT
public:
    explicit FileApplication(QObject *parent = nullptr, QString settingsPath = {});
    ~FileApplication() override;
    void open(const QString &path, SingleFileWindow *empty = nullptr);
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
    void chooseDirectory(WorkspaceWindow *owner);
    void retryTransfers();
    void advanceQuit();
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
    QSet<SingleFileWindow *> m_windows;
    QSet<WorkspaceWindow *> m_workspaces;
    struct Transfer { QPointer<SingleFileWindow> pane; QPointer<WorkspaceWindow> target; bool explicitTarget; };
    QHash<QUuid, Transfer> m_transfers;
    QHash<QUuid, QPointer<SingleFileWindow>> m_requests;
    QHash<QUuid, QPointer<SingleFileWindow>> m_documents;
    QHash<QUuid, LinkNavigation> m_linkNavigations;
    bool m_quitting = false;
    bool m_destroying = false;
};
}
