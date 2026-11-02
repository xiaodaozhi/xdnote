#include "GuiRuntime.h"
#include <QCoreApplication>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QGuiApplication>
#include <QStyleHints>
#include <QWebEngineUrlScheme>

namespace xdnote {
void initializeGuiRuntime() {
    QWebEngineUrlScheme preview("xdnote-preview");
    preview.setSyntax(QWebEngineUrlScheme::Syntax::Path);
    preview.setFlags(QWebEngineUrlScheme::SecureScheme | QWebEngineUrlScheme::NoAccessAllowed);
    QWebEngineUrlScheme::registerScheme(preview);
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
#ifdef Q_OS_WIN
    // D3D11 can use the system WARP adapter on hosts without a hardware GPU.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
#else
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
#endif
    QQuickStyle::setStyle(QStringLiteral("Basic"));
}
void configureGuiApplication() {
    // The product's Tab contract includes buttons on every platform. This
    // application hint does not modify the operating system's preferences.
    QGuiApplication::styleHints()->setTabFocusBehavior(Qt::TabFocusAllControls);
}
}
