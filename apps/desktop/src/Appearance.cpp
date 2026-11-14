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
Appearance::Appearance(QObject *parent, QString settingsPath) : QObject(parent), m_preferences(new Preferences(this, std::move(settingsPath))) {
    m_theme = m_preferences->value("appearance", "theme").toString(); m_accent = m_preferences->value("appearance", "accent").toString(); m_error = m_preferences->error();
    connect(m_preferences, &Preferences::changed, this, [this](const QString &section) {
        const bool recovering = !m_error.isEmpty();
        m_error.clear();
        if (section != "appearance") { if (recovering) emit changed(); return; }
        m_theme = m_preferences->value("appearance", "theme").toString(); m_accent = m_preferences->value("appearance", "accent").toString(); emit changed();
    });
    connect(m_preferences, &Preferences::writeFailed, this, [this](const QString &message) { m_error = message; emit preferenceFailed(message); });
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] { if (m_theme == "system") emit changed(); });
}
bool Appearance::dark() const {
    return m_theme == "dark" || (m_theme == "system" && QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark);
}
QString Appearance::scheme() const { return dark() ? QStringLiteral("dark") : QStringLiteral("light"); }
QColor Appearance::color(const char *name) const { return QColor(visualTokens().value("themes").toObject().value(scheme()).toObject().value(name).toString()); }
QColor Appearance::accentColor(const char *name) const { return QColor(visualTokens().value("accents").toObject().value(m_accent).toObject().value(scheme()).toObject().value(name).toString()); }
QColor Appearance::semantic(const char *name) const { return QColor(visualTokens().value("semantic").toObject().value(scheme()).toObject().value(name).toString()); }
bool Appearance::setPreferences(const QString &theme, const QString &accent) {
    return m_preferences->setValues("appearance", {{"theme", theme}, {"accent", accent}});
}
}
