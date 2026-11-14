#pragma once
#include <QString>
#include <QList>
namespace xdnote {
struct RegistrationValue { QString path,name,value; };
QList<RegistrationValue> associationValues(const QString &executable);
QString associationStatus();
QString registerFileAssociations();
bool openDefaultApplicationSettings();
}
