#pragma once
#include <QWidget>
class QLabel;
class QScrollArea;
namespace xdnote {
class Appearance;
class NavigationStore;
class WorkspaceNavigation;
class WorkspaceWelcome final : public QWidget {
    Q_OBJECT
public:
    WorkspaceWelcome(Appearance *, NavigationStore *, QWidget *parent);
    void setRootPath(const QString &path);
signals:
    void chooseFileRequested();
    void chooseDirectoryRequested();
    void newDocumentRequested();
    void recentRequested();
    void openRequested(QString path);
protected:
    bool eventFilter(QObject *, QEvent *) override;
private:
    void applyAppearance();
    void refreshRecent();
    Appearance *m_appearance;
    NavigationStore *m_navigation;
    QString m_root;
    QScrollArea *m_scroll;
    QWidget *m_panel = nullptr;
    QLabel *m_brand, *m_empty;
    WorkspaceNavigation *m_recent;
};
}
