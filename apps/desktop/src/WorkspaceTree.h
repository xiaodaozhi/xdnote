#pragma once
#include <QTreeView>
#include <QFileSystemWatcher>
#include <QStandardItemModel>
#include <atomic>
#include <memory>

namespace xdnote {
class Appearance;
class WorkspaceTree final : public QTreeView {
    Q_OBJECT
public:
    enum class State { Unselected, Loading, Ready, Empty, Missing, Unreadable };
    WorkspaceTree(Appearance *appearance, QWidget *parent);
    ~WorkspaceTree() override;
    void setRootPath(const QString &path);
    void refresh();
    QString rootPath() const { return m_root; }
    State state() const { return m_state; }
    QString message() const { return m_message; }
    bool acceptsFile(const QString &path) const;
    void selectPath(const QString &path);
    void searchFiles(const QString &query);
    void cancelSearch();
signals:
    void openRequested(QString path);
    void stateChanged();
    void searchFinished(QString query, QStringList paths, QString warning);
protected:
    void drawRow(QPainter *, const QStyleOptionViewItem &, const QModelIndex &) const override;
    void drawBranches(QPainter *, const QRect &, const QModelIndex &) const override;
    void mouseMoveEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;
private:
    void fetchDirectory(const QString &path, QStandardItem *parent, quint64 generation);
    void setState(State state, const QString &message);
    void applyAppearance();
    void restoreSelection();
    Appearance *m_appearance;
    QStandardItemModel m_model;
    QFileSystemWatcher m_watch;
    QString m_root, m_message;
    State m_state = State::Unselected;
    quint64 m_generation = 0;
    QString m_selectedPath;
    QStringList m_expanded;
    QPersistentModelIndex m_hover;
    std::shared_ptr<std::atomic_bool> m_searchCancel;
};
}
