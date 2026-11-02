#pragma once
#include <QColor>
#include <QJsonObject>
#include <QObject>

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
signals:
    void changed();
    void preferenceFailed(const QString &message);
private:
    bool fail(const QString &message);
    QString m_theme = QStringLiteral("system"), m_accent = QStringLiteral("graphite");
    QString m_path, m_error;
    QJsonObject m_settings;
    bool m_storageValid = true;
};
}
