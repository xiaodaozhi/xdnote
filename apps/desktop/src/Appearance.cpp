#include "Appearance.h"
#include "VisualTokens.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStyleHints>

namespace xdnote {
namespace {
bool validTheme(const QString &theme) { return theme == "system" || theme == "light" || theme == "dark"; }
}
Appearance::Appearance(QObject *parent, QString settingsPath) : QObject(parent), m_path(std::move(settingsPath)) {
    if (!m_path.isEmpty() && QFileInfo::exists(m_path)) {
        QFile file(m_path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) {
            m_storageValid = false; m_error = QStringLiteral("无法读取外观设置，原配置已保留");
        } else {
            const auto document = QJsonDocument::fromJson(file.readAll());
            m_storageValid = document.isObject() && document.object().value("schemaVersion").toInt() == 1;
            if (!m_storageValid) m_error = QStringLiteral("外观设置格式无效或版本不支持，原配置已保留");
            else {
                m_settings = document.object();
                const auto appearance = m_settings.value("appearance").toObject();
                const auto theme = appearance.value("theme").toString(), accent = appearance.value("accent").toString();
                if (validTheme(theme)) m_theme = theme;
                if (visualTokens().value("accents").toObject().contains(accent)) m_accent = accent;
            }
        }
    }
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        if (m_theme == "system") emit changed();
    });
}
bool Appearance::dark() const {
    return m_theme == "dark" || (m_theme == "system" && QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark);
}
QString Appearance::scheme() const { return dark() ? QStringLiteral("dark") : QStringLiteral("light"); }
QColor Appearance::color(const char *name) const { return QColor(visualTokens().value("themes").toObject().value(scheme()).toObject().value(name).toString()); }
QColor Appearance::accentColor(const char *name) const { return QColor(visualTokens().value("accents").toObject().value(m_accent).toObject().value(scheme()).toObject().value(name).toString()); }
QColor Appearance::semantic(const char *name) const { return QColor(visualTokens().value("semantic").toObject().value(scheme()).toObject().value(name).toString()); }
bool Appearance::fail(const QString &message) { m_error = message; emit preferenceFailed(message); return false; }
bool Appearance::setPreferences(const QString &theme, const QString &accent) {
    if (!validTheme(theme) || !visualTokens().value("accents").toObject().contains(accent)) return fail(QStringLiteral("外观选项无效"));
    if (!m_storageValid) return fail(m_error);
    if (m_theme == theme && m_accent == accent) return true;
    auto settings = m_settings;
    auto appearance = settings.value("appearance").toObject();
    appearance.insert("theme", theme); appearance.insert("accent", accent);
    settings.insert("appearance", appearance); settings.insert("schemaVersion", 1);
    if (!m_path.isEmpty()) {
        if (!QDir().mkpath(QFileInfo(m_path).path())) return fail(QStringLiteral("无法创建设置目录，外观未更改"));
        QSaveFile file(m_path); const auto bytes = QJsonDocument(settings).toJson();
        if (bytes.size() > 65536) return fail(QStringLiteral("设置文件过大，外观未更改"));
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            return fail(QStringLiteral("无法保存外观设置，外观未更改：") + file.errorString());
    }
    m_settings = settings; m_theme = theme; m_accent = accent; m_error.clear(); emit changed(); return true;
}
}
