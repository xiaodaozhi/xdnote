#include "UiFonts.h"
#include <QFontDatabase>
#include <QFontInfo>
#include <QGuiApplication>
#include <QApplication>
namespace xdnote {
namespace {
QStringList sansFamilies() {
#ifdef Q_OS_WIN
    return {QStringLiteral("Microsoft YaHei UI"), QStringLiteral("Microsoft YaHei"), QStringLiteral("Segoe UI"), QStringLiteral("Segoe UI Emoji")};
#elif defined(Q_OS_MACOS)
    return {QFontDatabase::systemFont(QFontDatabase::GeneralFont).family(), QStringLiteral("PingFang SC"), QStringLiteral("Apple Color Emoji")};
#else
    return {QFontDatabase::systemFont(QFontDatabase::GeneralFont).family(), QStringLiteral("Noto Sans CJK SC"), QStringLiteral("WenQuanYi Micro Hei"), QStringLiteral("Noto Color Emoji")};
#endif
}
QStringList monoFamilies() {
    QStringList families;
    const auto preferred = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (QFontInfo(preferred).fixedPitch()) families.append(preferred.family());
#ifdef Q_OS_WIN
    families.append({QStringLiteral("Consolas"), QStringLiteral("Cascadia Mono"), QStringLiteral("Courier New")});
#elif defined(Q_OS_MACOS)
    families.append({QStringLiteral("Menlo"), QStringLiteral("Monaco")});
#else
    families.append({QStringLiteral("DejaVu Sans Mono"), QStringLiteral("Liberation Mono"), QStringLiteral("Noto Sans Mono")});
#endif
    families.append(sansFamilies()); return families;
}
QString css(QStringList families, const QString &generic) {
    for (auto &family : families) { family.replace("'", "\\'"); family = "'" + family + "'"; }
    return families.join(',') + ',' + generic;
}
}
QFont uiFont(int pixels) { QFont result; result.setFamilies(sansFamilies()); result.setStyleHint(QFont::SansSerif); result.setPixelSize(pixels); return result; }
QFont sourceFont(int pixels) { QFont result; result.setFamilies(monoFamilies()); result.setStyleHint(QFont::Monospace); result.setPixelSize(pixels); return result; }
QString readerFontCss() { return css(sansFamilies(), "sans-serif"); }
QString sourceFontCss() { return css(monoFamilies(), "monospace"); }
void installFontFallbacks() {
    QStringList available;
    for (const auto &family : sansFamilies()) if (QFontDatabase::hasFamily(family)) available.append(family);
    // Han glyphs in the system monospace font also use the application's sans
    // fallback, instead of the platform's legacy serif fallback.
    QFontDatabase::setApplicationFallbackFontFamilies(QChar::Script_Han, available);
    QApplication::setFont(uiFont());
}
}
