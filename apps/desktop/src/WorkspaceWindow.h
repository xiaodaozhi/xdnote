#pragma once
#include "WindowChrome.h"
#include <QHash>
#include <QPointer>
#include <QUuid>
#include <QTimer>

class QTabBar;
class QStackedWidget;
class QLabel;
class QAction;
class QLineEdit;
namespace xdnote {
class SingleFileWindow;
class WindowGeometry;
class WindowGeometryStore;
class WorkspaceTree;
class ElidedLabel;
class SharedButton;
class NavigationStore;
class WorkspaceNavigation;
class WorkspaceWindow final : public ApplicationWindow {
    Q_OBJECT
public:
    WorkspaceWindow(Appearance *appearance, WindowGeometryStore *store, NavigationStore *navigation = nullptr);
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
signals:
    void openRequested(QString path);
    void chooseFileRequested();
    void chooseDirectoryRequested();
    void closeCancelled();
    void quitRequested();
protected:
    void closeEvent(QCloseEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    bool event(QEvent *) override;
    bool eventFilter(QObject *, QEvent *) override;
private:
    void refreshPane(const QUuid &id);
    void selectCurrent();
    void advanceClose();
    void applyAppearance();
    void updateSidebar();
    void showNavigation(bool favorites);
    void updateNavigationSelection();
    void startSearch();
    int indexOf(const QUuid &id) const;
    QHash<QUuid, QPointer<SingleFileWindow>> m_panes;
    WindowTitleBar *m_header;
    QTabBar *m_tabs;
    QWidget *m_sidebar = nullptr;
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
    QWidget *m_welcome, *m_emptyFooter;
    WindowGeometry *m_geometry;
    QAction *m_emptyOpen, *m_emptyQuit;
    bool m_closingAll = false, m_sidebarWanted = true;
    bool m_sidebarExplicit = false;
    bool m_tearingDown = false;
};
}
