#pragma once
#include <QWidget>
#include <QSvgRenderer>

class QHBoxLayout;
namespace xdnote {
class Appearance;
class SharedButton;
// Shared application title row; platform decorations remain native off Windows.
class WindowTitleBar final : public QWidget {
public:
    WindowTitleBar(Appearance *appearance, QWidget *parent);
    void setTitle(const QString &title, bool dirty);
    void setCenter(QWidget *widget, int leftInset);
    void updateWindowState();
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void changeEvent(QEvent *) override;
private:
    int captionWidth() const;
    Appearance *m_appearance;
    SharedButton *m_maximize = nullptr;
    QHBoxLayout *m_row;
    QWidget *m_center = nullptr;
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
    void resetNativeFrame() { m_frameInitialized = false; m_nativeMaxPressed = false; }
    void applyNativeAppearance();
    void showEvent(QShowEvent *) override;
    void changeEvent(QEvent *) override;
    bool nativeEvent(const QByteArray &, void *, qintptr *) override;
private:
    Appearance *m_chromeAppearance;
    WindowTitleBar *m_windowHeader = nullptr;
    bool m_frameInitialized = false, m_nativeMaxPressed = false;
};
}
