#pragma once
#include "WindowChrome.h"
#include <QHash>
#include <QPointer>
#include <QUuid>
#include <QTimer>
#include <memory>

class QTabBar;
class QStackedWidget;
class QLabel;
class QAction;
class QLineEdit;
class QMenu;
namespace xdnote {
class SingleFileWindow;
class WindowGeometry;
class WindowGeometryStore;
class WorkspaceTree;
class ElidedLabel;
class SharedButton;
class NavigationStore;
class WorkspaceNavigation;
class WorkspaceWelcome;
class WorkspaceResourceReader;
class WorkspaceReferenceController;
class FileManager;
class ResourceMaintenanceController;
class DocumentOutline;
class WorkspaceSearch;
class WorkspaceWindow final : public ApplicationWindow {
    Q_OBJECT
public:
    WorkspaceWindow(Appearance *appearance, WindowGeometryStore *store, NavigationStore *navigation = nullptr, FileManager *manager = nullptr);
    ~WorkspaceWindow() override;
    bool addPane(SingleFileWindow *pane);
    void activatePane(const QUuid &id);
    void closePane(const QUuid &id);
    SingleFileWindow *activePane() const;
    QList<SingleFileWindow *> panes() const;
    bool closingWorkspace() const { return m_closingAll; }
    void setRootPath(const QString &path);
    QString rootPath() const;
    WorkspaceTree *tree() const { return m_tree; }
    WindowGeometry *windowGeometry() const { return m_geometry; }
    void showNotice(const QString &message);
    WorkspaceReferenceController *references() const { return m_references; }
    ResourceMaintenanceController *resourceMaintenance() const { return m_maintenance; }
    void setFocusMode(bool enabled);
    std::shared_ptr<WorkspaceResourceReader> resourceRoot() const { return m_resourceRoot; }
signals:
    void referenceSourceRequested(QString path, QString identity, QByteArray sha256, qsizetype start, qsizetype end);
    void openRequested(QString path);
    void chooseFileRequested();
    void chooseWorkspaceFileRequested();
    void newDocumentRequested();
    void chooseDirectoryRequested();
    void closeCancelled();
    void quitRequested();
    void settingsRequested();
protected:
    void closeEvent(QCloseEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    bool event(QEvent *) override;
    bool eventFilter(QObject *, QEvent *) override;
private:
    WorkspaceSearch *m_fullSearch = nullptr;
    bool m_fullSearchWanted = false;
    void installTabClose(int index, const QUuid &id);
    void refreshPane(const QUuid &id);
    void selectCurrent();
    void advanceClose();
    void applyAppearance();
    void updateSidebar();
    void showNavigation(bool favorites);
    void updateNavigationSelection();
    void startSearch();
    void populateReferences(QMenu *menu, SingleFileWindow *pane);
    int indexOf(const QUuid &id) const;
    QHash<QUuid, QPointer<SingleFileWindow>> m_panes;
    std::shared_ptr<WorkspaceResourceReader> m_resourceRoot;
    WorkspaceReferenceController *m_references;
    ResourceMaintenanceController *m_maintenance;
    WindowTitleBar *m_header;
    QTabBar *m_tabs;
    QWidget *m_sidebar = nullptr;
    QStackedWidget *m_sidebarViews = nullptr;
    DocumentOutline *m_outline = nullptr;
    bool m_outlineWanted = false, m_focusMode = false;
    WorkspaceTree *m_tree;
    SharedButton *m_rootLabel, *m_recent, *m_favorites, *m_theme;
    QLineEdit *m_search = nullptr;
    QWidget *m_searchBox;
    NavigationStore *m_navigation;
    WorkspaceNavigation *m_navigationPage;
    QTimer m_searchDelay;
    QAction *m_favoriteAction = nullptr;
    bool m_navigationVisible = false, m_searchTab = false;
    QLabel *m_treeState, *m_warning;
    QStackedWidget *m_pages = nullptr, *m_footers = nullptr;
    WorkspaceWelcome *m_welcome;
    QWidget *m_emptyFooter;
    WindowGeometry *m_geometry;
    QAction *m_emptyOpen, *m_emptyQuit;
    bool m_closingAll = false, m_sidebarWanted = true;
    bool m_sidebarExplicit = false;
    bool m_tearingDown = false;
};
}
