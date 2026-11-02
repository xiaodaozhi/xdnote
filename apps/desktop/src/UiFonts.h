#pragma once
#include <QFont>
#include <QString>
namespace xdnote {
QFont uiFont(int pixels = 14);
QFont sourceFont(int pixels = 14);
QString readerFontCss();
QString sourceFontCss();
void installFontFallbacks();
}
