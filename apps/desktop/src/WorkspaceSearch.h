#pragma once
#include <QWidget>
#include <QHash>
#include <atomic>
#include "WorkspaceResourceReader.h"
class QLineEdit;
class QLabel;
class QListWidget;
namespace xdnote {
class Appearance;
class FileManager;
struct SearchBuffer { QByteArray utf8; QString identity; bool ready = false; };
struct WorkspaceHit { QString path, identity, excerpt; QByteArray sha256; qsizetype start=0,end=0; int line=0; };
struct WorkspaceMatches { QList<WorkspaceHit> hits; QString warning; bool cancelled=false; };
WorkspaceMatches searchWorkspace(const std::shared_ptr<WorkspaceResourceReader> &root,const QString &query,const QHash<QString,SearchBuffer> &buffers,const std::shared_ptr<std::atomic_bool> &cancel);
class WorkspaceSearch final : public QWidget {
    Q_OBJECT
public:
    WorkspaceSearch(Appearance *appearance, FileManager *manager, QWidget *parent);
    ~WorkspaceSearch() override;
    void setRoot(std::shared_ptr<WorkspaceResourceReader> root);
    void focusQuery();
signals:
    void backRequested();
    void locationRequested(QString path,QString identity,QByteArray sha256,qsizetype start,qsizetype end);
private:
    void start();
    void cancel();
    FileManager *m_manager;
    QLineEdit *m_query;
    QLabel *m_status;
    QListWidget *m_list;
    std::shared_ptr<WorkspaceResourceReader> m_root;
    std::shared_ptr<std::atomic_bool> m_cancel;
    quint64 m_generation=0;
    QList<WorkspaceHit> m_hits;
};
}
