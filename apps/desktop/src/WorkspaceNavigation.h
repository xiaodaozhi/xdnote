#pragma once
#include <QWidget>
class QLabel;
class QListWidget;
namespace xdnote {
class Appearance;
class NavigationStore;
class WorkspaceNavigation final : public QWidget {
    Q_OBJECT
public:
    enum class Page { Recent, Favorites, Search };
    WorkspaceNavigation(Appearance *appearance, NavigationStore *store, QWidget *parent);
    void showPage(Page page, const QString &root);
    void showSearch(const QString &query, const QStringList &paths, const QString &warning, bool loading = false);
    Page page() const { return m_page; }
    QString selectedPath() const;
    void toggleSelectedFavorite();
signals:
    void openRequested(QString path);
    void clearSearchRequested();
private:
    void refresh();
    void applyAppearance();
    Appearance *m_appearance;
    NavigationStore *m_store;
    Page m_page = Page::Recent;
    QString m_root, m_query, m_searchWarning;
    QStringList m_searchPaths;
    bool m_loading = false;
    QLabel *m_heading, *m_summary, *m_empty;
    QListWidget *m_list;
    QWidget *m_clear;
};
}
