#include "DocumentSearch.h"
#include "Appearance.h"
#include "SharedWidgets.h"
#include "UiFonts.h"
#include "Utf8Text.h"
#include <QApplication>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <algorithm>

namespace xdnote {
TextMatches findText(const QString &text, const QString &query, bool matchCase, bool wholeWord, int limit) {
    TextMatches result;
    if (query.isEmpty() || limit < 1 || !DocumentSession::validText(query)) return result;
    const auto word = [](QChar c) { return c.isLetterOrNumber() || c == '_' || c.isMark() || c.isSurrogate(); };
    int from = 0;
    while (from <= text.size()-query.size()) {
        const auto at = text.indexOf(query,from,matchCase?Qt::CaseSensitive:Qt::CaseInsensitive);
        if (at < 0) break;
        const int end = int(at+query.size());
        if (DocumentSession::utf16Boundary(text,int(at)) && DocumentSession::utf16Boundary(text,end)
            && (!wholeWord || ((at==0 || !word(text[at-1])) && (end==text.size() || !word(text[end]))))) {
            if (result.items.size() >= limit) { result.complete = false; break; }
            result.items.append({int(at),int(query.size())});
        }
        from = end;
    }
    return result;
}
DocumentSearch::DocumentSearch(Appearance *a, QWidget *parent) : QWidget(parent) {
    setObjectName("documentSearch");setFont(uiFont());
    auto *box=new QVBoxLayout(this);box->setContentsMargins(16,8,16,8);box->setSpacing(6);
    auto *line=new QHBoxLayout;line->setSpacing(8);box->addLayout(line);
    m_query=new SharedLineEdit(a,this);m_query->setObjectName("findQuery");m_query->setPlaceholderText(QStringLiteral("查找 Markdown 原文"));m_query->setAccessibleName(QStringLiteral("查找文字"));m_query->setClearButtonEnabled(true);line->addWidget(m_query,1);
    const auto button=[&](const QString &text,const QString &name,QHBoxLayout *row,auto fn){auto *b=new SharedButton(text,name,a,this);b->setOutlined(true);row->addWidget(b);connect(b,&QPushButton::clicked,this,fn);return b;};
    button(QStringLiteral("上一个"),"findPrevious",line,[this]{next(true);});button(QStringLiteral("下一个"),"findNext",line,[this]{next();});
    auto *close=button({},"findClose",line,[this]{hide();emit dismissed();});close->setSymbol("close");close->setFixedWidth(32);close->setAccessibleName(QStringLiteral("关闭查找"));
    auto *options=new QHBoxLayout;box->addLayout(options);m_case=new SharedCheckBox(QStringLiteral("区分大小写"),a,this);m_word=new SharedCheckBox(QStringLiteral("完整词"),a,this);options->addWidget(m_case);options->addWidget(m_word);options->addStretch();m_status=new QLabel(this);m_status->setObjectName("findStatus");m_status->setFont(uiFont(12));options->addWidget(m_status);
    m_replaceRow=new QWidget(this);auto *replacement=new QHBoxLayout(m_replaceRow);replacement->setContentsMargins(0,0,0,0);replacement->setSpacing(8);m_replacement=new SharedLineEdit(a,this);m_replacement->setObjectName("replaceText");m_replacement->setPlaceholderText(QStringLiteral("替换为"));m_replacement->setAccessibleName(QStringLiteral("替换文字"));replacement->addWidget(m_replacement,1);button(QStringLiteral("替换"),"replaceOne",replacement,[this]{replace(false);});button(QStringLiteral("全部替换"),"replaceAll",replacement,[this]{replace(true);});box->addWidget(m_replaceRow);m_replaceRow->hide();
    connect(m_query,&QLineEdit::textChanged,this,[this]{m_current=-1;refresh();if(!m_matches.items.isEmpty())next();});connect(m_query,&QLineEdit::returnPressed,this,[this]{next(QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier));});
    connect(m_case,&QCheckBox::toggled,this,[this]{m_current=-1;refresh();});connect(m_word,&QCheckBox::toggled,this,[this]{m_current=-1;refresh();});
    const auto theme=[this,a]{setStyleSheet(QStringLiteral("QWidget#documentSearch{background:%1;border-bottom:1px solid %2} QLabel{color:%3}").arg(a->color("surface").name(),a->color("border").name(),a->color("textSecondary").name()));};connect(a,&Appearance::changed,this,theme);theme();hide();
}
void DocumentSearch::setSession(DocumentSession *session) {
    if(m_session)disconnect(m_session,nullptr,this,nullptr);m_session=session;
    if(session){connect(session,&DocumentSession::changed,this,[this]{refresh();});connect(session,&DocumentSession::closed,this,[this]{m_session=nullptr;hide();});}refresh();
}
void DocumentSearch::open(bool replaceMode){m_replaceRow->setVisible(replaceMode);show();refresh();m_query->setFocus(Qt::ShortcutFocusReason);m_query->selectAll();}
void DocumentSearch::refresh(){
    m_snapshot=m_session?m_session->snapshot():DocumentSnapshot{};m_matches={};
    if(m_snapshot.result!=EditResult::Applied){m_status->setText(QStringLiteral("当前文档暂不可查找"));return;}
    m_matches=findText(decodeUtf8Literal(m_snapshot.utf8),m_query->text(),m_case->isChecked(),m_word->isChecked());
    if(m_current>=m_matches.items.size())m_current=-1;
    m_status->setText(m_query->text().isEmpty()?QStringLiteral("搜索原文"):m_matches.items.isEmpty()?QStringLiteral("未找到匹配"):QStringLiteral("%1 / %2%3").arg(m_current+1).arg(m_matches.items.size()).arg(m_matches.complete?QString():QStringLiteral("+，请缩小范围")));
}
void DocumentSearch::next(bool backwards){refresh();if(m_matches.items.isEmpty())return;const int count=int(m_matches.items.size());m_current=(m_current+(backwards?(m_current<0?0:-1):1)+count)%count;const auto hit=m_matches.items[m_current];refresh();emit locationRequested(hit.start,hit.start+hit.length);}
void DocumentSearch::replace(bool all){
    if(!m_session)return;const auto before=m_session->snapshot();if(before.result!=EditResult::Applied){m_status->setText(resultDescription(before.result));return;}
    if(before.revision!=m_snapshot.revision){refresh();m_status->setText(QStringLiteral("文档已变化，请重新选择匹配"));return;}
    if(m_matches.items.isEmpty())return;if(all&&!m_matches.complete){m_status->setText(QStringLiteral("匹配超过上限，请缩小范围后替换"));return;}
    const auto value=m_replacement->text();auto text=decodeUtf8Literal(before.utf8);EditResult result;
    if(all && qint64(before.utf8.size())+qint64(m_matches.items.size())*(value.toUtf8().size()-m_query->text().toUtf8().size())>DocumentSession::AdmissionSourceLimit){m_status->setText(QStringLiteral("替换结果超过文档容量上限"));return;}
    if(all){for(auto it=m_matches.items.crbegin();it!=m_matches.items.crend();++it)text.replace(it->start,it->length,value);result=m_session->replace(0,int(decodeUtf8Literal(before.utf8).size()),text,before.revision);}
    else{if(m_current<0)m_current=0;const auto hit=m_matches.items[m_current];result=m_session->replace(hit.start,hit.start+hit.length,value,before.revision);}
    refresh();if(result!=EditResult::Applied&&result!=EditResult::Unchanged)m_status->setText(resultDescription(result));else{m_current=-1;next();}
}
void DocumentSearch::keyPressEvent(QKeyEvent *event){if(event->key()==Qt::Key_Escape){hide();emit dismissed();event->accept();return;}QWidget::keyPressEvent(event);}
}
