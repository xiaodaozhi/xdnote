#include "Shortcuts.h"
#include "Preferences.h"
#include <QAction>
namespace xdnote {
QList<CommandShortcut> commandShortcuts(){return {
    {"openAction",QStringLiteral("打开文档"),QKeySequence::Open},{"saveAction",QStringLiteral("保存"),QKeySequence::Save},{"saveAsAction",QStringLiteral("另存为"),QKeySequence::SaveAs},{"closeAction",QStringLiteral("关闭文档"),QKeySequence::Close},{"quitAction",QStringLiteral("退出"),QKeySequence::Quit},
    {"undoAction",QStringLiteral("撤销"),QKeySequence::Undo},{"redoAction",QStringLiteral("重做"),QKeySequence::Redo},{"findAction",QStringLiteral("查找原文"),QKeySequence::Find},{"replaceAction",QStringLiteral("替换原文"),QKeySequence::Replace},
    {"focusAction",QStringLiteral("专注模式"),QKeySequence(Qt::Key_F11)},{"workspaceSearchAction",QStringLiteral("筛选工作区文件名"),QKeySequence(Qt::CTRL|Qt::Key_K)},{"fullSearchAction",QStringLiteral("工作区全文搜索"),QKeySequence(Qt::CTRL|Qt::SHIFT|Qt::Key_F)},{"favoriteDocument",QStringLiteral("收藏文档"),QKeySequence(Qt::CTRL|Qt::Key_D)},{"settingsAction",QStringLiteral("设置"),QKeySequence(Qt::CTRL|Qt::Key_Comma)}
};}
QJsonObject shortcutDefaults(){QJsonObject result;for(const auto &command:commandShortcuts())result[command.id]=command.standard.toString(QKeySequence::PortableText);return result;}
bool validShortcut(const QString &text){
    if(text.isEmpty())return true;if(text.size()>80)return false;const auto sequence=QKeySequence::fromString(text,QKeySequence::PortableText);if(sequence.isEmpty()||sequence.count()>2||sequence.toString(QKeySequence::PortableText).compare(text,Qt::CaseInsensitive)!=0)return false;
    for(int i=0;i<sequence.count();++i){const auto key=sequence[i];if(key.key()==Qt::Key_unknown||key.key()==Qt::Key_Escape||key.key()==Qt::Key_Tab||key.key()==Qt::Key_Backtab)return false;if(!(key.keyboardModifiers()&(Qt::ControlModifier|Qt::AltModifier|Qt::MetaModifier))&&(key.key()<Qt::Key_F1||key.key()>Qt::Key_F35))return false;}return true;
}
bool distinctShortcuts(const QJsonObject &values){
    QList<QKeySequence> used;for(const auto &command:commandShortcuts()){const auto value=values.value(command.id);if(!value.isString()||!validShortcut(value.toString()))return false;const auto sequence=QKeySequence::fromString(value.toString(),QKeySequence::PortableText);if(sequence.isEmpty())continue;for(const auto &prior:used)if(sequence.matches(prior)!=QKeySequence::NoMatch||prior.matches(sequence)!=QKeySequence::NoMatch)return false;used.append(sequence);}return true;
}
void bindShortcut(QAction *action,Preferences *preferences,const QString &id){
    if(!shortcutDefaults().contains(id))return;action->setProperty("commandId",id);const auto update=[action,preferences,id]{action->setShortcut(QKeySequence::fromString(preferences->value("shortcuts",id).toString(),QKeySequence::PortableText));};update();QObject::connect(preferences,&Preferences::changed,action,[update](const QString &section){if(section=="shortcuts")update();});
}
}
