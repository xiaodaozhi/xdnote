#pragma once
#include <QJsonObject>
#include <QKeySequence>
class QAction;
namespace xdnote {
class Preferences;
struct CommandShortcut { QString id,title; QKeySequence standard; };
QList<CommandShortcut> commandShortcuts();
QJsonObject shortcutDefaults();
bool validShortcut(const QString &text);
bool distinctShortcuts(const QJsonObject &values);
void bindShortcut(QAction *action,Preferences *preferences,const QString &id);
}
