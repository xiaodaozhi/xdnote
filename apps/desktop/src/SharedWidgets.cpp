#include "SharedWidgets.h"
#include "Appearance.h"
#include "InputModality.h"
#include "VisualTokens.h"
#include <QButtonGroup>
#include <QFile>
#include <QFocusEvent>
#include <QIconEngine>
#include <QKeyEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QSvgRenderer>
#include <QTimer>

namespace xdnote {
namespace {
int control(const char *key) { return visualTokens().value("controls").toObject().value(key).toInt(); }
class SvgIcon final : public QIconEngine {
public:
    SvgIcon(QByteArray svg) : m_svg(std::move(svg)) {}
    QIconEngine *clone() const override { return new SvgIcon(m_svg); }
    void paint(QPainter *painter, const QRect &rect, QIcon::Mode, QIcon::State) override { QSvgRenderer renderer(m_svg); renderer.render(painter, QRectF(rect)); }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override { return scaledPixmap(size, mode, state, 1.0); }
    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
        QPixmap result(size * scale); result.setDevicePixelRatio(scale); result.fill(Qt::transparent);
        QPainter painter(&result); paint(&painter, QRect(QPoint(), size), mode, state); return result;
    }
private:
    QByteArray m_svg;
};
}
QIcon themedIcon(const QString &name, const QColor &color) {
    if (name == "status-dirty") return QIcon(new SvgIcon(QByteArray("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 16 16\"><circle cx=\"8\" cy=\"8\" r=\"5\" fill=\"") + color.name().toUtf8() + "\"/></svg>"));
    QFile file(QStringLiteral(":/xdnote/design/assets/ui-v2/icons/svg/%1.svg").arg(name));
    if (!file.open(QIODevice::ReadOnly)) return {};
    auto svg = file.readAll(); svg.replace("currentColor", color.name().toUtf8());
    return QIcon(new SvgIcon(std::move(svg)));
}
SharedButton::SharedButton(const QString &text, const QString &name, Appearance *appearance, QWidget *parent)
    : QPushButton(text, parent), m_appearance(appearance) {
    setObjectName(name); setFocusPolicy(Qt::StrongFocus); setAutoDefault(false);
    // Native styles can supply a class-specific button font instead of the
    // parent's explicit logical UI size (notably on macOS).
    if (parent) setFont(parent->font());
    setFixedHeight(control("height")); setMinimumWidth(control("iconButtonHit"));
    connect(appearance, &Appearance::changed, this, [this] { update(); });
    connect(InputModality::forApplication(), &InputModality::changed, this, [this] { update(); });
    connect(this, &QPushButton::pressed, this, [this] { m_tab = false; InputModality::forApplication()->clear(); update(); });
}
bool SharedButton::keyboardOutline() const {
    return m_tab && hasFocus() && isEnabled() && InputModality::forApplication()->keyboardNavigation() && window()->isActiveWindow();
}
QSize SharedButton::sizeHint() const {
    const int textWidth = text().isEmpty() ? 0 : fontMetrics().horizontalAdvance(text());
    const int iconWidth = m_symbol.isEmpty() ? 0 : control("iconSize") + (textWidth ? 8 : 0);
    return {qMax(control("iconButtonHit"), textWidth + iconWidth + (textWidth ? 24 : 0) + (menu() && textWidth ? 16 : 0)), control("height")};
}
void SharedButton::focusInEvent(QFocusEvent *event) { m_tab = event->reason() == Qt::TabFocusReason || event->reason() == Qt::BacktabFocusReason; QPushButton::focusInEvent(event); update(); }
void SharedButton::focusOutEvent(QFocusEvent *event) { m_tab = false; QPushButton::focusOutEvent(event); update(); }
void SharedButton::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_Space || event->key() == Qt::Key_Escape) {
        m_tab = false; InputModality::forApplication()->clear(); update();
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) { if (menu()) showMenu(); else click(); event->accept(); return; }
    if (isCheckable() && group() && (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right)) {
        const auto buttons = group()->buttons(); const int direction = event->key() == Qt::Key_Left ? -1 : 1;
        const int next = (buttons.indexOf(this) + buttons.size() + direction) % buttons.size();
        if (buttons[next]->isEnabled()) { buttons[next]->setFocus(Qt::OtherFocusReason); buttons[next]->click(); }
        event->accept(); return;
    }
    QPushButton::keyPressEvent(event);
}
void SharedButton::paintEvent(QPaintEvent *) {
    if (!m_appearance) return;
    auto *appearance = m_appearance.data();
    const bool solid = property("primary").toBool() || isChecked();
    QColor background = solid ? appearance->accentColor(isDown() ? "pressed" : underMouse() ? "hover" : "solid")
        : isDown() ? appearance->accentColor("soft") : underMouse() ? appearance->color("codeSurface") : QColor(Qt::transparent);
    QColor foreground = solid ? appearance->accentColor("foreground") : appearance->color("text");
    if (!isEnabled()) { background = solid ? appearance->color("border") : QColor(Qt::transparent); foreground = appearance->color("textSecondary"); }
    if (property("danger").toBool() && isEnabled()) {
        foreground = solid ? appearance->accentColor("foreground") : appearance->semantic("error");
        background = solid ? appearance->semantic("error") : underMouse() ? appearance->color("codeSurface") : QColor(Qt::transparent);
    }
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(background); painter.setPen(m_outlined ? QPen(appearance->color("border"), control("borderWidth")) : QPen(Qt::NoPen));
    const int radius = visualTokens().value("radius").toObject().value("control").toInt();
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    const int icon = m_symbol.isEmpty() ? 0 : control("iconSize"); const int label = fontMetrics().horizontalAdvance(text());
    const int dropdown = menu() && label ? 16 : 0;
    const int total = label + icon + (label && icon ? 8 : 0) + dropdown; int left = (width() - total) / 2;
    if (icon) { themedIcon(m_symbol, foreground).paint(&painter, QRect(left, (height() - icon) / 2, icon, icon)); left += icon + (label ? 8 : 0); }
    painter.setPen(foreground); painter.setFont(font()); painter.drawText(QRect(left, 0, label, height()), Qt::AlignCenter, text());
    if (dropdown) themedIcon("chevron-down", foreground).paint(&painter, QRect(left + label + 4, (height() - 12) / 2, 12, 12));
    if (keyboardOutline()) {
        painter.setBrush(Qt::NoBrush); painter.setPen(QPen(appearance->accentColor(), control("focusRingWidth")));
        painter.drawRoundedRect(QRectF(rect()).adjusted(2, 2, -2, -2), radius - 1, radius - 1);
    }
}
SharedMenu::SharedMenu(Appearance *appearance, QWidget *parent) : QMenu(parent) {
    auto font = this->font(); font.setPixelSize(control("menuFontSize")); setFont(font);
    connect(this, &QMenu::aboutToHide, this, [this] {
        InputModality::forApplication()->clear();
        QPointer<QWidget> caller(parentWidget());
        QTimer::singleShot(0, this, [caller] { if (caller && !qobject_cast<QMenu *>(caller.data())) caller->setFocus(Qt::PopupFocusReason); });
    });
    auto refresh = [this, appearance] {
        setStyleSheet(QStringLiteral("QMenu{background:%1;color:%2;border:1px solid %3;border-radius:%7px;padding:4px} QMenu::item{min-height:%8px;padding:4px 20px} QMenu::item:selected{background:%4;color:%5} QMenu::item:disabled{color:%6} QMenu::separator{height:1px;background:%3;margin:4px 8px}")
            .arg(appearance->color("surface").name(), appearance->color("text").name(), appearance->color("border").name(), appearance->accentColor("soft").name(), appearance->accentColor("softForeground").name(), appearance->color("textSecondary").name())
            .arg(visualTokens().value("radius").toObject().value("menu").toInt()).arg(control("menuRowHeight") - 8));
    };
    connect(appearance, &Appearance::changed, this, refresh); refresh();
}
ElidedLabel::ElidedLabel(QWidget *parent) : QLabel(parent) {
    setTextFormat(Qt::PlainText); setMinimumWidth(0); setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
}
void ElidedLabel::setFullText(const QString &text) {
    m_full = text; setToolTip(text); setAccessibleName(text); setText(fontMetrics().elidedText(text, Qt::ElideMiddle, qMax(0, width())));
}
void ElidedLabel::resizeEvent(QResizeEvent *event) { QLabel::resizeEvent(event); setFullText(m_full); }
}
