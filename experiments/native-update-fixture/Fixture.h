#pragma once
#include <QJsonObject>
#include <QString>
class QApplication;
class QNetworkAccessManager;
class QObject;
QNetworkAccessManager *fixtureNetwork(QObject *parent);
QString fixtureRoot();
QString fixtureFeed();
QString fixtureArchive();
void fixtureBoot(QApplication *application);

void fixtureNativeError(const QString &domain, qint64 code, const QString &description, const QString &reason);
