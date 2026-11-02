#include "SharedWidgets.h"
#include "Appearance.h"
#include "InputModality.h"
#include "VisualTokens.h"
#include "UiFonts.h"
#include <QButtonGroup>
#include <QFile>
#include <QFocusEvent>
#include <QIconEngine>
#include <QKeyEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QSvgRenderer>
#include <QTimer>
#include <QScreen>
#include <QShowEvent>
#include <QVBoxLayout>

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
QIcon colorSwatch(const QColor &color) {
    return QIcon(new SvgIcon(QByteArray("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 16 16\"><rect x=\"1\" y=\"1\" width=\"14\" height=\"14\" rx=\"4\" fill=\"") + color.name().toUtf8() + "\"/></svg>"));
}
void paintPopupShell(QPainter &painter, const QRectF &surface, const QColor &fill, const QColor &border, int radius, int shadow) {
    painter.setRenderHint(QPainter::Antialiasing); painter.setPen(Qt::NoPen);
    // Logical-coordinate shadow stays inside a transparent window on every DPR.
    // It does not rely on native popup effects or device-pixel widget masks.
    for (int spread = shadow; spread > 0; --spread) {
        const int alpha = 2 + (shadow - spread) * 8 / qMax(1, shadow);
        painter.setBrush(QColor(0, 0, 0, alpha));
        painter.drawRoundedRect(surface.adjusted(-spread, -spread, spread, spread).translated(0, 2), radius + spread, radius + spread);
    }
    painter.setBrush(fill); painter.setPen(QPen(border, 1));
    painter.drawRoundedRect(surface.adjusted(.5, .5, -.5, -.5), radius, radius);
}
SharedButton::SharedButton(const QString &text, const QString &name, Appearance *appearance, QWidget *parent)
    : QPushButton(text, parent), m_appearance(appearance) {
    setObjectName(name); setFocusPolicy(Qt::StrongFocus); setAutoDefault(false);
    setAttribute(Qt::WA_MacShowFocusRect, false);
    // Native styles can supply a class-specific button font instead of the
    // parent's explicit logical UI size (notably on macOS).
    if (parent) setFont(parent->font());
    setFixedHeight(control("height")); setMinimumWidth(control("iconButtonHit"));
    connect(appearance, &Appearance::changed, this, [this] { update(); });
    connect(InputModality::forApplication(), &InputModality::changed, this, [this] { update(); });
    connect(this, &QPushButton::pressed, this, [this] { m_tab = false; InputModality::forApplication()->clear(); update(); });
}
QString themedScrollBars(Appearance *appearance) {
    return QStringLiteral("QScrollBar{background:%1;border:0;margin:0} QScrollBar:vertical{width:%3px} QScrollBar:horizontal{height:%3px} QScrollBar::handle{background:%2;border:%4px solid %1;border-radius:%5px} QScrollBar::handle:vertical{min-height:%6px} QScrollBar::handle:horizontal{min-width:%6px} QScrollBar::add-line,QScrollBar::sub-line{width:0;height:0;border:0;background:transparent} QScrollBar::add-page,QScrollBar::sub-page{background:transparent} QAbstractScrollArea::corner{background:%1;border:0}")
        .arg(appearance->color("background").name(), appearance->color("textSecondary").name())
        .arg(control("scrollbarWidth")).arg(control("scrollbarInset")).arg(control("scrollbarWidth") / 2).arg(control("scrollbarHandleMin"));
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
    if (objectName() == "captionClose" && isEnabled()) {
        background = underMouse() || isDown() ? m_appearance->semantic("error") : QColor(Qt::transparent);
        foreground = underMouse() || isDown() ? QColor(Qt::white) : m_appearance->color("text");
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
SharedMenu::SharedMenu(Appearance *appearance, QWidget *parent) : QMenu(parent), m_appearance(appearance) {
    setWindowFlags(Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground); setAttribute(Qt::WA_NoSystemBackground);
    setFont(uiFont(control("menuFontSize")));
    connect(this, &QMenu::aboutToHide, this, [this] {
        InputModality::forApplication()->clear();
        QPointer<QWidget> caller(parentWidget());
        QTimer::singleShot(0, this, [caller] { if (caller && !qobject_cast<QMenu *>(caller.data())) caller->setFocus(Qt::PopupFocusReason); });
    });
    auto refresh = [this, appearance] {
        setStyleSheet(QStringLiteral("QMenu{background:transparent;color:%1;border:0;margin:12px;padding:4px} QMenu::item{min-height:%6px;padding:4px 24px 4px 8px;border-radius:4px;background:transparent} QMenu::item:selected{background:%3;color:%4} QMenu::item:disabled{color:%5} QMenu::separator{height:1px;background:%2;margin:4px 8px} QMenu::indicator{width:16px;height:16px} QMenu::icon{padding:0 6px}")
            .arg(appearance->color("text").name(), appearance->color("border").name(), appearance->accentColor("soft").name(), appearance->accentColor("softForeground").name(), appearance->color("textSecondary").name()).arg(control("menuRowHeight") - 8));
    };
    connect(appearance, &Appearance::changed, this, refresh); refresh();
}
QRect SharedMenu::surfaceRect() const { return rect().adjusted(12, 12, -12, -12); }
void SharedMenu::paintEvent(QPaintEvent *event) {
    { QPainter painter(this); painter.setCompositionMode(QPainter::CompositionMode_Source); painter.fillRect(rect(), Qt::transparent); painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
      paintPopupShell(painter, surfaceRect(), m_appearance->color("surface"), m_appearance->color("border"), 8, 10); }
    QMenu::paintEvent(event);
}
void SharedMenu::showEvent(QShowEvent *event) {
    QMenu::showEvent(event);
    // Keep the actual surface (rather than its shadow margin) aligned to the
    // invoking control. Child menus retain QMenu's keyboard/hover navigation.
    QPoint position = pos();
    if (auto *button = qobject_cast<QPushButton *>(parentWidget())) {
        const auto anchor = button->mapToGlobal(QPoint(0, button->height()));
        const bool right = button->objectName() == "moreActions" || button->objectName() == "contentZoom";
        position = QPoint(right ? anchor.x() + button->width() - width() + 12 : anchor.x() - 12, anchor.y() - 8);
    }
    const auto *target = QGuiApplication::screenAt(position); if (!target) target = screen();
    const auto area = target->availableGeometry();
    position.setX(qBound(area.left(), position.x(), qMax(area.left(), area.right() - width() + 1)));
    position.setY(qBound(area.top(), position.y(), qMax(area.top(), area.bottom() - height() + 1)));
    move(position);
}
SharedSpinner::SharedSpinner(Appearance *appearance, QWidget *parent) : QWidget(parent), m_appearance(appearance) {
    setObjectName("busySpinner"); setFixedSize(20, 20); setAccessibleName(QStringLiteral("正在处理"));
    m_timer.setInterval(50); connect(&m_timer, &QTimer::timeout, this, [this] { m_angle = (m_angle + 24) % 360; update(); });
    connect(appearance, &Appearance::changed, this, [this] { update(); });
}
void SharedSpinner::setRunning(bool running) { m_running = running; if (running && isVisible()) m_timer.start(); else m_timer.stop(); update(); }
void SharedSpinner::hideEvent(QHideEvent *event) { m_timer.stop(); QWidget::hideEvent(event); }
void SharedSpinner::showEvent(QShowEvent *event) { QWidget::showEvent(event); if (m_running) m_timer.start(); }
void SharedSpinner::paintEvent(QPaintEvent *) { if (!m_running) return; QPainter p(this); p.setRenderHint(QPainter::Antialiasing); p.setPen(QPen(m_appearance->accentColor(), 2, Qt::SolidLine, Qt::RoundCap)); p.drawArc(QRectF(rect()).adjusted(3, 3, -3, -3), -m_angle * 16, 260 * 16); }
namespace {
class DialogOverlay final : public QWidget {
public:
    explicit DialogOverlay(QWidget *owner) : QWidget(owner) { setObjectName("dialogDimOverlay"); setStyleSheet("background:rgba(0,0,0,55)"); setGeometry(owner->rect()); owner->installEventFilter(this); show(); raise(); }
protected:
    bool eventFilter(QObject *object, QEvent *event) override { if (object == parentWidget() && event->type() == QEvent::Resize) setGeometry(parentWidget()->rect()); return false; }
};
}
SharedDialog::SharedDialog(Appearance *appearance, QWidget *owner, int surfaceWidth)
    : QDialog(owner, Qt::Dialog | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint), m_appearance(appearance), m_surface(new QWidget(this)) {
    setAttribute(Qt::WA_TranslucentBackground); setWindowModality(Qt::WindowModal); setFont(owner->font());
    setStyleSheet("QDialog{background:transparent}");
    auto *outer = new QVBoxLayout(this); outer->setContentsMargins(16, 16, 16, 16); outer->addWidget(m_surface);
    m_surface->setObjectName("dialogSurface"); m_surface->setFixedWidth(surfaceWidth); setFixedWidth(surfaceWidth + 32);
    connect(appearance, &Appearance::changed, this, [this] { update(); });
}
void SharedDialog::paintEvent(QPaintEvent *) { QPainter painter(this); painter.setCompositionMode(QPainter::CompositionMode_Source); painter.fillRect(rect(), Qt::transparent); painter.setCompositionMode(QPainter::CompositionMode_SourceOver); paintPopupShell(painter, m_surface->geometry(), m_appearance->color("surface"), m_appearance->color("border"), 12, 14); }
void SharedDialog::showEvent(QShowEvent *event) { if (!m_overlay && parentWidget()) m_overlay = new DialogOverlay(parentWidget()); QDialog::showEvent(event); if (parentWidget()) move(parentWidget()->frameGeometry().center() - rect().center()); }
void SharedDialog::hideEvent(QHideEvent *event) { delete m_overlay; m_overlay = nullptr; QDialog::hideEvent(event); }
ElidedLabel::ElidedLabel(QWidget *parent) : QLabel(parent) {
    setTextFormat(Qt::PlainText); setMinimumWidth(0); setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
}
void ElidedLabel::setFullText(const QString &text) {
    m_full = text; setToolTip(text); setAccessibleName(text); setText(fontMetrics().elidedText(text, Qt::ElideMiddle, qMax(0, width())));
}
void ElidedLabel::resizeEvent(QResizeEvent *event) { QLabel::resizeEvent(event); setFullText(m_full); }
}
