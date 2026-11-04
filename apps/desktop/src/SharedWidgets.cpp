#include "SharedWidgets.h"
#include "Appearance.h"
#include "InputModality.h"
#include "VisualTokens.h"
#include "UiFonts.h"
#include <QButtonGroup>
#include <QActionGroup>
#include <QSplitterHandle>
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
#include <QCache>
#include <QPainterPath>
#include <QStyleOptionMenuItem>
#include <QtMath>
#include <cmath>
#include <algorithm>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#endif

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
SharedMenu *createAppearanceMenu(Appearance *appearance, QWidget *parent) {
    auto *menu = new SharedMenu(appearance, parent); menu->setTitle(QStringLiteral("外观")); menu->setObjectName("appearanceMenu");
    auto *themes = new QActionGroup(menu);
    const QList<QPair<QString, QString>> names{{"system", QStringLiteral("跟随系统")}, {"light", QStringLiteral("浅色")}, {"dark", QStringLiteral("深色")}};
    for (const auto &entry : names) {
        auto *action = menu->addAction(entry.second); action->setObjectName("theme-" + entry.first); action->setData(entry.first); action->setCheckable(true); themes->addAction(action);
        QObject::connect(action, &QAction::triggered, appearance, [appearance, theme = entry.first] { appearance->setPreferences(theme, appearance->accent()); });
    }
    auto *accents = new SharedMenu(appearance, menu); accents->setTitle(QStringLiteral("强调色")); accents->setObjectName("accentMenu"); menu->addMenu(accents); auto *group = new QActionGroup(menu);
    for (const auto &name : {"graphite", "mist-blue", "cobalt", "mauve", "terracotta", "olive"}) {
        const auto entry = visualTokens().value("accents").toObject().value(name).toObject(); auto *action = accents->addAction(entry.value("label").toString() + "  " + entry.value("base").toString());
        action->setObjectName("accent-" + QString::fromLatin1(name)); action->setData(QString::fromLatin1(name)); action->setCheckable(true); action->setIcon(colorSwatch(QColor(entry.value("base").toString()))); group->addAction(action);
        QObject::connect(action, &QAction::triggered, appearance, [appearance, name = QString::fromLatin1(name)] { appearance->setPreferences(appearance->theme(), name); });
    }
    const auto update = [menu, themes, accents, group, appearance] {
        for (auto *action : themes->actions()) { action->setChecked(action->data().toString() == appearance->theme()); action->setIcon(themedIcon(action->data() == "system" ? "monitor" : action->data() == "light" ? "sun" : "moon", appearance->color("text"))); }
        for (auto *action : group->actions()) action->setChecked(action->data().toString() == appearance->accent());
        menu->menuAction()->setIcon(themedIcon("palette", appearance->color("text"))); accents->menuAction()->setIcon(themedIcon("palette", appearance->color("text")));
    };
    QObject::connect(appearance, &Appearance::changed, menu, update); QObject::connect(appearance, &Appearance::preferenceFailed, menu, update); QObject::connect(menu, &QMenu::aboutToShow, menu, update); update(); return menu;
}
DocumentSplitter::DocumentSplitter(Appearance *appearance, QWidget *parent) : QSplitter(Qt::Horizontal, parent), m_appearance(appearance) {
    setChildrenCollapsible(false); setHandleWidth(control("splitterHitWidth"));
}
SharedSeparator::SharedSeparator(Qt::Orientation orientation, Appearance *appearance, QWidget *parent) : QWidget(parent), m_appearance(appearance) {
    if (orientation == Qt::Vertical) setFixedWidth(control("borderWidth")); else setFixedHeight(control("borderWidth"));
    setSizePolicy(orientation == Qt::Vertical ? QSizePolicy::Fixed : QSizePolicy::Expanding, orientation == Qt::Vertical ? QSizePolicy::Expanding : QSizePolicy::Fixed);
    connect(appearance, &Appearance::changed, this, qOverload<>(&QWidget::update));
}
void SharedSeparator::paintEvent(QPaintEvent *) { QPainter painter(this); painter.fillRect(rect(), m_appearance->color("border")); }
QSplitterHandle *DocumentSplitter::createHandle() {
    class Handle final : public QSplitterHandle {
    public:
        Handle(Appearance *appearance, QSplitter *parent) : QSplitterHandle(Qt::Horizontal, parent), appearance(appearance) {
            setObjectName("documentDivider"); setAccessibleName(QStringLiteral("调整编辑与预览宽度"));
            connect(appearance, &Appearance::changed, this, qOverload<>(&QWidget::update));
        }
        void paintEvent(QPaintEvent *) override {
            QPainter painter(this); painter.fillRect(rect(), appearance->color("background"));
            painter.fillRect(QRect((width() - control("borderWidth")) / 2, 0, control("borderWidth"), height()), appearance->color("border"));
            if (splitter()->count() == 2 && splitter()->widget(0)->isVisible() && splitter()->widget(1)->isVisible()) {
                const int toolbarHeight = visualTokens().value("windows").toObject().value("main").toObject().value("sourceToolbarHeight").toInt();
                painter.fillRect(QRect(0, toolbarHeight - control("borderWidth"), width(), control("borderWidth")), appearance->color("border"));
            }
        }
        Appearance *appearance;
    };
    return new Handle(m_appearance, this);
}
void paintPopupShell(QPainter &painter, const QRectF &surface, const QColor &fill, const QColor &border, int radius, int shadow) {
    painter.setRenderHint(QPainter::Antialiasing); painter.setPen(Qt::NoPen);
    const qreal dpr = painter.device()->devicePixelRatioF(); const int offset = qMax(2, shadow / 4), pad = shadow + offset + 2;
    const QRectF extent = surface.adjusted(-pad, -pad, pad, pad);
    const QSize size(qCeil(extent.width() * dpr), qCeil(extent.height() * dpr));
    const QString key = QStringLiteral("%1:%2:%3:%4:%5").arg(size.width()).arg(size.height()).arg(dpr).arg(radius).arg(shadow);
    static QCache<QString, QImage> cache(32 * 1024);
    auto *blurred = cache.object(key);
    QImage generated;
    if (!blurred) {
        QImage mask(size, QImage::Format_ARGB32_Premultiplied); mask.setDevicePixelRatio(dpr); mask.fill(Qt::transparent);
        { QPainter p(&mask); p.setRenderHint(QPainter::Antialiasing); p.setPen(Qt::NoPen); p.setBrush(Qt::black); p.drawRoundedRect(QRectF(pad, pad + offset, surface.width(), surface.height()), radius, radius); }
        const int reach = qMax(1, qCeil(shadow * dpr)); const double sigma = reach / 3.0;
        QVector<double> weights(2 * reach + 1); double total = 0;
        for (int k = -reach; k <= reach; ++k) { const double value = std::exp(-k * k / (2.0 * sigma * sigma)); weights[k + reach] = value; total += value; }
        for (auto &weight : weights) weight /= total;
        const int width = size.width(), height = size.height(); QVector<float> horizontal(width * height);
        for (int y = 0; y < height; ++y) { const auto *row = reinterpret_cast<const QRgb *>(mask.constScanLine(y)); for (int x = 0; x < width; ++x) {
            double alpha = 0; for (int k = qMax(-reach, -x); k <= qMin(reach, width - x - 1); ++k) alpha += qAlpha(row[x + k]) * weights[k + reach]; horizontal[y * width + x] = float(alpha);
        } }
        generated = QImage(size, QImage::Format_ARGB32_Premultiplied); generated.setDevicePixelRatio(dpr);
        for (int y = 0; y < height; ++y) { auto *row = reinterpret_cast<QRgb *>(generated.scanLine(y)); for (int x = 0; x < width; ++x) {
            double alpha = 0; for (int k = qMax(-reach, -y); k <= qMin(reach, height - y - 1); ++k) alpha += horizontal[(y + k) * width + x] * weights[k + reach]; row[x] = qRgba(0, 0, 0, qRound(alpha * .20));
        } }
        const int cost = qMax(1, int(generated.sizeInBytes() / 1024));
        if (cost <= cache.maxCost()) { cache.insert(key, new QImage(generated), cost); blurred = cache.object(key); } else blurred = &generated;
    }
    painter.drawImage(extent.topLeft(), *blurred);
    painter.setBrush(fill); painter.setPen(QPen(border, 1));
    painter.drawRoundedRect(surface.adjusted(.5, .5, -.5, -.5), radius, radius);
}
SharedButton::SharedButton(const QString &text, const QString &name, Appearance *appearance, QWidget *parent)
    : QPushButton(text, parent), m_appearance(appearance) {
    setObjectName(name); setFocusPolicy(Qt::StrongFocus); setAutoDefault(false);
    setAttribute(Qt::WA_MacShowFocusRect, false);
    setAttribute(Qt::WA_Hover, true);
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
    const int textWidth = text().isEmpty() ? 0 : qCeil(QFontMetricsF(font(), this).horizontalAdvance(text()));
    const int iconWidth = m_symbol.isEmpty() ? 0 : control("iconSize") + (textWidth ? 8 : 0);
    return {qMax(control("iconButtonHit"), textWidth + iconWidth + (textWidth ? 24 : 0) + (menu() && textWidth ? 16 : 0)), control("height")};
}
QSize SharedButton::minimumSizeHint() const { return property("navigationRow").toBool() ? QSize(control("iconButtonHit"), control("height")) : sizeHint(); }
void SharedButton::focusInEvent(QFocusEvent *event) { m_tab = event->reason() == Qt::TabFocusReason || event->reason() == Qt::BacktabFocusReason; QPushButton::focusInEvent(event); update(); }
void SharedButton::focusOutEvent(QFocusEvent *event) { m_tab = false; QPushButton::focusOutEvent(event); update(); }
void SharedButton::enterEvent(QEnterEvent *event) { QPushButton::enterEvent(event); update(); }
void SharedButton::leaveEvent(QEvent *event) { m_nativeHover = false; QPushButton::leaveEvent(event); update(); }
void SharedButton::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_Space || event->key() == Qt::Key_Escape) {
        m_tab = false; InputModality::forApplication()->clear(); update();
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) { if (menu()) showMenu(); else click(); event->accept(); return; }
    if (isCheckable() && group() && (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right)) {
        auto buttons = group()->buttons();
        buttons.erase(std::remove_if(buttons.begin(), buttons.end(), [](QAbstractButton *button) { return !button->isVisible() || !button->isEnabled(); }), buttons.end());
        if (buttons.isEmpty()) { event->accept(); return; }
        const int direction = event->key() == Qt::Key_Left ? -1 : 1;
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
    const bool caption = property("captionButton").toBool(), hovered = underMouse() || m_nativeHover;
    const bool navigation = property("navigationRow").toBool(), selected = property("navigationSelected").toBool();
    if (navigation && selected) { background = appearance->accentColor("soft"); foreground = appearance->color("text"); }
    if (caption && isEnabled()) {
        const bool close = objectName() == "captionClose";
        background = isDown() ? (close ? QColor("#A12317") : appearance->color("border")) : hovered ? (close ? QColor("#C42B1C") : appearance->color("border")) : QColor(Qt::transparent);
        foreground = close && (hovered || isDown()) ? QColor(Qt::white) : appearance->color("text");
    }
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(background); painter.setPen(m_outlined ? QPen(appearance->color("border"), control("borderWidth")) : QPen(Qt::NoPen));
    const int radius = visualTokens().value("radius").toObject().value("control").toInt();
    if (caption) painter.fillRect(rect(), background); else painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    if (navigation && selected) { painter.setPen(Qt::NoPen); painter.setBrush(appearance->accentColor()); painter.drawRoundedRect(QRectF(0, 5, 3, height() - 10), 1.5, 1.5); }
    const int icon = m_symbol.isEmpty() ? 0 : control("iconSize");
    const auto labelText = fontMetrics().elidedText(text(), Qt::ElideMiddle, qMax(0, width() - (icon ? icon + 8 : 0) - (menu() ? 16 : 0) - 24));
    const int label = qCeil(QFontMetricsF(font(), this).horizontalAdvance(labelText));
    const int dropdown = menu() && (label || property("showMenuIndicator").toBool()) ? 16 : 0;
    const int total = label + icon + (label && icon ? 8 : 0) + dropdown; int left = navigation ? 12 : (width() - total) / 2;
    if (icon) { themedIcon(m_symbol, foreground).paint(&painter, QRect(left, (height() - icon) / 2, icon, icon)); left += icon + (label ? 8 : 0); }
    painter.setPen(foreground); painter.setFont(font()); painter.drawText(QRect(left, 0, label, height()), Qt::AlignCenter, labelText);
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
        setStyleSheet(QStringLiteral("QMenu{background:transparent;color:%1;border:0;margin:18px;padding:4px} QMenu::item{min-height:%6px;padding:4px 24px 4px 8px;border-radius:4px;background:transparent} QMenu::item:selected{background:%3;color:%4} QMenu::item:disabled{color:%5} QMenu::separator{height:1px;background:%2;margin:4px 8px} QMenu::indicator{width:16px;height:16px} QMenu::icon{padding:0 6px}")
            .arg(appearance->color("text").name(), appearance->color("border").name(), appearance->accentColor("soft").name(), appearance->accentColor("softForeground").name(), appearance->color("textSecondary").name()).arg(control("menuRowHeight") - 8));
        setGraphicsEffect(nullptr);
    };
    connect(appearance, &Appearance::changed, this, refresh); refresh();
}
QRect SharedMenu::surfaceRect() const { return rect().adjusted(18, 18, -18, -18); }
void SharedMenu::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event); QPainter painter(this); painter.setCompositionMode(QPainter::CompositionMode_Source); painter.fillRect(rect(), Qt::transparent); painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    paintPopupShell(painter, surfaceRect(), m_appearance->color("surface"), m_appearance->color("border"), 8, 12);
    QPainterPath clip; clip.addRoundedRect(surfaceRect(), 8, 8); painter.setClipPath(clip);
    for (auto *action : actions()) if (action->isVisible()) { QStyleOptionMenuItem option; initStyleOption(&option, action); option.rect = actionGeometry(action); style()->drawControl(QStyle::CE_MenuItem, &option, &painter, this); }
}
void SharedMenu::showEvent(QShowEvent *event) {
    QMenu::showEvent(event);
    // The Windows 11 Qt style attaches a QGraphicsDropShadowEffect at polish.
    // Our translucent shell already owns its shadow, so remove that extra layer.
    setGraphicsEffect(nullptr);
#ifdef Q_OS_WIN
    const auto policy = DWMNCRP_DISABLED; DwmSetWindowAttribute(reinterpret_cast<HWND>(winId()), DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
#endif
    // Keep the actual surface (rather than its shadow margin) aligned to the
    // invoking control. Child menus retain QMenu's keyboard/hover navigation.
    QPoint position = pos();
    if (auto *button = qobject_cast<QPushButton *>(parentWidget())) {
        const auto anchor = button->mapToGlobal(QPoint(0, button->height()));
        const bool right = button->objectName() == "moreActions" || button->objectName() == "contentZoom";
        position = QPoint(right ? anchor.x() + button->width() - width() + 18 : anchor.x() - 18, anchor.y() - 14);
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
    auto *outer = new QVBoxLayout(this); outer->setContentsMargins(24, 24, 24, 24); outer->addWidget(m_surface);
    m_surface->setObjectName("dialogSurface"); m_surface->setFixedWidth(surfaceWidth); setFixedWidth(surfaceWidth + 48);
    connect(appearance, &Appearance::changed, this, [this] { update(); });
}
void SharedDialog::paintEvent(QPaintEvent *) { QPainter painter(this); painter.setCompositionMode(QPainter::CompositionMode_Source); painter.fillRect(rect(), Qt::transparent); painter.setCompositionMode(QPainter::CompositionMode_SourceOver); paintPopupShell(painter, m_surface->geometry(), m_appearance->color("surface"), m_appearance->color("border"), 12, 18); }
void SharedDialog::showEvent(QShowEvent *event) {
    if (!m_overlay && parentWidget()) m_overlay = new DialogOverlay(parentWidget()); QDialog::showEvent(event);
    setGraphicsEffect(nullptr);
#ifdef Q_OS_WIN
    const auto policy = DWMNCRP_DISABLED; DwmSetWindowAttribute(reinterpret_cast<HWND>(winId()), DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
#endif
    if (parentWidget()) move(parentWidget()->frameGeometry().center() - rect().center());
}
void SharedDialog::hideEvent(QHideEvent *event) { delete m_overlay; m_overlay = nullptr; QDialog::hideEvent(event); }
ElidedLabel::ElidedLabel(QWidget *parent) : QLabel(parent) {
    setTextFormat(Qt::PlainText); setMinimumWidth(0); setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
}
void ElidedLabel::setFullText(const QString &text) {
    m_full = text; setToolTip(text); setAccessibleName(text); setText(fontMetrics().elidedText(text, Qt::ElideMiddle, qMax(0, width())));
}
void ElidedLabel::resizeEvent(QResizeEvent *event) { QLabel::resizeEvent(event); setFullText(m_full); }
}
