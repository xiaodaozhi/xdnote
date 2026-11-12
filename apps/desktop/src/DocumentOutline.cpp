#include "DocumentOutline.h"
#include "SharedWidgets.h"
#include "Appearance.h"
#include "InputModality.h"
#include "UiFonts.h"
#include <QAbstractListModel>
#include <QSortFilterProxyModel>
#include <QListView>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QtConcurrentRun>
#include <QShowEvent>
namespace xdnote {
class HeadingListModel final:public QAbstractListModel {
public:
    HeadingListModel(QObject*p):QAbstractListModel(p){}
    MarkdownHeadingMap map;
    int rowCount(const QModelIndex&parent={})const override{return parent.isValid()?0:int(map.headings.size());}
    QVariant data(const QModelIndex&index,int role)const override{if(!index.isValid()||index.row()<0||index.row()>=map.headings.size())return {};const auto&h=map.headings[index.row()];if(role==Qt::DisplayRole)return QString(qMax(0,h.level-1)*2,' ')+h.title;if(role==Qt::ToolTipRole||role==Qt::AccessibleTextRole)return h.title;if(role==Qt::FontRole){auto f=uiFont();if(h.level==1)f.setWeight(QFont::DemiBold);return f;}return {};}
    void replace(MarkdownHeadingMap value){beginResetModel();map=std::move(value);endResetModel();}
};
namespace {
class HeadingDelegate final:public QStyledItemDelegate{
    Appearance*a;QListView*view;
public:HeadingDelegate(Appearance*a,QListView*v):QStyledItemDelegate(v),a(a),view(v){}
    QSize sizeHint(const QStyleOptionViewItem&o,const QModelIndex&i)const override{auto s=QStyledItemDelegate::sizeHint(o,i);s.setHeight(32);return s;}
    void paint(QPainter*p,const QStyleOptionViewItem&o,const QModelIndex&i)const override{auto opt=o;opt.state&=~QStyle::State_HasFocus;QStyledItemDelegate::paint(p,opt,i);if(view->hasFocus()&&InputModality::forApplication()->keyboardNavigation()&&i==view->currentIndex()){p->save();p->setRenderHint(QPainter::Antialiasing);p->setPen(QPen(a->accentColor(),2));p->setBrush(Qt::NoBrush);p->drawRoundedRect(o.rect.adjusted(2,2,-2,-2),4,4);p->restore();}}
};
}
DocumentOutline::DocumentOutline(Appearance*a,QWidget*p):QWidget(p){setObjectName("documentOutline");setFont(uiFont());auto*l=new QVBoxLayout(this);l->setContentsMargins(12,12,12,12);l->setSpacing(8);auto*top=new QHBoxLayout;auto*title=new QLabel(QStringLiteral("文档大纲"),this);auto f=uiFont(14);f.setWeight(QFont::DemiBold);title->setFont(f);top->addWidget(title,1);auto*close=new SharedButton({},"closeOutline",a,this);close->setSymbol("close");close->setFixedWidth(32);close->setAccessibleName(QStringLiteral("收起文档大纲"));top->addWidget(close);l->addLayout(top);connect(close,&QPushButton::clicked,this,&DocumentOutline::closeRequested);
    auto*search=new SharedLineEdit(a,this);search->setObjectName("outlineSearch");search->setPlaceholderText(QStringLiteral("搜索标题"));search->setClearButtonEnabled(true);l->addWidget(search);m_status=new QLabel(this);m_status->setProperty("secondary",true);m_status->setWordWrap(true);m_status->setFont(uiFont(12));l->addWidget(m_status);
    m_model=new HeadingListModel(this);m_filter=new QSortFilterProxyModel(this);m_filter->setSourceModel(m_model);m_filter->setFilterCaseSensitivity(Qt::CaseInsensitive);m_list=new QListView(this);m_list->setObjectName("outlineHeadings");m_list->setModel(m_filter);m_list->setUniformItemSizes(true);m_list->setItemDelegate(new HeadingDelegate(a,m_list));m_list->setFrameShape(QFrame::NoFrame);l->addWidget(m_list,1);connect(search,&QLineEdit::textChanged,m_filter,&QSortFilterProxyModel::setFilterFixedString);
    auto activate=[this](const QModelIndex&i){if(!m_session)return;const auto snapshot=m_session->snapshot();if(snapshot.documentId!=m_model->map.documentId||snapshot.revision!=m_model->map.revision){refresh();return;}const auto row=m_filter->mapToSource(i).row();if(row<0||row>=m_model->map.headings.size())return;InputModality::forApplication()->clear();m_list->viewport()->update();emit headingActivated(m_model->map.headings[row].id,snapshot.documentId,snapshot.revision);};connect(m_list,&QListView::clicked,this,activate);connect(m_list,&QListView::activated,this,activate);
    auto theme=[this,a]{setStyleSheet(QStringLiteral("QWidget#documentOutline{background:%1;color:%2} QLabel{background:transparent;color:%2} QLabel[secondary=true]{color:%3} QListView{background:%1;color:%2;border:0;outline:0} QListView::item{padding:4px;border-radius:4px} QListView::item:selected{background:%4;color:%2} QListView::item:hover{background:%5}").arg(a->color("sidebar").name(),a->color("text").name(),a->color("textSecondary").name(),a->accentColor("soft").name(),a->color("controlHover").name())+themedScrollBars(a));};connect(a,&Appearance::changed,this,theme);connect(InputModality::forApplication(),&InputModality::changed,m_list->viewport(),qOverload<>(&QWidget::update));theme();
    connect(&m_worker,&QFutureWatcher<MarkdownHeadingMap>::finished,this,[this]{const auto result=m_worker.result();if(!m_session){m_model->replace({});return;}const auto snapshot=m_session->snapshot();if(snapshot.documentId==result.documentId&&snapshot.revision==result.revision){m_model->replace(result);m_list->setEnabled(result.success);m_status->setText(result.success?(result.headings.isEmpty()?QStringLiteral("当前文档没有标题"):QStringLiteral("%1 个标题").arg(result.headings.size())):result.reason);}else m_pending=true;if(m_pending){m_pending=false;refresh();}});
}
void DocumentOutline::setSession(DocumentSession*session){if(m_session==session)return;disconnect(m_changed);disconnect(m_destroyed);m_session=session;m_model->replace({});if(session){m_changed=connect(session,&DocumentSession::changed,this,[this]{refresh();});m_destroyed=connect(session,&QObject::destroyed,this,[this]{m_session=nullptr;m_model->replace({});m_status->setText(QStringLiteral("请选择文档"));});}refresh();}
void DocumentOutline::showEvent(QShowEvent*e){QWidget::showEvent(e);refresh();}
void DocumentOutline::refresh(){if(!isVisible())return;if(!m_session){m_status->setText(QStringLiteral("请选择文档"));return;}if(m_worker.isRunning()){m_pending=true;return;}const auto snapshot=m_session->snapshot();if(snapshot.result!=EditResult::Applied){m_status->setText(QStringLiteral("正在输入，稍后刷新大纲"));m_list->setEnabled(false);return;}m_status->setText(QStringLiteral("正在读取标题…"));m_list->setEnabled(false);m_worker.setFuture(QtConcurrent::run([snapshot]{return analyzeMarkdownHeadings(snapshot);}));}
}
