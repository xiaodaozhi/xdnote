#pragma once
#include <QColor>
#include <QJsonObject>
#include <QObject>
#include "Preferences.h"

namespace xdnote {
class Appearance final : public QObject {
    Q_OBJECT
public:
    explicit Appearance(QObject *parent = nullptr, QString settingsPath = {});
    QString theme() const { return m_theme; }
    QString accent() const { return m_accent; }
    bool dark() const;
    QString scheme() const;
    QColor color(const char *name) const;
    QColor accentColor(const char *name = "solid") const;
    QColor semantic(const char *name) const;
    bool setPreferences(const QString &theme, const QString &accent);
    QString error() const { return m_error; }
    Preferences *preferences() const { return m_preferences; }
signals:
    void changed();
    void preferenceFailed(const QString &message);
private:
    QString m_theme = QStringLiteral("system"), m_accent = QStringLiteral("graphite");
    QString m_error;
    Preferences *m_preferences;
};
}
