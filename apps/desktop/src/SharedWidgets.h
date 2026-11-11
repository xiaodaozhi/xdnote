#pragma once
#include <QIcon>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QLabel>
#include <QDialog>
#include <QTimer>
#include <QSplitter>

namespace xdnote {
class Appearance;
class SharedMenu;
SharedMenu *createAppearanceMenu(Appearance *appearance, QWidget *parent);
class SharedSeparator final : public QWidget {
public:
    SharedSeparator(Qt::Orientation orientation, Appearance *appearance, QWidget *parent);
protected:
    void paintEvent(QPaintEvent *) override;
private:
    Appearance *m_appearance;
};
class DocumentSplitter final : public QSplitter {
public:
    DocumentSplitter(Appearance *appearance, QWidget *parent);
protected:
    QSplitterHandle *createHandle() override;
private:
    Appearance *m_appearance;
};
QIcon themedIcon(const QString &name, const QColor &color);
QIcon colorSwatch(const QColor &color);
void paintPopupShell(QPainter &painter, const QRectF &surface, const QColor &fill, const QColor &border, int radius, int shadow);
QString themedScrollBars(Appearance *appearance);
class SharedButton final : public QPushButton {
    Q_OBJECT
    Q_PROPERTY(bool keyboardOutline READ keyboardOutline)
public:
    SharedButton(const QString &text, const QString &name, Appearance *appearance, QWidget *parent = nullptr);
    void setSymbol(const QString &name) { m_symbol = name; updateGeometry(); update(); }
    void setOutlined(bool value) { m_outlined = value; update(); }
    void setNativeHover(bool value) { m_nativeHover = value; update(); }
    bool keyboardOutline() const;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
protected:
    void paintEvent(QPaintEvent *) override;
    void focusInEvent(QFocusEvent *) override;
    void focusOutEvent(QFocusEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;
private:
    QPointer<Appearance> m_appearance;
    QString m_symbol;
    bool m_tab = false, m_outlined = false, m_hover = false, m_nativeHover = false;
};
class SharedMenu final : public QMenu {
    Q_OBJECT
public:
    explicit SharedMenu(Appearance *appearance, QWidget *parent = nullptr);
    QRect surfaceRect() const;
protected:
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
private:
    QPointer<Appearance> m_appearance;
};
class SharedSpinner final : public QWidget {
    Q_OBJECT
public:
    explicit SharedSpinner(Appearance *appearance, QWidget *parent = nullptr);
    void setRunning(bool running);
    bool running() const { return m_timer.isActive(); }
protected:
    void paintEvent(QPaintEvent *) override;
    void hideEvent(QHideEvent *) override;
    void showEvent(QShowEvent *) override;
private:
    QPointer<Appearance> m_appearance;
    QTimer m_timer;
    int m_angle = 0;
    bool m_running = false;
};
class SharedDialog final : public QDialog {
    Q_OBJECT
public:
    SharedDialog(Appearance *appearance, QWidget *owner, int surfaceWidth);
    QWidget *surface() const { return m_surface; }
protected:
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
private:
    QPointer<Appearance> m_appearance;
    QWidget *m_surface;
    QPointer<QWidget> m_overlay;
};
class ElidedLabel final : public QLabel {
    Q_OBJECT
public:
    explicit ElidedLabel(QWidget *parent = nullptr);
    void setFullText(const QString &text);
    QString fullText() const { return m_full; }
protected:
    void resizeEvent(QResizeEvent *) override;
private:
    QString m_full;
};
}
