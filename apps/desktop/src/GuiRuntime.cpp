#include "GuiRuntime.h"
#include "UiFonts.h"
#include <QCoreApplication>
#include <QGuiApplication>
#include <QStyleHints>

namespace xdnote {
void initializeGuiRuntime() {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
}
void configureGuiApplication() {
    installFontFallbacks();
    // The product's Tab contract includes buttons on every platform. This
    // application hint does not modify the operating system's preferences.
    QGuiApplication::styleHints()->setTabFocusBehavior(Qt::TabFocusAllControls);
}
}
