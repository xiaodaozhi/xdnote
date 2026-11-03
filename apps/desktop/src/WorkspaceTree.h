#pragma once
#include <QTreeView>
#include <QFileSystemWatcher>
#include <QStandardItemModel>

namespace xdnote {
class Appearance;
class WorkspaceTree final : public QTreeView {
    Q_OBJECT
public:
    enum class State { Unselected, Loading, Ready, Empty, Missing, Unreadable };
    WorkspaceTree(Appearance *appearance, QWidget *parent);
    void setRootPath(const QString &path);
    void refresh();
    QString rootPath() const { return m_root; }
    State state() const { return m_state; }
    QString message() const { return m_message; }
    bool acceptsFile(const QString &path) const;
signals:
    void openRequested(QString path);
    void stateChanged();
private:
    void fetchDirectory(const QString &path, QStandardItem *parent, quint64 generation);
    void setState(State state, const QString &message);
    void applyAppearance();
    Appearance *m_appearance;
    QStandardItemModel m_model;
    QFileSystemWatcher m_watch;
    QString m_root, m_message;
    State m_state = State::Unselected;
    quint64 m_generation = 0;
};
}
