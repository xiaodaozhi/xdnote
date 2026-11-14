#pragma once
#include <QAbstractNativeEventFilter>
#include <QJsonObject>
#include <QMargins>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QTimer>
#include <optional>

class QWidget;
class QScreen;
class QWindow;
namespace xdnote {
inline bool normalWindowState(Qt::WindowStates state) { return !(state & (Qt::WindowMinimized | Qt::WindowMaximized | Qt::WindowFullScreen)); }
// Sizes in this section are logical client sizes, never native outer sizes.
struct WindowPlacement {
    QRect normal;
    QRect available;
    bool maximized = false;
    int normalVersion = 2;
    QJsonObject toJson() const;
    static std::optional<WindowPlacement> fromJson(const QJsonObject &value);
};
struct GeometryFit { QRect client; bool fitsMinimum = false; };
GeometryFit fitWindowClient(const QRect &requested, const QRect &available,
                            const QMargins &frame, const QSize &minimum, const QSize &maximum);
bool resemblesWindowTile(const QRect &outer, const QRect &available);
struct NativeTrackSizes { QSize minimum; QSize maximum; };
// All sizes here are device pixels, except the explicitly named logical limits.
bool nativeTrackSizes(const QSize &logicalMinimum, const QSize &logicalMaximum, quint32 dpi,
                      const QSize &nativeFrame, const NativeTrackSizes &incoming, NativeTrackSizes *output);
bool nativeDpiSize(const QSize &client, const QSize &outer, const QSize &pendingOuter,
                   quint32 currentDpi, quint32 targetDpi, const QSize &targetFrame, QSize *output);

class WindowGeometryStore final : public QObject {
    Q_OBJECT
public:
    explicit WindowGeometryStore(QObject *parent = nullptr, QString path = {});
    std::optional<WindowPlacement> placement(const QString &kind) const;
    bool save(const QString &kind, const WindowPlacement &placement);
    QString error() const { return m_error; }
private:
    QString m_path, m_error;
    QJsonObject m_entries;
    bool m_valid = true;
};

class WindowGeometry final : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
public:
    WindowGeometry(QWidget *window, WindowGeometryStore *store = nullptr, QString kind = {}, QSize compactMinimum = {});
    ~WindowGeometry() override;
    QSize preferredClientSize() const { return m_placement.normal.size(); }
    bool initialized() const { return m_initialized; }
    bool temporarilyConstrained() const { return m_constrained; }
    // Use for an explicit application/user resize intent, not a DPI notification.
    void rememberUserGeometry(const QRect &normal);
    void fitToScreen();
    bool flush();
signals:
    void geometryWarning(QString message);
protected:
    bool eventFilter(QObject *, QEvent *) override;
    bool nativeEventFilter(const QByteArray &, void *, qintptr *) override;
private:
    void initialize();
    void bindHandle();
    void watchScreen(QScreen *);
    void scheduleFit();
    void rememberPosition();
    QPointer<QWidget> m_window;
    QPointer<WindowGeometryStore> m_store;
    QPointer<QScreen> m_screen;
    QPointer<QWindow> m_handle;
    QString m_kind, m_layoutWarning;
    QSize m_minimum, m_maximum;
    QSize m_compactMinimum;
    WindowPlacement m_placement;
    QTimer m_fit, m_save;
    bool m_initialized = false, m_applying = false, m_transition = false;
    bool m_nativeMove = false, m_nativeResize = false, m_constrained = false;
    bool m_nativeStateTransition = false;
    bool m_pendingUserResize = false;
    bool m_restoreMaximized = false;
    bool m_hasSaved = false;
    bool m_restorePosition = false, m_fittedOnce = false, m_restoreNormal = false;
    bool m_explicitSizePending = false, m_returningToNormal = false;
    quint64 m_generation = 0;
};
}
