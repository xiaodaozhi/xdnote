#include "WorkspaceSearch.h"
#include "DocumentSearch.h"
#include "FileManager.h"
#include "Appearance.h"
#include "SharedWidgets.h"
#include "Utf8Text.h"
#include "UiFonts.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QListWidget>
#include <QLabel>
#include <QDir>
#include <QCryptographicHash>
#include <QFutureWatcher>
#include <QtConcurrentRun>
namespace xdnote {
WorkspaceMatches searchWorkspace(const std::shared_ptr<WorkspaceResourceReader> &root,const QString &query,const QHash<QString,SearchBuffer> &buffers,const std::shared_ptr<std::atomic_bool> &cancel){
    WorkspaceMatches result;if(!root||!root->active()||query.isEmpty()||query.size()>256){result.warning=QStringLiteral("请打开工作区，并输入不超过 256 字符的搜索词");return result;}
    QStringList pending{QString()};int entries=0,files=0,skipped=0;qsizetype bytes=0;
    while(!pending.isEmpty()){
        if(cancel->load()||!root->active()){result.cancelled=true;return result;}auto directory=pending.takeLast();const auto listing=root->listDirectory(directory,10000);
        if(!listing.ok()||!listing.complete)++skipped;
        for(const auto &entry:listing.entries){
            if(cancel->load()){result.cancelled=true;return result;}
            if(++entries>20000||files>=2000||bytes>64*1024*1024||result.hits.size()>=2000){result.warning=QStringLiteral("达到搜索上限，结果不完整；请缩小工作区或搜索范围");return result;}
            const auto path=directory.isEmpty()?entry.name:directory+'/'+entry.name;
            if(entry.kind==WorkspaceDirectoryEntry::Kind::Directory){if(entry.name!=".git"&&entry.name!=".svn")pending.append(path);continue;}
            if(entry.kind!=WorkspaceDirectoryEntry::Kind::File||(!path.endsWith(".md",Qt::CaseInsensitive)&&!path.endsWith(".markdown",Qt::CaseInsensitive)))continue;
            ++files;QByteArray content;QString identity;
            if(buffers.contains(path)){const auto buffer=buffers[path];if(!buffer.ready){++skipped;continue;}content=buffer.utf8;identity=buffer.identity;}
            else{const auto read=root->read(path,DocumentSession::AdmissionSourceLimit+3);if(!read.ok()){++skipped;continue;}content=read.bytes;identity=read.identity;if(content.startsWith("\xef\xbb\xbf"))content.remove(0,3);}
            bool valid=false;const auto text=decodeUtf8Literal(content,&valid);bytes+=content.size();if(!valid||content.size()>DocumentSession::AdmissionSourceLimit){++skipped;continue;}
            const auto found=findText(text,query,false,false,2001-int(result.hits.size()));const auto hash=QCryptographicHash::hash(content,QCryptographicHash::Sha256);
            for(const auto &match:found.items){if(result.hits.size()>=2000){result.warning=QStringLiteral("匹配超过 2000 项，结果不完整");return result;}const auto start=text.left(match.start).toUtf8().size();const auto end=start+text.mid(match.start,match.length).toUtf8().size();const auto left=match.start==0?0:text.lastIndexOf('\n',match.start-1)+1;const auto right=text.indexOf('\n',match.start);auto excerpt=text.mid(left,right<0?text.size()-left:right-left).trimmed();if(excerpt.size()>160)excerpt=excerpt.left(157)+QStringLiteral("…");result.hits.append({path,identity,excerpt,hash,start,end,int(text.left(match.start).count('\n')+1)});}
        }
    }
    if(skipped)result.warning=QStringLiteral("%1 个条目未读取（权限、编码、容量或正在输入）；结果可能不完整").arg(skipped);return result;
}
WorkspaceSearch::WorkspaceSearch(Appearance *a,FileManager *manager,QWidget *parent):QWidget(parent),m_manager(manager){
    setObjectName("workspaceFullSearch");auto *box=new QVBoxLayout(this);box->setContentsMargins(12,12,12,12);box->setSpacing(8);auto *back=new SharedButton(QStringLiteral("返回文件"),"fullSearchBack",a,this);back->setOutlined(true);box->addWidget(back);connect(back,&QPushButton::clicked,this,&WorkspaceSearch::backRequested);
    m_query=new SharedLineEdit(a,this);m_query->setObjectName("fullSearchQuery");m_query->setPlaceholderText(QStringLiteral("搜索工作区正文"));m_query->setAccessibleName(QStringLiteral("全文搜索"));m_query->setMaxLength(256);m_query->setClearButtonEnabled(true);box->addWidget(m_query);auto *buttons=new QHBoxLayout;auto *go=new SharedButton(QStringLiteral("搜索"),"fullSearchStart",a,this);go->setProperty("primary",true);buttons->addWidget(go);auto *stop=new SharedButton(QStringLiteral("取消"),"fullSearchCancel",a,this);stop->setOutlined(true);buttons->addWidget(stop);box->addLayout(buttons);m_status=new QLabel(QStringLiteral("只搜索已授权工作区的 Markdown 原文"),this);m_status->setWordWrap(true);m_status->setFont(uiFont(12));box->addWidget(m_status);m_list=new QListWidget(this);m_list->setObjectName("fullSearchResults");m_list->setTextElideMode(Qt::ElideRight);m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);box->addWidget(m_list,1);
    connect(go,&QPushButton::clicked,this,&WorkspaceSearch::start);connect(m_query,&QLineEdit::returnPressed,this,&WorkspaceSearch::start);connect(stop,&QPushButton::clicked,this,[this]{cancel();m_status->setText(QStringLiteral("搜索已取消"));});connect(m_query,&QLineEdit::textChanged,this,[this]{cancel();m_hits.clear();m_list->clear();m_status->setText(QStringLiteral("按 Enter 搜索"));});
    connect(m_list,&QListWidget::itemActivated,this,[this](QListWidgetItem *item){const int row=m_list->row(item);if(row<0||row>=m_hits.size())return;const auto hit=m_hits[row];if(m_root&&m_root->active())emit locationRequested(hit.path,hit.identity,hit.sha256,hit.start,hit.end);});
    const auto theme=[this,a]{m_list->setStyleSheet(QStringLiteral("QListWidget{background:%1;color:%2;border:1px solid %3;border-radius:6px;outline:0} QListWidget::item{padding:8px;border-radius:6px} QListWidget::item:selected{background:%4} QListWidget::item:hover{background:%5}").arg(a->color("surface").name(),a->color("text").name(),a->color("border").name(),a->accentColor("subtle").name(),a->color("hover").name())+themedScrollBars(a));m_status->setStyleSheet("color:"+a->color("textSecondary").name());};connect(a,&Appearance::changed,this,theme);theme();
}
WorkspaceSearch::~WorkspaceSearch(){cancel();}
void WorkspaceSearch::cancel(){++m_generation;if(m_cancel)m_cancel->store(true);}
void WorkspaceSearch::setRoot(std::shared_ptr<WorkspaceResourceReader> root){cancel();m_root=std::move(root);m_hits.clear();m_list->clear();m_status->setText(m_root?QStringLiteral("按 Enter 搜索工作区原文"):QStringLiteral("请先打开工作区目录"));}
void WorkspaceSearch::focusQuery(){m_query->setFocus(Qt::ShortcutFocusReason);m_query->selectAll();}
void WorkspaceSearch::start(){
    cancel();m_hits.clear();m_list->clear();const auto query=m_query->text();if(query.isEmpty()||!m_root){m_status->setText(QStringLiteral("请打开工作区并输入搜索词"));return;}
    QHash<QString,SearchBuffer> buffers;if(m_manager)for(auto *document:m_manager->documents()){const auto path=QDir(m_root->rootPath()).relativeFilePath(document->path());if(path==".."||path.startsWith("../")||QDir::isAbsolutePath(path))continue;const auto snapshot=document->session()->snapshot();buffers[path]={snapshot.utf8,document->identity(),snapshot.result==EditResult::Applied};}
    const auto generation=m_generation;const auto root=m_root;const auto token=m_cancel=std::make_shared<std::atomic_bool>(false);m_status->setText(QStringLiteral("正在搜索…"));auto *watcher=new QFutureWatcher<WorkspaceMatches>(this);
    connect(watcher,&QFutureWatcher<WorkspaceMatches>::finished,this,[this,watcher,generation]{const auto result=watcher->result();watcher->deleteLater();if(generation!=m_generation||result.cancelled)return;m_hits=result.hits;for(const auto &hit:m_hits){auto *item=new QListWidgetItem(QStringLiteral("%1:%2\n%3").arg(hit.path).arg(hit.line).arg(hit.excerpt),m_list);item->setToolTip(item->text());}m_status->setText(QStringLiteral("%1 个匹配%2").arg(m_hits.size()).arg(result.warning.isEmpty()?QString():"\n"+result.warning));});watcher->setFuture(QtConcurrent::run([root,query,buffers,token]{return searchWorkspace(root,query,buffers,token);}));
}
}
