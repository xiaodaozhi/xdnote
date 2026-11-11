#include "WindowChrome.h"
#include "Appearance.h"
#include "SharedWidgets.h"
#include "VisualTokens.h"
#include "InputModality.h"
#include <QApplication>
#include <QHBoxLayout>
#include <QPainter>
#include <QMouseEvent>
#include <QShowEvent>
#include <QWindow>
#include <QTextLayout>
#include <QGlyphRun>
#include <QRawFont>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#endif

namespace xdnote {
namespace {
int titleHeight() { return visualTokens().value("windows").toObject().value("main").toObject().value("titlebarHeight").toInt(); }
}
WindowTitleBar::WindowTitleBar(Appearance *appearance, QWidget *parent) : QWidget(parent), m_appearance(appearance) {
    setFixedHeight(titleHeight()); setObjectName("documentHeader");
    auto refresh = [this] { m_logo.load(QStringLiteral(":/xdnote/design/assets/ui-v2/brand/logo-%1.svg").arg(m_appearance->scheme())); update(); };
    connect(appearance, &Appearance::changed, this, refresh); refresh();
    m_row = new QHBoxLayout(this); m_row->setContentsMargins(0, 0, 0, 0); m_row->setSpacing(0); m_row->addStretch();
#ifdef Q_OS_WIN
    auto add = [&](const QString &name, const QString &label, const QString &icon, auto callback) {
        auto *button = new SharedButton({}, name, appearance, this); button->setProperty("captionButton", true); button->setFixedSize(height(), height()); button->setSymbol(icon); button->setAccessibleName(label); button->setToolTip(label); m_row->addWidget(button); connect(button, &QPushButton::clicked, this, callback); return button;
    };
    add("captionMinimize", QStringLiteral("最小化"), "minimize", [this] { window()->showMinimized(); });
    m_maximize = add("captionMaximize", QStringLiteral("最大化或还原"), "maximize", [this] { static_cast<ApplicationWindow *>(window())->toggleMaximized(); });
    auto *close = add("captionClose", QStringLiteral("关闭窗口"), "close", [this] { window()->close(); }); close->setProperty("danger", true);
#endif
}
void WindowTitleBar::setTitle(const QString &title, bool dirty) {
    m_title = title; m_dirty = dirty; setToolTip(title); setAccessibleName("xdnote · " + title + (dirty ? QStringLiteral("，未保存") : QString())); update();
}
void WindowTitleBar::setCenter(QWidget *widget, int leftInset) {
    if (m_center || !widget) return;
    delete m_row->takeAt(0); m_row->insertSpacing(0, leftInset); m_row->insertWidget(1, widget, 1); m_center = widget; update();
}
void WindowTitleBar::updateWindowState() { if (m_maximize) { m_maximize->setSymbol(window()->isMaximized() ? "restore-window" : "maximize"); m_maximize->setAccessibleName(window()->isMaximized() ? QStringLiteral("还原窗口") : QStringLiteral("最大化窗口")); } }
int WindowTitleBar::captionWidth() const {
#ifdef Q_OS_WIN
    return 3 * titleHeight();
#else
    return 0;
#endif
}
void WindowTitleBar::paintEvent(QPaintEvent *) {
    QPainter painter(this); painter.setFont(font()); painter.setRenderHint(QPainter::Antialiasing); painter.fillRect(rect(), m_appearance->color("sidebar"));
    const qreal height = visualTokens().value("brand").toObject().value("displayHeight").toDouble();
    const qreal width = height * m_logo.viewBoxF().width() / m_logo.viewBoxF().height();
    m_logo.render(&painter, QRectF(16, (this->height() - height) / 2, width, height));
    if (!m_center) {
        painter.setPen(m_appearance->color("border")); painter.drawLine(QPointF(32 + width, 14), QPointF(32 + width, this->height() - 14));
        const qreal left = 48 + width; QFontMetricsF metrics(font());
        const auto title = metrics.elidedText(m_title, Qt::ElideMiddle, qMax(0.0, this->width() - left - 32 - captionWidth()));
        QTextLayout text(title, font()); QTextOption option; option.setWrapMode(QTextOption::NoWrap); text.setTextOption(option);
        text.beginLayout(); auto line = text.createLine(); if (line.isValid()) line.setLineWidth(qMax(0.0, this->width() - left - captionWidth())); text.endLayout();
        QRectF ink;
        for (const auto &run : text.glyphRuns()) { const auto indexes = run.glyphIndexes(); const auto positions = run.positions(); for (qsizetype i = 0; i < indexes.size(); ++i) { const auto bounds = run.rawFont().boundingRect(indexes[i]).translated(positions[i]); if (!bounds.isEmpty()) ink = ink.isNull() ? bounds : ink.united(bounds); } }
        painter.setPen(m_appearance->color("text")); text.draw(&painter, QPointF(left, this->height() / 2.0 - ink.center().y()));
        if (m_dirty) { painter.setPen(Qt::NoPen); painter.setBrush(m_appearance->semantic("warning")); painter.drawEllipse(QPointF(left + metrics.horizontalAdvance(title) + 12, this->height() / 2.0), 3, 3); }
    }
    const int border = visualTokens().value("controls").toObject().value("borderWidth").toInt(); painter.fillRect(QRect(0, this->height() - border, this->width(), border), m_appearance->color("border"));
}
void WindowTitleBar::mousePressEvent(QMouseEvent *event) { if (event->button() == Qt::LeftButton && window()->windowHandle()) window()->windowHandle()->startSystemMove(); }
void WindowTitleBar::mouseDoubleClickEvent(QMouseEvent *event) { if (event->button() == Qt::LeftButton) static_cast<ApplicationWindow *>(window())->toggleMaximized(); }
void WindowTitleBar::changeEvent(QEvent *event) { QWidget::changeEvent(event); updateWindowState(); }
ApplicationWindow::ApplicationWindow(Appearance *appearance) : m_chromeAppearance(appearance ? appearance : new Appearance(this)) {
#ifdef Q_OS_WIN
    // Qt Widgets keeps zero client margins; Win32 receives the native caption
    // style at attachment so system commands retain DWM transition behavior.
    setWindowFlag(Qt::FramelessWindowHint);
#endif
}
void ApplicationWindow::toggleMaximized() {
#ifdef Q_OS_WIN
    if (isWindow() && internalWinId() && QGuiApplication::platformName() == "windows") {
        emit nativeStateChangeStarting();
        const auto hwnd = reinterpret_cast<HWND>(internalWinId());
        SendMessageW(hwnd, WM_SYSCOMMAND, IsZoomed(hwnd) ? SC_RESTORE : SC_MAXIMIZE, 0);
        return;
    }
#endif
    isMaximized() ? showNormal() : showMaximized();
}
void ApplicationWindow::changeEvent(QEvent *event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange && isWindow()) { if (m_windowHeader) m_windowHeader->updateWindowState(); QTimer::singleShot(0, this, &ApplicationWindow::applyNativeAppearance); }
}
void ApplicationWindow::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    if (!isWindow()) return;
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() != "windows") return;
    if (m_frameInitialized) { QTimer::singleShot(0, this, &ApplicationWindow::applyNativeAppearance); return; }
    const auto hwnd = reinterpret_cast<HWND>(winId());
    DWORD process = 0; if (GetWindowThreadProcessId(hwnd, &process) != GetCurrentThreadId() || process != GetCurrentProcessId()) return;
    m_frameInitialized = true;
    RECT before{}, outer{}; GetClientRect(hwnd, &before); GetWindowRect(hwnd, &outer);
    const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    SetWindowLongPtrW(hwnd, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
    const MARGINS margins{1, 1, 1, 1}; DwmExtendFrameIntoClientArea(hwnd, &margins);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    RECT after{}; GetClientRect(hwnd, &after);
    // Compensate exactly once for the removed caption. Repeated theme/DPI
    // events must never add this native-frame delta to normal client geometry.
    SetWindowPos(hwnd, nullptr, 0, 0, outer.right - outer.left + before.right - after.right,
                 outer.bottom - outer.top + before.bottom - after.bottom, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    QTimer::singleShot(0, this, &ApplicationWindow::applyNativeAppearance);
#endif
}
void ApplicationWindow::applyNativeAppearance() {
#ifdef Q_OS_WIN
    if (!isWindow() || !m_frameInitialized || QGuiApplication::platformName() != "windows") return;
    const auto hwnd = reinterpret_cast<HWND>(internalWinId()); DWORD process = 0;
    if (GetWindowThreadProcessId(hwnd, &process) != GetCurrentThreadId() || process != GetCurrentProcessId()) return;
    if (!isFullScreen()) {
        const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE), desired = (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW;
        if (style != desired) { SetWindowLongPtrW(hwnd, GWL_STYLE, desired); SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED); }
    }
    const auto policy = DWMNCRP_ENABLED; DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
    const BOOL dark = m_chromeAppearance->dark(); DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    // Public Windows attributes; unsupported attributes safely fail on Windows 10.
    const DWORD round = 2; DwmSetWindowAttribute(hwnd, 33, &round, sizeof(round));
    const auto border = m_chromeAppearance->color("border"); const COLORREF color = RGB(border.red(), border.green(), border.blue()); DwmSetWindowAttribute(hwnd, 34, &color, sizeof(color));
    const MARGINS margins{1, 1, 1, 1}; DwmExtendFrameIntoClientArea(hwnd, &margins);
#endif
}
bool ApplicationWindow::nativeEvent(const QByteArray &type, void *message, qintptr *result) {
    if (!isWindow()) return QWidget::nativeEvent(type, message, result);
#ifdef Q_OS_WIN
    auto *msg = static_cast<MSG *>(message); const auto hwnd = reinterpret_cast<HWND>(internalWinId());
    if (msg->hwnd != hwnd) return QWidget::nativeEvent(type, message, result);
    if ((msg->message == WM_SYSCOMMAND && ((msg->wParam & 0xfff0) == SC_MAXIMIZE || (msg->wParam & 0xfff0) == SC_RESTORE || (msg->wParam & 0xfff0) == SC_MINIMIZE))
        || (msg->message == WM_SIZE && (msg->wParam != SIZE_RESTORED || windowState() != Qt::WindowNoState)))
        emit nativeStateChangeStarting();
    if (msg->message == WM_NCCALCSIZE && msg->wParam) {
        auto *params = reinterpret_cast<NCCALCSIZE_PARAMS *>(msg->lParam);
        if (IsZoomed(hwnd)) {
            // Windows places the maximized resize band outside the work area.
            // Exclude that band so the custom header stays fully visible.
            MONITORINFO monitor{sizeof(MONITORINFO)};
            if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
                RECT visible{}; if (IntersectRect(&visible, &params->rgrc[0], &monitor.rcWork)) params->rgrc[0] = visible;
            }
        }
        *result = 0; return true;
    }
    if (msg->message == WM_NCHITTEST) {
        RECT outer{}; GetWindowRect(hwnd, &outer); const int x = GET_X_LPARAM(msg->lParam), y = GET_Y_LPARAM(msg->lParam);
        POINT point{x, y}; ScreenToClient(hwnd, &point); const qreal scale = devicePixelRatioF();
        const QPoint logical(qRound(point.x / scale), qRound(point.y / scale)); auto *child = childAt(logical);
        if (child && child->property("captionButton").toBool()) { *result = child->objectName() == "captionMaximize" ? HTMAXBUTTON : HTCLIENT; return true; }
        const UINT dpi = GetDpiForWindow(hwnd);
        const int bx = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        const int by = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        if (!IsZoomed(hwnd)) {
            const bool left = x < outer.left + bx, right = x >= outer.right - bx, top = y < outer.top + by, bottom = y >= outer.bottom - by;
            if (top || bottom || left || right) { *result = top ? (left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP) : bottom ? (left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM) : left ? HTLEFT : HTRIGHT; return true; }
        }
        if (m_windowHeader && m_windowHeader->geometry().contains(logical)) {
            if (child && child->objectName() == "captionMaximize") { *result = HTMAXBUTTON; return true; }
            *result = !child || child == m_windowHeader ? HTCAPTION : HTCLIENT; return true;
        }
    }
    if (msg->message == WM_NCMOUSEMOVE || msg->message == WM_NCMOUSELEAVE) {
        if (auto *button = findChild<SharedButton *>("captionMaximize")) {
            button->setNativeHover(msg->message == WM_NCMOUSEMOVE && msg->wParam == HTMAXBUTTON);
            if (msg->message == WM_NCMOUSEMOVE) { TRACKMOUSEEVENT tracking{sizeof(TRACKMOUSEEVENT), TME_LEAVE | TME_NONCLIENT, hwnd, 0}; TrackMouseEvent(&tracking); }
        }
    }
    if (msg->message == WM_NCLBUTTONDOWN && msg->wParam == HTMAXBUTTON) {
        InputModality::forApplication()->clear();
        m_nativeMaxPressed = true; if (auto *button = findChild<SharedButton *>("captionMaximize")) { button->setDown(true); button->update(); }
        *result = 0; return true;
    }
    if ((msg->message == WM_NCLBUTTONUP || msg->message == WM_LBUTTONUP) && m_nativeMaxPressed) {
        m_nativeMaxPressed = false; if (auto *button = findChild<SharedButton *>("captionMaximize")) { button->setDown(false); button->update(); }
        if (msg->message == WM_NCLBUTTONUP && msg->wParam == HTMAXBUTTON) toggleMaximized();
        *result = 0; return true;
    }
#endif
    return QWidget::nativeEvent(type, message, result);
}

}
