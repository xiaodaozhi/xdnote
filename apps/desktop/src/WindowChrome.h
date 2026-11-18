#pragma once
#include <QWidget>
#include <QSvgRenderer>

class QHBoxLayout;
namespace xdnote {
class Appearance;
class SharedButton;
// Shared application title row. Cocoa overlays its native window controls.
class WindowTitleBar final : public QWidget {
public:
    WindowTitleBar(Appearance *appearance, QWidget *parent);
    void setTitle(const QString &title, bool dirty);
    void setCenter(QWidget *widget, int leftInset);
    void updateWindowState();
#ifdef Q_OS_MACOS
    void setNativeInsets(const QMargins &insets);
#endif
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void changeEvent(QEvent *) override;
private:
    int captionWidth() const;
    void updateCenterInsets();
    Appearance *m_appearance;
    SharedButton *m_maximize = nullptr;
    QHBoxLayout *m_row;
    QWidget *m_center = nullptr;
    int m_centerLeftInset = 0;
    QMargins m_nativeInsets;
    QSvgRenderer m_logo;
    QString m_title;
    bool m_dirty = false;
};
class ApplicationWindow : public QWidget {
    Q_OBJECT
public:
    explicit ApplicationWindow(Appearance *appearance = nullptr);
    void toggleMaximized();
    Appearance *windowAppearance() const { return m_chromeAppearance; }
signals:
    void nativeStateChangeStarting();
protected:
    void setTitleBar(WindowTitleBar *header) { m_windowHeader = header; }
    void resetNativeFrame();
    void applyNativeAppearance();
    void showEvent(QShowEvent *) override;
    void changeEvent(QEvent *) override;
    bool nativeEvent(const QByteArray &, void *, qintptr *) override;
private:
#ifdef Q_OS_MACOS
    void applyMacWindowChrome();
    QObject *m_macChromeObserver = nullptr;
#endif
    Appearance *m_chromeAppearance;
    WindowTitleBar *m_windowHeader = nullptr;
    bool m_frameInitialized = false, m_nativeMaxPressed = false;
};
}
