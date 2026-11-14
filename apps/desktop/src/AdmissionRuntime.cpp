#include "AdmissionRuntime.h"
#include "GuiRuntime.h"
#include <QQuickStyle>
#include <QQuickWindow>

namespace xdnote {
void initializeAdmissionRuntime() {
    initializeGuiRuntime();
#ifdef Q_OS_WIN
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
#else
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
#endif
    QQuickStyle::setStyle(QStringLiteral("Basic"));
}
}
