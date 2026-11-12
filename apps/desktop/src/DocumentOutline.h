#pragma once
#include <QWidget>
#include <QPointer>
#include <QFutureWatcher>
#include "MarkdownHeadings.h"
class QListView;
class QSortFilterProxyModel;
class QLabel;
namespace xdnote {
class Appearance;
class HeadingListModel;
class DocumentOutline final:public QWidget {
    Q_OBJECT
public:
    DocumentOutline(Appearance *appearance,QWidget*parent=nullptr);
    void setSession(DocumentSession*session);
signals:
    void headingActivated(QString id,QUuid document,quint64 revision);
    void closeRequested();
protected:
    void showEvent(QShowEvent*)override;
private:
    void refresh();
    QPointer<DocumentSession>m_session;
    QMetaObject::Connection m_changed,m_destroyed;
    QFutureWatcher<MarkdownHeadingMap>m_worker;
    HeadingListModel*m_model;
    QSortFilterProxyModel*m_filter;
    QListView*m_list;
    QLabel*m_status;
    bool m_pending=false;
};
}
