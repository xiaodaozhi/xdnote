#include "WindowGeometry.h"
#include "WindowChrome.h"
#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QResizeEvent>
#include <QScreen>
#include <QWidget>
#include <QWindow>
#include <cmath>
#include <limits>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

namespace xdnote {
namespace {
constexpr int unlimited = (1 << 24) - 1;
bool validRect(const QRect &rect) {
    return rect.width() > 0 && rect.height() > 0 && rect.width() <= 65536 && rect.height() <= 65536
        && qAbs(qint64(rect.x())) <= 1000000 && qAbs(qint64(rect.y())) <= 1000000;
}
QJsonObject rectangle(const QRect &rect) { return {{"x", rect.x()}, {"y", rect.y()}, {"width", rect.width()}, {"height", rect.height()}}; }
std::optional<QRect> rectangle(const QJsonObject &json) {
    for (const auto *key : {"x", "y", "width", "height"}) {
        const auto value = json.value(key);
        if (!value.isDouble() || !std::isfinite(value.toDouble()) || std::floor(value.toDouble()) != value.toDouble()
            || qAbs(value.toDouble()) > 1000000) return {};
    }
    QRect result(json.value("x").toInt(), json.value("y").toInt(), json.value("width").toInt(), json.value("height").toInt());
    return validRect(result) ? std::optional<QRect>(result) : std::nullopt;
}
bool scale(qint64 value, quint32 source, quint32 target, qint64 *output) {
    if (value <= 0 || value > std::numeric_limits<int>::max() || !source || !target) return false;
    const quint64 result = (quint64(value) * target + source / 2) / source;
    if (!result || result > quint64(std::numeric_limits<int>::max())) return false;
    *output = qint64(result); return true;
}
bool wayland() { return QGuiApplication::platformName().startsWith("wayland"); }
#ifdef Q_OS_WIN
bool nativeMapping(HWND hwnd, QWidget *window) {
    DWORD process = 0;
    if (!hwnd || GetWindowThreadProcessId(hwnd, &process) != GetCurrentThreadId() || process != GetCurrentProcessId()
        || !window || !normalWindowState(window->windowState()) || IsZoomed(hwnd) || IsIconic(hwnd)
        || !AreDpiAwarenessContextsEqual(GetWindowDpiAwarenessContext(hwnd), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
        || !AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
        || QGuiApplication::highDpiScaleFactorRoundingPolicy() != Qt::HighDpiScaleFactorRoundingPolicy::PassThrough
        || GetMenu(hwnd)) return false;
    const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if ((style & WS_CAPTION) != WS_CAPTION || !(style & WS_THICKFRAME) || (style & WS_CHILD)) return false;
    for (const auto *name : {"QT_SCALE_FACTOR", "QT_SCREEN_SCALE_FACTORS", "QT_USE_PHYSICAL_DPI", "QT_FONT_DPI", "QT_DEVICE_PIXEL_RATIO", "QT_AUTO_SCREEN_SCALE_FACTOR"})
        if (qEnvironmentVariableIsSet(name)) return false;
    if (qEnvironmentVariableIsSet("QT_ENABLE_HIGHDPI_SCALING")) {
        bool valid = false; const int enabled = qEnvironmentVariableIntValue("QT_ENABLE_HIGHDPI_SCALING", &valid);
        if (!valid || enabled != 1) return false;
    }
    return true;
}
bool nativeFrame(HWND hwnd, QWidget *window, UINT dpi, QSize *result) {
    if (!dpi) return false;
    // Qt Widgets owns a zero-margin client area for the custom application
    // frame. Native WS_CAPTION is retained for system commands and animation.
    if (window->windowFlags().testFlag(Qt::FramelessWindowHint)) { *result = QSize(0, 0); return true; }
    RECT rect{};
    if (!AdjustWindowRectExForDpi(&rect, DWORD(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE,
                                 DWORD(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), dpi)) return false;
    const qint64 width = qint64(rect.right) - rect.left, height = qint64(rect.bottom) - rect.top;
    if (width < 0 || height < 0 || width > INT_MAX || height > INT_MAX) return false;
    *result = QSize(int(width), int(height)); return true;
}
#endif
}
QJsonObject WindowPlacement::toJson() const { return {{"normal", rectangle(normal)}, {"available", rectangle(available)}, {"maximized", maximized}, {"normalVersion", normalVersion}}; }
std::optional<WindowPlacement> WindowPlacement::fromJson(const QJsonObject &value) {
    const auto normal = rectangle(value.value("normal").toObject()), available = rectangle(value.value("available").toObject());
    const auto revision = value.value("normalVersion"); const int version = revision.isUndefined() ? 1 : revision.toInt(0);
    if (!normal || !available || !value.value("maximized").isBool() || version < 1 || version > 2
        || (!revision.isUndefined() && (!revision.isDouble() || revision.toDouble() != version))) return {};
    return WindowPlacement{*normal, *available, value.value("maximized").toBool(), version};
}
GeometryFit fitWindowClient(const QRect &requested, const QRect &available, const QMargins &frame, const QSize &minimum, const QSize &maximum) {
    if (!validRect(requested) || !validRect(available) || frame.left() < 0 || frame.top() < 0 || frame.right() < 0 || frame.bottom() < 0
        || minimum.width() < 0 || minimum.height() < 0 || maximum.width() < minimum.width() || maximum.height() < minimum.height()) return {};
    const qint64 width = qint64(available.width()) - frame.left() - frame.right(), height = qint64(available.height()) - frame.top() - frame.bottom();
    if (width <= 0 || height <= 0) return {};
    const bool fits = width >= minimum.width() && height >= minimum.height();
    const QSize wanted = requested.size().expandedTo(minimum).boundedTo(maximum);
    const QSize size(qMax(minimum.width(), qMin(wanted.width(), int(width))), qMax(minimum.height(), qMin(wanted.height(), int(height))));
    const int left = available.x() + frame.left(), top = available.y() + frame.top();
    const int right = available.right() - frame.right() - size.width() + 1, bottom = available.bottom() - frame.bottom() - size.height() + 1;
    return {QRect(QPoint(qBound(left, requested.x(), qMax(left, right)), qBound(top, requested.y(), qMax(top, bottom))), size), fits};
}
bool resemblesWindowTile(const QRect &outer, const QRect &available) {
    if (!validRect(outer) || !validRect(available)) return false;
    const auto closeEnough = [](int a, int b) { return qAbs(a - b) <= 4; };
    if (!available.adjusted(-4, -4, 4, 4).contains(outer)) return false;
    // Native snap/tiling has no portable Qt state flag. Preserve compositor
    // arrangements touching work-area edges rather than forcing a normal size.
    return (closeEnough(outer.top(), available.top()) && closeEnough(outer.bottom(), available.bottom())
            && (closeEnough(outer.left(), available.left()) || closeEnough(outer.right(), available.right())))
        || (closeEnough(outer.left(), available.left()) && closeEnough(outer.right(), available.right())
            && (closeEnough(outer.top(), available.top()) || closeEnough(outer.bottom(), available.bottom())))
        || (closeEnough(outer.width() * 2, available.width()) && closeEnough(outer.height() * 2, available.height())
            && (closeEnough(outer.left(), available.left()) || closeEnough(outer.right(), available.right()))
            && (closeEnough(outer.top(), available.top()) || closeEnough(outer.bottom(), available.bottom())));
}
bool nativeTrackSizes(const QSize &minimum, const QSize &maximum, quint32 dpi, const QSize &frame,
                      const NativeTrackSizes &incoming, NativeTrackSizes *output) {
    if (!output || !dpi || frame.width() < 0 || frame.height() < 0) return false;
    auto result = incoming;
    const auto axis = [dpi](int min, int max, int border, int *minOut, int *maxOut) {
        if (min < 0 || min >= unlimited || max <= 0) return false;
        qint64 scaled = 0;
        if (min && (!scale(min, 96, dpi, &scaled) || scaled + border > INT_MAX)) return false;
        if (min) *minOut = int(scaled + border);
        if (max < unlimited) {
            if (!scale(qMax(1, qMax(min, max)), 96, dpi, &scaled) || scaled + border > INT_MAX) return false;
            *maxOut = int(scaled + border);
        }
        return true;
    };
    int minWidth = result.minimum.width(), minHeight = result.minimum.height(), maxWidth = result.maximum.width(), maxHeight = result.maximum.height();
    if (!axis(minimum.width(), maximum.width(), frame.width(), &minWidth, &maxWidth)
        || !axis(minimum.height(), maximum.height(), frame.height(), &minHeight, &maxHeight)) return false;
    result.minimum = QSize(minWidth, minHeight); result.maximum = QSize(maxWidth, maxHeight); *output = result; return true;
}
bool nativeDpiSize(const QSize &client, const QSize &outer, const QSize &pending, quint32 source, quint32 target, const QSize &frame, QSize *output) {
    if (!output || client.isEmpty() || outer.isEmpty() || pending.isEmpty() || frame.width() < 0 || frame.height() < 0) return false;
    qint64 width = 0, height = 0;
    if (!scale(qint64(client.width()) + pending.width() - outer.width(), source, target, &width)
        || !scale(qint64(client.height()) + pending.height() - outer.height(), source, target, &height)
        || width + frame.width() > INT_MAX || height + frame.height() > INT_MAX) return false;
    *output = QSize(int(width + frame.width()), int(height + frame.height())); return true;
}

WindowGeometryStore::WindowGeometryStore(QObject *parent, QString path) : QObject(parent), m_path(std::move(path)) {
    if (m_path.isEmpty() || !QFileInfo::exists(m_path)) return;
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) { m_valid = false; m_error = QStringLiteral("无法读取窗口尺寸设置，原配置已保留。"); return; }
    const auto document = QJsonDocument::fromJson(file.readAll());
    const auto root = document.object(); m_valid = document.isObject() && root.value("schemaVersion").toInt(-1) == 1 && root.value("windows").isObject();
    if (m_valid) { m_entries = root.value("windows").toObject(); for (auto it = m_entries.begin(); it != m_entries.end(); ++it) if (!it.value().isObject() || !WindowPlacement::fromJson(it.value().toObject())) m_valid = false; }
    if (!m_valid) m_error = QStringLiteral("窗口尺寸设置无效或版本不支持，原配置已保留。");
}
std::optional<WindowPlacement> WindowGeometryStore::placement(const QString &kind) const { return m_valid ? WindowPlacement::fromJson(m_entries.value(kind).toObject()) : std::nullopt; }
bool WindowGeometryStore::save(const QString &kind, const WindowPlacement &placement) {
    if (!m_valid) return false;
    if (kind.isEmpty() || kind.size() > 64 || !WindowPlacement::fromJson(placement.toJson())) { m_error = QStringLiteral("窗口尺寸无效，未保存设置。"); return false; }
    auto entries = m_entries; entries.insert(kind, placement.toJson());
    const auto bytes = QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"windows", entries}}).toJson();
    if (!m_path.isEmpty()) {
        QSaveFile file(m_path);
        if (bytes.size() > 65536 || !QDir().mkpath(QFileInfo(m_path).absolutePath()) || !file.open(QIODevice::WriteOnly)
            || file.write(bytes) != bytes.size() || !file.commit()) { m_error = QStringLiteral("无法记住窗口尺寸，文档内容不受影响。"); return false; }
    }
    m_entries = entries; m_error.clear(); return true;
}

WindowGeometry::WindowGeometry(QWidget *window, WindowGeometryStore *store, QString kind)
    : QObject(window), m_window(window), m_store(store), m_kind(std::move(kind)), m_minimum(window->minimumSize()), m_maximum(window->maximumSize()) {
    const auto initial = window->geometry(); m_placement.normal = initial;
    if (store) if (const auto saved = store->placement(m_kind)) { m_placement = *saved; m_restoreMaximized = saved->maximized; m_hasSaved = true; }
#ifdef Q_OS_WIN
    // Old active-window handling could save the work-area size as normal.
    // Recover that legacy case once; modern intentional work-area sizes stay.
    if (m_placement.normalVersion < 2 && m_placement.maximized && m_placement.normal.size() == m_placement.available.size()) m_placement.normal.setSize(initial.size());
#endif
    m_placement.normalVersion = 2;
    m_fit.setSingleShot(true); m_save.setSingleShot(true); m_save.setInterval(250);
    connect(&m_fit, &QTimer::timeout, this, &WindowGeometry::fitToScreen);
    connect(&m_save, &QTimer::timeout, this, [this] { flush(); });
    window->installEventFilter(this); qApp->installNativeEventFilter(this);
    if (auto *shell = qobject_cast<ApplicationWindow *>(window))
        connect(shell, &ApplicationWindow::nativeStateChangeStarting, this, [this] { m_nativeStateTransition = true; });
    connect(qApp, &QGuiApplication::screenRemoved, this, [this](QScreen *) { scheduleFit(); });
    if (window->isVisible()) QTimer::singleShot(0, this, &WindowGeometry::initialize);
}
WindowGeometry::~WindowGeometry() { if (qApp) qApp->removeNativeEventFilter(this); }
void WindowGeometry::bindHandle() {
    if (!m_window || m_handle == m_window->windowHandle()) return;
    if (m_handle) disconnect(m_handle, nullptr, this, nullptr);
    m_handle = m_window->windowHandle();
    if (m_handle) connect(m_handle, &QWindow::screenChanged, this, [this](QScreen *target) { watchScreen(target); scheduleFit(); });
}
void WindowGeometry::watchScreen(QScreen *screen) {
    if (m_screen == screen) return;
    if (m_screen) disconnect(m_screen, nullptr, this, nullptr);
    m_screen = screen;
    if (screen) {
        connect(screen, &QScreen::availableGeometryChanged, this, [this] { scheduleFit(); });
        connect(screen, &QScreen::logicalDotsPerInchChanged, this, [this] { scheduleFit(); });
    }
}
void WindowGeometry::initialize() {
    if (!m_window || m_initialized || !m_window->windowHandle()) return;
    m_initialized = true;
    auto *screen = m_window->screen();
    if (!wayland()) for (auto *candidate : QGuiApplication::screens()) if (candidate->availableGeometry() == m_placement.available) { screen = candidate; break; }
    watchScreen(screen);
    bindHandle();
    if (!wayland() && screen) m_window->windowHandle()->setScreen(screen);
    // A window can already be maximized/zoomed on its first native Show. The
    // work-area context is still required even when normal fitting is skipped.
    if (screen) m_placement.available = screen->availableGeometry();
    // Validate saved coordinates against an actual work area before moving;
    // never briefly place a restored window on a disconnected screen.
    m_restorePosition = m_hasSaved && !wayland();
    fitToScreen();
    if (m_restoreMaximized) { m_placement.maximized = true; m_window->showMaximized(); }
    if (m_store && !m_store->error().isEmpty()) emit geometryWarning(m_store->error());
}
void WindowGeometry::scheduleFit() {
    m_transition = true; ++m_generation;
    if (!m_nativeMove) m_fit.start(0);
}
void WindowGeometry::fitToScreen() {
    if (!m_window || !m_initialized || m_nativeMove) return;
    // AppKit can classify an ordinary work-area-sized fit as zoomed. An
    // explicit normal-size intent must leave that state before it is applied;
    // merely ignoring all non-normal states loses the request indefinitely.
    if (m_explicitSizePending && m_window->isMaximized() && !m_window->isFullScreen() && !m_window->isMinimized()) {
        if (!m_returningToNormal) {
            m_returningToNormal = true; m_transition = true;
            QScopedValueRollback<bool> applying(m_applying, true);
            m_window->showNormal(); m_fit.start(0);
        }
        return;
    }
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == "windows" && m_window->internalWinId()) {
        const auto hwnd = reinterpret_cast<HWND>(m_window->internalWinId());
        if (IsZoomed(hwnd) || IsIconic(hwnd)) { m_pendingUserResize = false; return; }
    }
#endif
    m_transition = false;
    if (!normalWindowState(m_window->windowState()) || !m_window->windowHandle()) { m_pendingUserResize = false; return; }
    m_returningToNormal = false;
    // A later screen notification may invalidate the queued end-of-resize
    // callback. Keep its intent until the final fitting pass consumes it.
    if (m_pendingUserResize) { rememberUserGeometry(m_window->geometry()); m_pendingUserResize = false; }
    auto *screen = m_window->screen(); if (!screen) screen = QGuiApplication::primaryScreen(); if (!screen) return;
    watchScreen(screen);
    const auto available = screen->availableGeometry();
    const auto frame = m_window->windowHandle()->frameMargins();
    if (m_fittedOnce && !m_restoreNormal && !m_constrained && !m_explicitSizePending && resemblesWindowTile(m_window->frameGeometry(), available)) return;
    const auto position = m_restorePosition ? m_placement.normal.topLeft() : m_window->geometry().topLeft();
    const auto fit = fitWindowClient(QRect(position, m_placement.normal.size()), available, frame, m_minimum, m_maximum);
    if (!fit.client.isValid()) return;
    QScopedValueRollback<bool> applying(m_applying, true);
    m_constrained = fit.client.size() != m_placement.normal.size();
    m_window->resize(fit.client.size());
    if (!wayland()) m_window->move(fit.client.topLeft() - QPoint(frame.left(), frame.top()));
    m_restorePosition = false; m_restoreNormal = false; m_nativeStateTransition = false; m_fittedOnce = true;
    if (m_explicitSizePending) m_placement.maximized = false;
    m_explicitSizePending = false;
    m_placement.available = available;
    if (!wayland()) m_placement.normal.moveTopLeft(m_window->geometry().topLeft());
    m_layoutWarning = fit.fitsMinimum ? QString() : QStringLiteral("当前屏幕可用区域小于窗口最小布局，部分内容可能无法完整显示。");
    emit geometryWarning(m_store && !m_store->error().isEmpty() ? m_store->error() : m_layoutWarning);
    m_save.start();
}
void WindowGeometry::rememberUserGeometry(const QRect &normal) {
    if (!validRect(normal)) return;
    m_placement.normal = normal;
    // A new explicit size must take precedence even if the previous temporary
    // fit happens to resemble a snapped window. Do not mark it applied early.
    m_constrained = m_window && m_window->size() != normal.size();
    m_explicitSizePending = m_constrained; m_save.start();
}
void WindowGeometry::rememberPosition() {
    if (!m_window || !m_initialized || m_applying || m_transition || m_nativeMove || !normalWindowState(m_window->windowState()) || wayland()) return;
    m_placement.normal.moveTopLeft(m_window->geometry().topLeft());
    if (m_window->screen()) m_placement.available = m_window->screen()->availableGeometry();
    m_save.start();
}
bool WindowGeometry::flush() {
    if (!m_store || !m_initialized) return true;
    if (m_store->save(m_kind, m_placement)) { emit geometryWarning(m_layoutWarning); return true; }
    emit geometryWarning(m_store->error()); return false;
}
bool WindowGeometry::eventFilter(QObject *target, QEvent *event) {
    if (!m_window || target != m_window) return false;
    if (event->type() == QEvent::Show && !m_initialized) QTimer::singleShot(0, this, &WindowGeometry::initialize);
    if (event->type() == QEvent::WinIdChange) QTimer::singleShot(0, this, [this] { bindHandle(); if (m_initialized) scheduleFit(); });
    if (event->type() == QEvent::Resize && !m_initialized && !m_hasSaved && !event->spontaneous())
        m_placement.normal.setSize(static_cast<QResizeEvent *>(event)->size());
    if (event->type() == QEvent::ScreenChangeInternal || event->type() == QEvent::DevicePixelRatioChange) scheduleFit();
    if (event->type() == QEvent::Move) rememberPosition();
    if (event->type() == QEvent::Resize && event->spontaneous() && m_initialized && !m_applying && !m_transition && !m_nativeStateTransition && !m_restoreNormal && !m_nativeMove
        && normalWindowState(m_window->windowState()) && m_window->screen()
        && !resemblesWindowTile(m_window->frameGeometry(), m_window->screen()->availableGeometry())) rememberUserGeometry(m_window->geometry());
    if (event->type() == QEvent::WindowStateChange && !m_window->isMinimized() && !m_window->isFullScreen()
        && (static_cast<QWindowStateChangeEvent *>(event)->oldState() & ~Qt::WindowActive) != (m_window->windowState() & ~Qt::WindowActive)) {
        if (!m_applying && !m_returningToNormal) { m_placement.maximized = m_window->isMaximized(); m_save.start(); }
        if (normalWindowState(m_window->windowState())) { m_restoreNormal = true; scheduleFit(); }
    }
    if (event->type() == QEvent::Hide && !m_window->isMinimized()) flush();
    return false;
}
bool WindowGeometry::nativeEventFilter(const QByteArray &, void *message, qintptr *result) {
#ifdef Q_OS_WIN
    auto *msg = static_cast<MSG *>(message);
    if (!msg || !m_window || !m_window->internalWinId() || msg->hwnd != reinterpret_cast<HWND>(m_window->internalWinId())) return false;
    const auto hwnd = msg->hwnd;
    DWORD process = 0; if (GetWindowThreadProcessId(hwnd, &process) != GetCurrentThreadId() || process != GetCurrentProcessId()) return false;
    if ((msg->message == WM_SYSCOMMAND && ((msg->wParam & 0xfff0) == SC_MAXIMIZE || (msg->wParam & 0xfff0) == SC_RESTORE || (msg->wParam & 0xfff0) == SC_MINIMIZE))
        || (msg->message == WM_SIZE && (msg->wParam != SIZE_RESTORED || !normalWindowState(m_window->windowState()))))
        m_nativeStateTransition = true;
    if (msg->message == WM_ENTERSIZEMOVE) { m_nativeMove = true; m_nativeResize = false; }
    if (msg->message == WM_SIZING) m_nativeResize = true;
    if (msg->message == WM_DPICHANGED) scheduleFit(); // Qt owns the suggested rectangle and DPI lifecycle.
    if (msg->message == WM_EXITSIZEMOVE) {
        m_pendingUserResize = m_pendingUserResize || m_nativeResize; m_nativeMove = false; m_nativeResize = false;
        const auto generation = ++m_generation;
        QTimer::singleShot(0, this, [this, generation] {
            if (!m_window || generation != m_generation) return;
            if (m_pendingUserResize && normalWindowState(m_window->windowState())) rememberUserGeometry(m_window->geometry());
            m_pendingUserResize = false;
            if (m_transition) fitToScreen(); else rememberPosition();
        });
    }
    if ((msg->message != WM_GETMINMAXINFO && msg->message != WM_GETDPISCALEDSIZE) || !result || !msg->lParam || !nativeMapping(hwnd, m_window)) return false;
    if (msg->message == WM_GETMINMAXINFO) {
        QSize frame; if (!nativeFrame(hwnd, m_window, GetDpiForWindow(hwnd), &frame)) return false;
        auto copy = *reinterpret_cast<MINMAXINFO *>(msg->lParam); NativeTrackSizes sizes;
        if (!nativeTrackSizes(m_window->minimumSize(), m_window->maximumSize(), GetDpiForWindow(hwnd), frame,
            {QSize(copy.ptMinTrackSize.x, copy.ptMinTrackSize.y), QSize(copy.ptMaxTrackSize.x, copy.ptMaxTrackSize.y)}, &sizes)) return false;
        copy.ptMinTrackSize = POINT{sizes.minimum.width(), sizes.minimum.height()}; copy.ptMaxTrackSize = POINT{sizes.maximum.width(), sizes.maximum.height()};
        *reinterpret_cast<MINMAXINFO *>(msg->lParam) = copy; *result = 0; return true;
    }
    if (msg->message == WM_GETDPISCALEDSIZE && msg->wParam <= std::numeric_limits<UINT>::max()) {
        RECT client{}, outer{}; QSize frame, output; const auto incoming = *reinterpret_cast<SIZE *>(msg->lParam);
        if (!GetClientRect(hwnd, &client) || !GetWindowRect(hwnd, &outer) || !nativeFrame(hwnd, m_window, UINT(msg->wParam), &frame)) return false;
        const qint64 width = qint64(outer.right) - outer.left, height = qint64(outer.bottom) - outer.top;
        if (width <= 0 || height <= 0 || width > INT_MAX || height > INT_MAX) return false;
        if (!nativeDpiSize(QSize(client.right - client.left, client.bottom - client.top), QSize(int(width), int(height)), QSize(incoming.cx, incoming.cy),
            GetDpiForWindow(hwnd), UINT(msg->wParam), frame, &output)) return false;
        *reinterpret_cast<SIZE *>(msg->lParam) = SIZE{output.width(), output.height()}; *result = TRUE; return true;
    }
#else
    Q_UNUSED(message); Q_UNUSED(result);
#endif
    return false;
}
}
