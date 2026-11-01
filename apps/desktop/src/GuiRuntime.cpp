#include "GuiRuntime.h"
#include <QCoreApplication>
#include <QQuickStyle>
#include <QQuickWindow>

namespace xdnote {
void initializeGuiRuntime() {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
#ifdef Q_OS_WIN
    // D3D11 can use the system WARP adapter on hosts without a hardware GPU.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
#else
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
#endif
    QQuickStyle::setStyle(QStringLiteral("Basic"));
}
}
