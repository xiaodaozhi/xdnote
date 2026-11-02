#pragma once
#include <QIcon>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QLabel>

namespace xdnote {
class Appearance;
QIcon themedIcon(const QString &name, const QColor &color);
class SharedButton final : public QPushButton {
    Q_OBJECT
    Q_PROPERTY(bool keyboardOutline READ keyboardOutline)
public:
    SharedButton(const QString &text, const QString &name, Appearance *appearance, QWidget *parent = nullptr);
    void setSymbol(const QString &name) { m_symbol = name; updateGeometry(); update(); }
    void setOutlined(bool value) { m_outlined = value; update(); }
    bool keyboardOutline() const;
    QSize sizeHint() const override;
protected:
    void paintEvent(QPaintEvent *) override;
    void focusInEvent(QFocusEvent *) override;
    void focusOutEvent(QFocusEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
private:
    QPointer<Appearance> m_appearance;
    QString m_symbol;
    bool m_tab = false, m_outlined = false;
};
class SharedMenu final : public QMenu {
public:
    explicit SharedMenu(Appearance *appearance, QWidget *parent = nullptr);
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
