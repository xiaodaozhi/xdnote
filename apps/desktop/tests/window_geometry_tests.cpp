#include "WindowGeometry.h"
#include "GuiRuntime.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QScreen>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QWindow>
#include <limits>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif
using namespace xdnote;
#ifdef Q_OS_MACOS
bool macWindowIsZoomed(QWidget *window);
#endif
namespace {
void write(const QString &path, const QByteArray &bytes) { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), bytes.size()); }
QByteArray read(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
#ifdef Q_OS_WIN
struct NativeMonitor { HMONITOR handle; RECT work; UINT dpi; };
BOOL CALLBACK collectMonitor(HMONITOR handle, HDC, LPRECT, LPARAM context) {
    MONITORINFO info{sizeof(MONITORINFO)};
    if (!GetMonitorInfoW(handle, &info)) return TRUE;
    // Qt's Windows screen origins stay in native desktop coordinates, while
    // extents are logical. Match both without names or hardware identifiers.
    for (auto *screen : QGuiApplication::screens()) {
        const auto geometry = screen->geometry(); const qreal dpr = screen->devicePixelRatio();
        if (geometry.topLeft() == QPoint(info.rcMonitor.left, info.rcMonitor.top)
            && qAbs(qRound(geometry.width() * dpr) - (info.rcMonitor.right - info.rcMonitor.left)) <= 1
            && qAbs(qRound(geometry.height() * dpr) - (info.rcMonitor.bottom - info.rcMonitor.top)) <= 1) {
            reinterpret_cast<QList<NativeMonitor> *>(context)->append({handle, info.rcWork, UINT(qRound(dpr * 96))}); break;
        }
    }
    return TRUE;
}
#endif
class GeometryTests final : public QObject {
    Q_OBJECT
private slots:
    void pendingResizeIsPreservedAcrossDpi() {
        QSize result;
        QVERIFY(nativeDpiSize({1500, 900}, {1516, 939}, {1666, 1014}, 144, 96, {16, 39}, &result));
        QCOMPARE(result, QSize(1116, 689));
        QVERIFY(nativeDpiSize({1500, 900}, {1500, 900}, {1650, 975}, 144, 96, {0, 0}, &result));
        QCOMPARE(result, QSize(1100, 650));
    }
    void roundTripsDoNotAccumulate_data() {
        QTest::addColumn<int>("target");
        for (const int dpi : {96, 120, 144, 192}) QTest::newRow(qPrintable(QString::number(dpi))) << dpi;
    }
    void roundTripsDoNotAccumulate() {
        QFETCH(int, target);
        QSize size(801, 603);
        for (int i = 0; i < 50; ++i) {
            QSize scaled, restored;
            QVERIFY(nativeDpiSize(size, size, size, 96, target, {0, 0}, &scaled));
            QVERIFY(nativeDpiSize(scaled, scaled, scaled, target, 96, {0, 0}, &restored));
            QCOMPARE(restored, QSize(801, 603)); size = restored;
        }
    }
    void trackingUsesHwndDpiAndPreservesUnlimitedAxes() {
        // Qt may still report 1.5 while the HWND has already switched to 96.
        // Native constraints have no dependency on that stale screen value.
        NativeTrackSizes result;
        const NativeTrackSizes incoming{{11, 22}, {10000, 9000}};
        QVERIFY(nativeTrackSizes({560, 480}, {16777215, 16777215}, 96, {16, 39}, incoming, &result));
        QCOMPARE(result.minimum, QSize(576, 519)); QCOMPARE(result.maximum, incoming.maximum);
        QVERIFY(nativeTrackSizes({0, 480}, {16777215, 800}, 120, {20, 49}, incoming, &result));
        QCOMPARE(result.minimum, QSize(11, 649)); QCOMPARE(result.maximum, QSize(10000, 1049));
    }
    void invalidCalculationsDoNotPartiallyWrite() {
        const NativeTrackSizes original{{1, 2}, {3, 4}}; auto track = original;
        QVERIFY(!nativeTrackSizes({1, 16777215}, {16777215, 16777215}, 96, {0, 0}, original, &track));
        QCOMPARE(track.minimum, original.minimum); QCOMPARE(track.maximum, original.maximum);
        QSize output(31, 41);
        QVERIFY(!nativeDpiSize({100, 100}, {116, 139}, {1, 139}, 96, 144, {0, 0}, &output));
        QVERIFY(!nativeDpiSize({100, 100}, {100, 100}, {100, 100}, 0, 144, {0, 0}, &output));
        QVERIFY(!nativeDpiSize({INT_MAX, INT_MAX}, {INT_MAX, INT_MAX}, {INT_MAX, INT_MAX}, 1, UINT_MAX, {0, 0}, &output));
        QCOMPARE(output, QSize(31, 41));
    }
    void fittingKeepsFrameInsideNegativeWorkArea() {
        const QRect work(-1280, -40, 1280, 720), requested(-1600, -400, 1000, 800);
        const auto fit = fitWindowClient(requested, work, {8, 31, 8, 8}, {560, 480}, {16777215, 16777215});
        QVERIFY(fit.fitsMinimum); QCOMPARE(fit.client.size(), QSize(1000, 681));
        QVERIFY(work.contains(fit.client.adjusted(-8, -31, 8, 8)));
        const auto restored = fitWindowClient(QRect(fit.client.topLeft(), requested.size()), {-1920, -100, 1920, 1080}, {8, 31, 8, 8}, {560, 480}, {16777215, 16777215});
        QCOMPARE(restored.client.size(), requested.size());
    }
    void insufficientLayoutIsExplicit() {
        const auto fit = fitWindowClient({0, 0, 800, 820}, {0, 0, 320, 240}, {}, {560, 480}, {16777215, 16777215});
        QVERIFY(!fit.fitsMinimum); QCOMPARE(fit.client.size(), QSize(560, 480)); QCOMPARE(fit.client.topLeft(), QPoint(0, 0));
        QVERIFY(!fitWindowClient({0, 0, 800, 820}, {0, 0, 5, 5}, {8, 31, 8, 8}, {560, 480}, {16777215, 16777215}).client.isValid());
    }
    void tiledArrangementsAreNotNormalWindows() {
        const QRect work(-1920, 0, 1920, 1040);
        QVERIFY(resemblesWindowTile({-1920, 0, 960, 1040}, work));
        QVERIFY(resemblesWindowTile({-960, 0, 960, 1040}, work));
        QVERIFY(resemblesWindowTile({-1920, 0, 1920, 520}, work));
        QVERIFY(resemblesWindowTile({-1920, 0, 960, 520}, work));
        QVERIFY(resemblesWindowTile({-960, 520, 960, 520}, work));
        QVERIFY(!resemblesWindowTile({-1920, 0, 2400, 1040}, work));
        QVERIFY(!resemblesWindowTile({-1800, 100, 800, 820}, work));
    }
    void storeRoundTripAndMalformedPreservation() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path = dir.filePath("windows.json");
        const WindowPlacement first{{-1200, 50, 801, 603}, {-1280, 0, 1280, 1000}, true};
        WindowGeometryStore store(nullptr, path); QVERIFY(store.save("standalone", first));
        WindowGeometryStore reopened(nullptr, path); const auto restored = reopened.placement("standalone"); QVERIFY(restored);
        QCOMPARE(restored->normal, first.normal); QCOMPARE(restored->available, first.available); QVERIFY(restored->maximized);
        write(path, "{\"schemaVersion\":999,\"windows\":{}}"); const auto original = read(path);
        WindowGeometryStore invalid(nullptr, path); QVERIFY(!invalid.placement("standalone")); QVERIFY(!invalid.error().isEmpty());
        QVERIFY(!invalid.save("standalone", first)); QCOMPARE(read(path), original);
        auto malformed = first.toJson(); malformed.insert("normal", QJsonObject{{"x", 0}, {"y", 0}, {"width", 1.5}, {"height", 100}});
        QVERIFY(!WindowPlacement::fromJson(malformed));
    }
    void failedPersistenceDoesNotPublishNewState() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto parent = dir.filePath("occupied"); write(parent, "original bytes");
        WindowGeometryStore store(nullptr, parent + "/windows.json");
        QVERIFY(!store.save("standalone", {{0, 0, 800, 820}, {0, 0, 1920, 1040}, false}));
        QVERIFY(!store.placement("standalone")); QVERIFY(!store.error().isEmpty()); QCOMPARE(read(parent), QByteArray("original bytes"));
    }
    void temporaryFitDoesNotOverwritePreferenceAndSpecialStates() {
        WindowGeometryStore store; QWidget window; window.setMinimumSize(200, 150); window.resize(400, 300);
        WindowGeometry controller(&window, &store, "fixture"); window.show();
        QTRY_VERIFY(window.windowHandle()); QTest::qWait(20);
        const auto work = window.screen()->availableGeometry(); const QSize preferred(work.width() + 200, work.height() + 200);
        controller.rememberUserGeometry(QRect(window.geometry().topLeft(), preferred)); controller.fitToScreen();
        QCOMPARE(controller.preferredClientSize(), preferred); QVERIFY(controller.temporarilyConstrained()); QVERIFY(controller.flush());
        QCOMPARE(store.placement("fixture")->normal.size(), preferred);
        controller.rememberUserGeometry(QRect(window.geometry().topLeft(), QSize(350, 250))); controller.fitToScreen();
        QTRY_COMPARE(window.size(), QSize(350, 250));
        QCOMPARE(controller.preferredClientSize(), QSize(350, 250)); QVERIFY(!controller.temporarilyConstrained());
        QTRY_VERIFY(window.windowState() == Qt::WindowNoState);
        QVERIFY(controller.flush()); QVERIFY(!store.placement("fixture")->maximized);
        window.showMaximized();
#ifdef Q_OS_MACOS
        QTRY_VERIFY_WITH_TIMEOUT(macWindowIsZoomed(&window), 5000);
#else
        QTRY_VERIFY(window.isMaximized());
#endif
        controller.fitToScreen();
        QCOMPARE(controller.preferredClientSize(), QSize(350, 250));
        qInfo() << "restore-from-maximized" << window.windowState() << window.size() << "preferred" << controller.preferredClientSize();
        window.showNormal(); QTRY_COMPARE(window.size(), QSize(350, 250));
        for (int i = 0; i < 10; ++i) controller.fitToScreen();
        QCOMPARE(window.size(), QSize(350, 250));
    }
    void restoredWindowUsesLogicalClientAndReachablePosition() {
        WindowGeometryStore store;
        QVERIFY(store.save("fixture", {{900000, -900000, 411, 311}, {900000, -900000, 1920, 1080}, false}));
        QWidget window; window.setMinimumSize(200, 150); window.resize(300, 200);
        WindowGeometry controller(&window, &store, "fixture"); window.show();
        QTRY_COMPARE(window.size(), QSize(411, 311));
        const auto work = window.screen()->availableGeometry();
        if (!QGuiApplication::platformName().startsWith("wayland")) QTRY_VERIFY(work.contains(window.frameGeometry()));
        QVERIFY(controller.flush()); QCOMPARE(store.placement("fixture")->normal.size(), QSize(411, 311));
    }
    void explicitSizeBeforeFirstShowIsPreserved() {
        QWidget window; window.setMinimumSize(100, 100); window.resize(300, 200);
        WindowGeometry controller(&window); window.resize(421, 321); window.show();
        QTRY_COMPARE(window.size(), QSize(421, 321));
        QTest::qWait(20); QCOMPARE(controller.preferredClientSize(), QSize(421, 321));
    }
    void fullscreenAndMinimizationPreserveNormalAndMaximizedPreference() {
        WindowGeometryStore store; QWidget window; window.setMinimumSize(100, 100); window.resize(411, 311);
        WindowGeometry controller(&window, &store, "fixture"); window.show(); QTest::qWait(30);
        window.showMaximized(); QTRY_VERIFY(window.isMaximized());
#ifdef Q_OS_MACOS
        QTRY_VERIFY_WITH_TIMEOUT(macWindowIsZoomed(&window), 5000);
#endif
        QVERIFY(controller.flush());
        QVERIFY(store.placement("fixture")->maximized);
        window.showFullScreen(); QTRY_VERIFY(window.isFullScreen()); QVERIFY(controller.flush());
        QVERIFY(store.placement("fixture")->maximized); QCOMPARE(controller.preferredClientSize(), QSize(411, 311));
        window.showMinimized(); QTRY_VERIFY(window.isMinimized()); QVERIFY(controller.flush());
        QVERIFY(store.placement("fixture")->maximized); QCOMPARE(controller.preferredClientSize(), QSize(411, 311));
        window.showNormal(); QTRY_COMPARE(window.size(), QSize(411, 311));
    }
    void persistenceFeedbackClearsAfterSuccessfulRetry() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto occupied = dir.filePath("occupied"); write(occupied, "fixture");
        WindowGeometryStore store(nullptr, occupied + "/windows.json"); QWidget window; window.resize(411, 311);
        WindowGeometry controller(&window, &store, "fixture"); QSignalSpy feedback(&controller, &WindowGeometry::geometryWarning);
        window.show(); QTest::qWait(30); QVERIFY(!controller.flush()); QVERIFY(!feedback.isEmpty()); QVERIFY(!feedback.last().at(0).toString().isEmpty());
        QVERIFY(QFile::remove(occupied)); QVERIFY(controller.flush()); QVERIFY(feedback.last().at(0).toString().isEmpty());
        QCOMPARE(store.placement("fixture")->normal.size(), QSize(411, 311));
    }
#ifdef Q_OS_WIN
    void nativeMessagesTouchOnlyOwnedOrdinaryWindows() {
        QWidget window; window.setMinimumSize(201, 151); window.setMaximumSize(1001, 701); window.resize(401, 301);
        WindowGeometry controller(&window); window.show(); QTest::qWait(30);
        const auto hwnd = reinterpret_cast<HWND>(window.winId()); const auto dpi = GetDpiForWindow(hwnd);
        const auto evidenceRoot = qEnvironmentVariable("XDNOTE_TEST_EVIDENCE");
        if (!evidenceRoot.isEmpty()) {
            const auto work = window.screen()->availableGeometry(); RECT nativeClient{}, nativeOuter{};
            QVERIFY(GetClientRect(hwnd, &nativeClient)); QVERIFY(GetWindowRect(hwnd, &nativeOuter));
            const QJsonObject evidence{{"scope", "owned-native-window-not-physical-drag"}, {"qtVersion", qVersion()},
                {"hwndDpi", int(dpi)}, {"windowDpr", window.windowHandle()->devicePixelRatio()}, {"screenDpr", window.screen()->devicePixelRatio()},
                {"screenCount", int(QGuiApplication::screens().size())}, {"logicalWidth", window.width()}, {"logicalHeight", window.height()},
                {"nativeClientWidth", int(nativeClient.right - nativeClient.left)}, {"nativeClientHeight", int(nativeClient.bottom - nativeClient.top)},
                {"nativeOuterWidth", int(nativeOuter.right - nativeOuter.left)}, {"nativeOuterHeight", int(nativeOuter.bottom - nativeOuter.top)},
                {"availableWidth", work.width()}, {"availableHeight", work.height()}};
            QVERIFY(QDir().mkpath(evidenceRoot + "/geometry")); write(evidenceRoot + "/geometry/native-window.json", QJsonDocument(evidence).toJson());
        }
        RECT frame{}; QVERIFY(AdjustWindowRectExForDpi(&frame, DWORD(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE, DWORD(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), dpi));
        MINMAXINFO info{}; info.ptMaxPosition = {71, 73}; info.ptMaxSize = {900, 800}; info.ptReserved = {41, 43};
        SendMessageW(hwnd, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&info));
        QCOMPARE(info.ptMinTrackSize.x, MulDiv(201, dpi, 96) + frame.right - frame.left);
        QCOMPARE(info.ptMinTrackSize.y, MulDiv(151, dpi, 96) + frame.bottom - frame.top);
        QCOMPARE(info.ptMaxPosition.x, 71L); QCOMPARE(info.ptMaxPosition.y, 73L); QCOMPARE(info.ptMaxSize.x, 900L); QCOMPARE(info.ptReserved.x, 41L);
        RECT client{}, outer{}; QVERIFY(GetClientRect(hwnd, &client)); QVERIFY(GetWindowRect(hwnd, &outer));
        SIZE pending{outer.right - outer.left + 40, outer.bottom - outer.top + 20}; const auto input = pending;
        QCOMPARE(SendMessageW(hwnd, WM_GETDPISCALEDSIZE, dpi, reinterpret_cast<LPARAM>(&pending)), LRESULT(TRUE));
        QCOMPARE(pending.cx, input.cx); QCOMPARE(pending.cy, input.cy);
        // A second HWND without a controller retains normal platform handling.
        QWidget other; other.resize(301, 201); other.show(); QTest::qWait(10);
        SIZE untouched{333, 222}; MSG unrelated{}; unrelated.hwnd = reinterpret_cast<HWND>(other.winId()); unrelated.message = WM_GETDPISCALEDSIZE;
        unrelated.wParam = dpi; unrelated.lParam = reinterpret_cast<LPARAM>(&untouched); qintptr callbackResult = 123;
        auto *filter = static_cast<QAbstractNativeEventFilter *>(&controller);
        QVERIFY(!filter->nativeEventFilter("windows_generic_MSG", &unrelated, &callbackResult));
        QCOMPARE(callbackResult, qintptr(123));
        QCOMPARE(untouched.cx, 333L); QCOMPARE(untouched.cy, 222L);
        const auto oldScale = qgetenv("QT_SCALE_FACTOR"); qputenv("QT_SCALE_FACTOR", "1"); pending = input;
        MSG overridden{}; overridden.hwnd = hwnd; overridden.message = WM_GETDPISCALEDSIZE; overridden.wParam = dpi; overridden.lParam = reinterpret_cast<LPARAM>(&pending);
        const auto handled = filter->nativeEventFilter("windows_generic_MSG", &overridden, &callbackResult);
        if (oldScale.isNull()) qunsetenv("QT_SCALE_FACTOR"); else qputenv("QT_SCALE_FACTOR", oldScale);
        QVERIFY(!handled); QCOMPARE(pending.cx, input.cx); QCOMPARE(pending.cy, input.cy);
        // Deterministic ordering regression, not a physical DPI migration:
        // a late DPI notification must not discard an actual resize intent.
        MSG sequence{}; sequence.hwnd = hwnd; sequence.message = WM_ENTERSIZEMOVE;
        filter->nativeEventFilter("windows_generic_MSG", &sequence, &callbackResult);
        sequence.message = WM_SIZING; filter->nativeEventFilter("windows_generic_MSG", &sequence, &callbackResult);
        window.resize(501, 351);
        sequence.message = WM_EXITSIZEMOVE; filter->nativeEventFilter("windows_generic_MSG", &sequence, &callbackResult);
        sequence.message = WM_DPICHANGED; filter->nativeEventFilter("windows_generic_MSG", &sequence, &callbackResult);
        QTRY_COMPARE(controller.preferredClientSize(), QSize(501, 351));
    }
    void mixedDpiNativeRoundTripsWhenDesktopProvidesThem() {
        QList<NativeMonitor> monitors;
        QVERIFY(EnumDisplayMonitors(nullptr, nullptr, collectMonitor, reinterpret_cast<LPARAM>(&monitors)));
        QSet<UINT> dpis; for (const auto &monitor : monitors) dpis.insert(monitor.dpi);
        QSet<int> qtDpis; QJsonArray qtDpiEvidence;
        for (auto *screen : QGuiApplication::screens()) { const int dpi = qRound(screen->devicePixelRatio() * 96); qtDpis.insert(dpi); qtDpiEvidence.append(dpi); }
        const auto evidenceRoot = qEnvironmentVariable("XDNOTE_TEST_EVIDENCE");
        auto saveEvidence = [&](QJsonObject value) {
            value.insert("qtScreenDpis", qtDpiEvidence); value.insert("matchedNativeScreens", int(monitors.size()));
            if (!evidenceRoot.isEmpty()) { QVERIFY(QDir().mkpath(evidenceRoot + "/geometry")); write(evidenceRoot + "/geometry/native-roundtrip.json", QJsonDocument(value).toJson()); }
        };
        if (qtDpis.size() >= 2 && dpis.size() < 2 && !qEnvironmentVariableIsSet("QT_SCALE_FACTOR") && !qEnvironmentVariableIsSet("QT_SCREEN_SCALE_FACTORS")) {
            saveEvidence({{"scope", "native-programmatic-not-mouse-drag"}, {"executed", false}, {"reason", "qt-native-screen-mapping-incomplete"}});
            QFAIL("Qt reports mixed DPI but native screen mapping is incomplete");
        }
        if (dpis.size() < 2 || qEnvironmentVariableIsSet("QT_SCALE_FACTOR") || qEnvironmentVariableIsSet("QT_SCREEN_SCALE_FACTORS")) {
            saveEvidence({{"scope", "native-programmatic-not-mouse-drag"}, {"executed", false}, {"reason", "mixed-dpi-desktop-unavailable-or-overridden"}});
            QSKIP("Real mixed-DPI desktop is unavailable or scaling is overridden; no native migration executed");
        }
        const auto first = monitors.first(); auto second = first;
        for (const auto &monitor : monitors) if (monitor.dpi != first.dpi) { second = monitor; break; }
        QWidget window; window.setMinimumSize(201, 151); window.resize(401, 301);
        WindowGeometry controller(&window); window.show(); QTest::qWait(30);
        const auto hwnd = reinterpret_cast<HWND>(window.winId()); QJsonArray samples;
        int step = 0;
        for (int round = 0; round < 3; ++round) for (const auto &target : {first, second, first}) {
            RECT outer{}; QVERIFY(GetWindowRect(hwnd, &outer));
            const int workWidth = target.work.right - target.work.left, workHeight = target.work.bottom - target.work.top;
            const int outerWidth = int(outer.right - outer.left), outerHeight = int(outer.bottom - outer.top);
            const int x = target.work.left + qMax(0, (workWidth - outerWidth) / 2);
            const int y = target.work.top + qMax(0, (workHeight - outerHeight) / 2);
            QVERIFY(SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE));
            QTRY_VERIFY_WITH_TIMEOUT(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) == target.handle, 5000);
            QTRY_COMPARE_WITH_TIMEOUT(GetDpiForWindow(hwnd), target.dpi, 5000);
            QTRY_COMPARE_WITH_TIMEOUT(window.devicePixelRatioF(), qreal(target.dpi) / 96, 5000);
            QTRY_COMPARE_WITH_TIMEOUT(window.size(), QSize(401, 301), 5000);
            for (int sample = 0; sample < 5; ++sample) {
                QTest::qWait(20); QCOMPARE(window.size(), QSize(401, 301)); QCOMPARE(controller.preferredClientSize(), QSize(401, 301));
            }
            RECT client{}; QVERIFY(GetClientRect(hwnd, &client)); QVERIFY(GetWindowRect(hwnd, &outer));
            QVERIFY(qAbs(client.right - client.left - MulDiv(401, target.dpi, 96)) <= 1);
            QVERIFY(qAbs(client.bottom - client.top - MulDiv(301, target.dpi, 96)) <= 1);
            samples.append(QJsonObject{{"step", step++}, {"dpi", int(GetDpiForWindow(hwnd))}, {"windowDpr", window.devicePixelRatioF()},
                {"logicalWidth", window.width()}, {"logicalHeight", window.height()}, {"nativeClientWidth", int(client.right - client.left)}, {"nativeClientHeight", int(client.bottom - client.top)}});
        }
        saveEvidence({{"scope", "native-programmatic-not-mouse-drag"}, {"executed", true}, {"samples", samples}});
    }
#endif
};
}
int main(int argc, char **argv) {
    initializeGuiRuntime(); QApplication app(argc, argv); configureGuiApplication(); app.setQuitOnLastWindowClosed(false);
    GeometryTests tests; return QTest::qExec(&tests, argc, argv);
}
#include "window_geometry_tests.moc"
