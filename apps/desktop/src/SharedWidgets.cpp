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
#include <QKeySequence>
#include <QPainter>
#include <QResizeEvent>
#include <QSvgRenderer>
#include <QTimer>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPaintEvent>
#include <QCache>
#include <QPainterPath>
#include <QStyleOptionMenuItem>
#include <QProxyStyle>
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
    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State) override { painter->save();if(mode==QIcon::Disabled)painter->setOpacity(painter->opacity()*.38);QSvgRenderer renderer(m_svg); renderer.render(painter, QRectF(rect));painter->restore(); }
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
    QFile file(QStringLiteral(":/xdnote/design/assets/ui-v2/icons/svg/%1.svg").arg(name == "status-dirty" ? QStringLiteral("dirty") : name));
    if (!file.open(QIODevice::ReadOnly)) return {};
    auto svg = file.readAll(); svg.replace("currentColor", color.name().toUtf8());
    return QIcon(new SvgIcon(std::move(svg)));
}
QIcon colorSwatch(const QColor &color) {
    return themedIcon(QStringLiteral("accent-swatch"), color);
}
namespace {
void paintSpinner(QPainter &painter, const QRect &area, const QColor &color, int angle) {
    painter.save(); painter.translate(area.x()+area.width()/2.0,area.y()+area.height()/2.0); painter.rotate(angle);
    themedIcon(QStringLiteral("spinner"),color).paint(&painter,QRect(-area.width()/2,-area.height()/2,area.width(),area.height())); painter.restore();
}
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
    m_busyTimer.setInterval(40);
    connect(&m_busyTimer, &QTimer::timeout, this, [this] { m_busyAngle = (m_busyAngle + 18) % 360; update(); });
}
void SharedButton::setBusy(bool value) { if (m_busy == value) return; m_busy = value; if (value && isVisible()) m_busyTimer.start(); else m_busyTimer.stop(); updateGeometry(); update(); }
void SharedButton::showEvent(QShowEvent *e) { QPushButton::showEvent(e); if (m_busy) m_busyTimer.start(); }
void SharedButton::hideEvent(QHideEvent *e) { m_busyTimer.stop(); QPushButton::hideEvent(e); }
QString themedScrollBars(Appearance *appearance) {
    return QStringLiteral("QScrollBar{background:%1;border:0;margin:0} QScrollBar:vertical{width:%3px} QScrollBar:horizontal{height:%3px} QScrollBar::handle{background:%2;border:%4px solid %1;border-radius:%5px} QScrollBar::handle:vertical{min-height:%6px} QScrollBar::handle:horizontal{min-width:%6px} QScrollBar::add-line,QScrollBar::sub-line{width:0;height:0;border:0;background:transparent} QScrollBar::add-page,QScrollBar::sub-page{background:transparent} QAbstractScrollArea::corner{background:%1;border:0}")
        .arg(appearance->color("background").name(), appearance->color("textSecondary").name())
        .arg(control("scrollbarWidth")).arg(control("scrollbarInset")).arg(control("scrollbarWidth") / 2).arg(control("scrollbarHandleMin"))
        + QStringLiteral(" QScrollBar::handle:hover{background:%1} QScrollBar::handle:pressed{background:%2}").arg(appearance->color("scrollbarHover").name(), appearance->color("scrollbarPressed").name());
}
bool SharedButton::keyboardOutline() const {
    return m_tab && hasFocus() && isEnabled() && InputModality::forApplication()->keyboardNavigation() && window()->isActiveWindow();
}
QSize SharedButton::sizeHint() const {
    const int textWidth = text().isEmpty() ? 0 : qCeil(QFontMetricsF(font(), this).horizontalAdvance(text()));
    const int iconWidth = m_symbol.isEmpty() && !m_busy ? 0 : control("iconSize") + (textWidth ? 8 : 0);
    return {qMax(control("iconButtonHit"), textWidth + iconWidth + (textWidth ? 24 : 0) + (menu() && textWidth ? 16 : 0)), control("height")};
}
QSize SharedButton::minimumSizeHint() const { return property("navigationRow").toBool() ? QSize(control("iconButtonHit"), control("height")) : sizeHint(); }
void SharedButton::focusInEvent(QFocusEvent *event) { m_tab = event->reason() == Qt::TabFocusReason || event->reason() == Qt::BacktabFocusReason; QPushButton::focusInEvent(event); update(); }
void SharedButton::focusOutEvent(QFocusEvent *event) { m_tab = false; QPushButton::focusOutEvent(event); update(); }
void SharedButton::enterEvent(QEnterEvent *event) { m_hover = true; QPushButton::enterEvent(event); update(); }
void SharedButton::leaveEvent(QEvent *event) { m_hover = false; m_nativeHover = false; QPushButton::leaveEvent(event); update(); }
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
    const bool hovered = isEnabled() && (m_hover || underMouse() || m_nativeHover);
    const bool solid = property("primary").toBool() || isChecked();
    QColor background = solid ? appearance->accentColor(isDown() ? "pressed" : hovered ? "hover" : "solid")
        : isDown() ? appearance->color("controlPressed") : hovered ? appearance->color("controlHover") : QColor(Qt::transparent);
    QColor foreground = solid ? appearance->accentColor("foreground") : appearance->color("text");
    if (!isEnabled() && !m_busy) { background = solid ? appearance->color("border") : m_outlined ? appearance->color("surface") : QColor(Qt::transparent); foreground = appearance->color("textSecondary"); }
    if (m_outlined && isEnabled() && !solid && !hovered && !isDown()) background = appearance->color("surface");
    if (m_busy) { background = appearance->accentColor(); foreground = appearance->accentColor("foreground"); }
    if (property("danger").toBool() && isEnabled()) {
        foreground = solid ? appearance->accentColor("foreground") : appearance->semantic("error");
        background = solid ? appearance->semantic("error").darker(isDown() ? 118 : hovered ? 108 : 100)
            : isDown() ? appearance->color("controlPressed") : hovered ? appearance->color("controlHover") : appearance->color("surface");
    }
    const bool caption = property("captionButton").toBool();
    const bool navigation = property("navigationRow").toBool(), selected = property("navigationSelected").toBool();
    if (navigation && selected) { background = isDown() ? appearance->color("controlPressed") : hovered ? appearance->color("controlHover") : appearance->accentColor("soft"); foreground = appearance->color("text"); }
    if (caption && isEnabled()) {
        const bool close = objectName() == "captionClose";
        background = isDown() ? (close ? QColor("#A12317") : appearance->color("controlPressed")) : hovered ? (close ? QColor("#C42B1C") : appearance->color("controlHover")) : QColor(Qt::transparent);
        foreground = close && (hovered || isDown()) ? QColor(Qt::white) : appearance->color("text");
    }
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(background); painter.setPen(m_outlined ? QPen(property("danger").toBool() && isEnabled() ? appearance->semantic("error") : appearance->color("border"), control("borderWidth")) : QPen(Qt::NoPen));
    const bool tabClose = property("tabClose").toBool();
    const int radius = tabClose ? visualTokens()["tabs"].toObject()["closeRadius"].toInt() : property("footerControl").toBool() ? control("footerControlRadius") : property("segmentPart").toBool() ? 4 : visualTokens().value("radius").toObject().value("control").toInt();
    if (caption) painter.fillRect(rect(), background); else painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    if (navigation && selected) { painter.setPen(Qt::NoPen); painter.setBrush(appearance->accentColor()); painter.drawRoundedRect(QRectF(0, 5, 3, height() - 10), 1.5, 1.5); }
    const int icon = m_symbol.isEmpty() && !m_busy ? 0 : tabClose ? visualTokens()["tabs"].toObject()["closeIcon"].toInt() : control("iconSize");
    const auto labelText = fontMetrics().elidedText(text(), Qt::ElideMiddle, qMax(0, width() - (icon ? icon + 8 : 0) - (menu() ? 16 : 0) - 24));
    const int label = qCeil(QFontMetricsF(font(), this).horizontalAdvance(labelText));
    const int dropdown = menu() && (label || property("showMenuIndicator").toBool()) ? 16 : 0;
    const bool field = property("fieldDropdown").toBool();
    const int total = label + icon + (label && icon ? 8 : 0) + dropdown; int left = navigation || field ? control("contentInset") : (width() - total) / 2;
    if (icon) { const QRect area(left,(height()-icon)/2,icon,icon); if (m_busy) paintSpinner(painter,area,foreground,m_busyAngle); else themedIcon(m_symbol,foreground).paint(&painter,area); left += icon + (label ? 8 : 0); }
    painter.setPen(foreground); painter.setFont(font()); painter.drawText(QRect(left, 0, label, height()), Qt::AlignCenter, labelText);
    if (dropdown) themedIcon(menu() && menu()->isVisible() ? "chevron-up" : "chevron-down", foreground).paint(&painter, QRect(field ? width() - control("contentInset") - 12 : left + label + 4, (height() - 12) / 2, 12, 12));
    if (keyboardOutline()) {
        painter.setBrush(Qt::NoBrush); painter.setPen(QPen(appearance->accentColor(), control("focusRingWidth")));
        painter.drawRoundedRect(QRectF(rect()).adjusted(2, 2, -2, -2), radius - 1, radius - 1);
    }
}
SharedLineEdit::SharedLineEdit(Appearance *appearance, QWidget *parent) : QLineEdit(parent), m_appearance(appearance) {
    setFont(parent ? parent->font() : uiFont()); setFixedHeight(control("height")); setAttribute(Qt::WA_MacShowFocusRect, false);
    auto refresh = [this, appearance] { auto p = palette(); p.setColor(QPalette::PlaceholderText, appearance->color("textSecondary")); setPalette(p);
        setStyleSheet(QStringLiteral("QLineEdit{background:%1;color:%2;border:1px solid %3;border-radius:6px;padding:4px %4px;selection-background-color:%5} QLineEdit:disabled{color:%6;background:%7} QLineEdit[readOnly=true]{background:%7}")
            .arg(appearance->color("surface").name(), appearance->color("text").name(), appearance->color("border").name()).arg(control("contentInset") - 1).arg(appearance->accentColor("soft").name(), appearance->color("textSecondary").name(), appearance->color("sidebar").name())); update(); };
    connect(appearance, &Appearance::changed, this, refresh); connect(InputModality::forApplication(), &InputModality::changed, this, qOverload<>(&QWidget::update)); refresh();
}
void SharedLineEdit::focusInEvent(QFocusEvent *e) { m_tab = e->reason() == Qt::TabFocusReason || e->reason() == Qt::BacktabFocusReason; QLineEdit::focusInEvent(e); update(); }
void SharedLineEdit::focusOutEvent(QFocusEvent *e) { m_tab = false; QLineEdit::focusOutEvent(e); update(); }
void SharedLineEdit::keyPressEvent(QKeyEvent *e) { if (e->key() == Qt::Key_Escape || e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) { m_tab = false; InputModality::forApplication()->clear(); } QLineEdit::keyPressEvent(e); update(); }
void SharedLineEdit::paintEvent(QPaintEvent *e) {
    QLineEdit::paintEvent(e);
    if (m_shortcut && !hasFocus()) {
        // Keep the actual QLineEdit, its value and accessibility intact. Only
        // the idle presentation changes; focus restores native text editing.
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen); p.setBrush(m_appearance->color(isEnabled() && !isReadOnly() ? "surface" : "sidebar"));
        p.drawRoundedRect(QRectF(rect()).adjusted(1,1,-1,-1),5,5);
        const QRect area = rect().adjusted(control("contentInset"),3,-control("contentInset"),-3);
        p.setClipRect(area); p.setPen(m_appearance->color(isEnabled()?"text":"textSecondary"));
        if (text().trimmed().isEmpty()) { p.setPen(m_appearance->color("textSecondary")); p.drawText(area,Qt::AlignVCenter,placeholderText()); }
        else {
            const auto sequence=QKeySequence::fromString(text(),QKeySequence::PortableText);
            bool valid=!sequence.isEmpty();for(int i=0;i<sequence.count();++i)valid=valid&&sequence[i].key()!=Qt::Key_unknown&&sequence[i].key()!=Qt::Key(0);
            const auto tokens=visualTokens()["settings"].toObject();
            const int capHeight=tokens["keyCapHeight"].toInt(),gap=tokens["keyCapGap"].toInt();
            int x=area.left();
            for (int chord=0;valid&&chord<sequence.count();++chord) {
                if(chord){p.drawText(QRect(x,area.y(),12,area.height()),Qt::AlignCenter,QStringLiteral(","));x+=16;}
                const auto key=sequence[chord];QStringList labels;
                // Let Qt supply platform-native modifier names/symbols.
                const auto native=QKeySequence(key).toString(QKeySequence::NativeText);
                const auto keyName=QKeySequence(key.key()).toString(QKeySequence::NativeText);
                auto modifiers=native; if(modifiers.endsWith(keyName))modifiers.chop(keyName.size());
                if(modifiers.endsWith('+'))modifiers.chop(1);
                if(modifiers.contains('+'))labels=modifiers.split('+',Qt::SkipEmptyParts);
                else if(!modifiers.isEmpty()) {
#ifdef Q_OS_MACOS
                    for(const auto symbol:modifiers)labels.append(QString(symbol));
#else
                    labels.append(modifiers);
#endif
                }
                labels.append(keyName);
                for(const auto &label:labels) {
                    const int width=qMax(tokens["keyCapMinWidth"].toInt(),fontMetrics().horizontalAdvance(label)+2*tokens["keyCapPadding"].toInt());
                    const QRectF cap(x,(height()-capHeight)/2.0,width,capHeight);
                    p.setPen(QPen(m_appearance->color("border"),1));p.setBrush(m_appearance->color("sidebar"));
                    p.drawRoundedRect(cap.adjusted(.5,.5,-.5,-.5),4,4);p.setPen(m_appearance->color(isEnabled()?"text":"textSecondary"));p.drawText(cap,Qt::AlignCenter,label);x+=width+gap;
                }
            }
            if(!valid)p.drawText(area,Qt::AlignVCenter,fontMetrics().elidedText(text(),Qt::ElideRight,area.width()));
            else if(x-gap>area.right()+1){p.fillRect(QRect(area.right()-14,area.top(),15,area.height()),m_appearance->color("surface"));p.drawText(QRect(area.right()-14,area.top(),15,area.height()),Qt::AlignCenter,QStringLiteral("…"));}
        }
    }
    if (m_tab && hasFocus() && isEnabled() && InputModality::forApplication()->keyboardNavigation()) { QPainter p(this); p.setRenderHint(QPainter::Antialiasing); p.setPen(QPen(m_appearance->accentColor(), control("focusRingWidth"))); p.setBrush(Qt::NoBrush); p.drawRoundedRect(QRectF(rect()).adjusted(2,2,-2,-2),4,4); }
}
SharedCheckBox::SharedCheckBox(const QString &text, Appearance *appearance, QWidget *parent) : QCheckBox(text,parent),m_appearance(appearance) {
    setFont(parent ? parent->font() : uiFont()); setFixedHeight(control("height")); setFocusPolicy(Qt::StrongFocus); setAttribute(Qt::WA_MacShowFocusRect,false); setAttribute(Qt::WA_Hover);
    connect(appearance,&Appearance::changed,this,qOverload<>(&QWidget::update)); connect(InputModality::forApplication(),&InputModality::changed,this,qOverload<>(&QWidget::update));
    connect(this,&QCheckBox::pressed,this,[this]{m_tab=false;InputModality::forApplication()->clear();update();});
}
QSize SharedCheckBox::sizeHint() const { return {32 + fontMetrics().horizontalAdvance(text()),control("height")}; }
void SharedCheckBox::focusInEvent(QFocusEvent *e) {m_tab=e->reason()==Qt::TabFocusReason||e->reason()==Qt::BacktabFocusReason;QCheckBox::focusInEvent(e);update();}
void SharedCheckBox::focusOutEvent(QFocusEvent *e) {m_tab=false;QCheckBox::focusOutEvent(e);update();}
void SharedCheckBox::keyPressEvent(QKeyEvent *e) {if(e->key()==Qt::Key_Return||e->key()==Qt::Key_Enter){m_tab=false;InputModality::forApplication()->clear();click();e->accept();return;}if(e->key()==Qt::Key_Space||e->key()==Qt::Key_Escape){m_tab=false;InputModality::forApplication()->clear();}QCheckBox::keyPressEvent(e);update();}
void SharedCheckBox::paintEvent(QPaintEvent *) {
    auto*a=m_appearance.data();QPainter p(this);p.setRenderHint(QPainter::Antialiasing);const QRectF box(3,(height()-16)/2.0,16,16);const bool on=checkState()!=Qt::Unchecked;
    QColor fill=on?a->accentColor(isDown()?"pressed":underMouse()?"hover":"solid"):a->color(underMouse()?"controlHover":"surface");if(!isEnabled())fill=a->color("border");
    p.setBrush(fill);p.setPen(QPen(on?fill:a->color("textSecondary"),1));p.drawRoundedRect(box,3,3);
    if(on){p.setPen(QPen(isEnabled()?a->accentColor("foreground"):a->color("textSecondary"),1.75,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));if(checkState()==Qt::PartiallyChecked)p.drawLine(box.center()+QPointF(-4,0),box.center()+QPointF(4,0));else{QPainterPath tick;tick.moveTo(box.topLeft()+QPointF(4,8));tick.lineTo(box.topLeft()+QPointF(7,11));tick.lineTo(box.topLeft()+QPointF(12,5));p.drawPath(tick);}}
    p.setFont(font());p.setPen(a->color(isEnabled()?"text":"textSecondary"));p.drawText(QRect(27,0,width()-27,height()),Qt::AlignVCenter|Qt::AlignLeft,fontMetrics().elidedText(text(),Qt::ElideRight,width()-27));
    if(m_tab&&hasFocus()&&InputModality::forApplication()->keyboardNavigation()){p.setBrush(Qt::NoBrush);p.setPen(QPen(a->accentColor(),2));p.drawRoundedRect(QRectF(1,2,qMin(width(),sizeHint().width())-2,height()-4),4,4);}
}
SharedSegment::SharedSegment(Appearance*a,QWidget*p):QWidget(p),m_appearance(a){setFixedHeight(control("height"));auto*l=new QHBoxLayout(this);l->setContentsMargins(2,2,2,2);l->setSpacing(2);connect(a,&Appearance::changed,this,qOverload<>(&QWidget::update));}
void SharedSegment::addButton(SharedButton*b){b->setProperty("segmentPart",true);b->setFixedHeight(control("height")-4);b->setFixedWidth(control("modeSegmentWidth"));static_cast<QHBoxLayout*>(layout())->addWidget(b);}
void SharedSegment::paintEvent(QPaintEvent*){QPainter p(this);p.setRenderHint(QPainter::Antialiasing);p.setBrush(m_appearance->color("sidebar"));p.setPen(QPen(m_appearance->color("border"),1));p.drawRoundedRect(QRectF(rect()).adjusted(.5,.5,-.5,-.5),6,6);}
SharedPanel::SharedPanel(Appearance*a,QWidget*p):QWidget(p),m_appearance(a){connect(a,&Appearance::changed,this,qOverload<>(&QWidget::update));}
void SharedPanel::paintEvent(QPaintEvent*){QPainter p(this);p.setRenderHint(QPainter::Antialiasing);p.setBrush(m_appearance->color("surface"));p.setPen(QPen(m_appearance->color("border"),1));p.drawRoundedRect(QRectF(rect()).adjusted(.5,.5,-.5,-.5),8,8);}
namespace {
class ScrollableMenuStyle final : public QProxyStyle {
public:
    int styleHint(StyleHint hint, const QStyleOption *option, const QWidget *widget, QStyleHintReturn *result) const override {
        if (hint == SH_Menu_Scrollable) return 1;
        return QProxyStyle::styleHint(hint, option, widget, result);
    }
};
}
SharedMenu::SharedMenu(Appearance *appearance, QWidget *parent) : QMenu(parent), m_appearance(appearance) {
    auto *placement = new ScrollableMenuStyle; placement->setParent(this); setStyle(placement);
    setWindowFlags(Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground); setAttribute(Qt::WA_NoSystemBackground);
    setFont(uiFont(control("menuFontSize")));
    connect(this, &QMenu::aboutToHide, this, [this] {
        InputModality::forApplication()->clear();
        QPointer<QWidget> caller(parentWidget());
        QTimer::singleShot(0, this, [caller] { if (caller && !qobject_cast<QMenu *>(caller.data())) { caller->setFocus(Qt::PopupFocusReason); caller->update(); } });
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
    const auto items = actions();
    const bool hasIcons = std::any_of(items.cbegin(), items.cend(), [](QAction *a) { return !a->icon().isNull(); });
    QList<QAction *> visible; for (auto *a : items) if (a->isVisible()) visible.append(a);
    const int scroller = qMax(12, style()->pixelMetric(QStyle::PM_MenuScrollerHeight, nullptr, this));
    auto content = surfaceRect().adjusted(4,4,-4,-4);
    const bool above = !visible.isEmpty() && actionGeometry(visible.first()).top() < content.top();
    const bool below = !visible.isEmpty() && actionGeometry(visible.last()).bottom() > content.bottom();
    const QRect up(content.left(), content.top(), content.width(), scroller), down(content.left(), content.bottom()-scroller+1, content.width(), scroller);
    if (above) content.setTop(content.top()+scroller);
    if (below) content.setBottom(content.bottom()-scroller);
    painter.save(); painter.setClipRect(content, Qt::IntersectClip);
    for (auto *action : actions()) if (action->isVisible()) {
        const auto r=actionGeometry(action); if(!r.intersects(surfaceRect()))continue;
        if(action->isSeparator()){painter.setPen(QPen(m_appearance->color("border"),1));painter.drawLine(r.left()+8,r.center().y(),r.right()-8,r.center().y());continue;}
        const bool active=action==activeAction()&&action->isEnabled();
        if(active){painter.setPen(Qt::NoPen);painter.setBrush(m_appearance->accentColor("soft"));painter.drawRoundedRect(r,4,4);}
        const auto fg=action->isEnabled()?m_appearance->color("text"):m_appearance->color("textSecondary");painter.setPen(fg);painter.setFont(font());
        if(!action->icon().isNull())action->icon().paint(&painter,QRect(r.left()+8,r.top()+(r.height()-16)/2,16,16),Qt::AlignCenter,action->isEnabled()?QIcon::Normal:QIcon::Disabled);
        int right=r.right()-8;
        if(action->menu()){themedIcon("chevron-right",fg).paint(&painter,QRect(right-12,r.top()+(r.height()-12)/2,12,12));right-=24;}
        else if(action->isCheckable()){if(action->isChecked())themedIcon("check",fg).paint(&painter,QRect(right-16,r.top()+(r.height()-16)/2,16,16));right-=24;}
        const auto shortcut=action->shortcut().isEmpty()?action->property("displayShortcut").toString():action->shortcut().toString(QKeySequence::NativeText);
        if(!shortcut.isEmpty()){const int w=fontMetrics().horizontalAdvance(shortcut);painter.setPen(m_appearance->color("textSecondary"));painter.drawText(QRect(right-w,r.top(),w,r.height()),Qt::AlignRight|Qt::AlignVCenter,shortcut);right-=w+24;}
        const int left=r.left()+(hasIcons?32:8);auto text=action->text();text.replace("&&",QString(QChar(0xF000)));text.remove('&');text.replace(QChar(0xF000),'&');
        painter.setPen(fg);painter.drawText(QRect(left,r.top(),qMax(0,right-left),r.height()),Qt::AlignLeft|Qt::AlignVCenter,fontMetrics().elidedText(text,Qt::ElideRight,qMax(0,right-left)));
    }
    painter.restore();
    for (const auto &entry : {qMakePair(above,up),qMakePair(below,down)}) if (entry.first) {
        painter.fillRect(entry.second,m_appearance->color("surface"));
        const auto center=entry.second.center(); themedIcon(entry.second==up?"chevron-up":"chevron-down",m_appearance->color("textSecondary")).paint(&painter,QRect(center.x()-6,center.y()-6,12,12));
    }
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
    if (parentWidget()) parentWidget()->update();
}
SharedSpinner::SharedSpinner(Appearance *appearance, QWidget *parent) : QWidget(parent), m_appearance(appearance) {
    setObjectName("busySpinner"); setFixedSize(20, 20); setAccessibleName(QStringLiteral("正在处理"));
    m_timer.setInterval(50); connect(&m_timer, &QTimer::timeout, this, [this] { m_angle = (m_angle + 24) % 360; update(); });
    connect(appearance, &Appearance::changed, this, [this] { update(); });
}
void SharedSpinner::setRunning(bool running) { m_running = running; if (running && isVisible()) m_timer.start(); else m_timer.stop(); update(); }
void SharedSpinner::hideEvent(QHideEvent *event) { m_timer.stop(); QWidget::hideEvent(event); }
void SharedSpinner::showEvent(QShowEvent *event) { QWidget::showEvent(event); if (m_running) m_timer.start(); }
void SharedSpinner::paintEvent(QPaintEvent *) { if (!m_running) return; QPainter p(this); paintSpinner(p,rect(),m_appearance->accentColor(),m_angle); }
namespace {
class DialogOverlay final : public QWidget {
public:
    explicit DialogOverlay(QWidget *owner) : QWidget(owner) { setObjectName("dialogDimOverlay"); setStyleSheet("background:rgba(0,0,0,55)"); setGeometry(owner->rect()); owner->installEventFilter(this); show(); raise(); }
protected:
    bool eventFilter(QObject *object, QEvent *event) override { if (object == parentWidget() && event->type() == QEvent::Resize) setGeometry(parentWidget()->rect()); return false; }
};
}
SharedDialog::SharedDialog(Appearance *appearance, QWidget *owner, int surfaceWidth)
    : QDialog(owner, Qt::Dialog | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint), m_appearance(appearance), m_surface(new QWidget(this)), m_scroll(new QScrollArea(this)), m_preferredSurfaceWidth(surfaceWidth) {
    setAttribute(Qt::WA_TranslucentBackground); setWindowModality(Qt::WindowModal); setFont(owner->font());
    auto refresh=[this,appearance]{auto p=palette();p.setColor(QPalette::Window,appearance->color("surface"));p.setColor(QPalette::WindowText,appearance->color("text"));p.setColor(QPalette::Base,appearance->color("surface"));p.setColor(QPalette::Text,appearance->color("text"));setPalette(p);
        setStyleSheet(QStringLiteral("QDialog{background:transparent} QWidget#dialogSurface{background:transparent} QLabel{color:%1;background:transparent} QLabel[secondary=true]{color:%2} QScrollArea{background:transparent;border:0} QScrollArea>QWidget>QWidget{background:%3;color:%1}").arg(appearance->color("text").name(),appearance->color("textSecondary").name(),appearance->color("surface").name())+themedScrollBars(appearance));update();};
    auto *outer = new QVBoxLayout(this); outer->setContentsMargins(24, 24, 24, 24);outer->setSizeConstraint(QLayout::SetNoConstraint);
    m_scroll->setObjectName("dialogContentScroll");m_scroll->setFrameShape(QFrame::NoFrame);m_scroll->setWidget(m_surface);m_scroll->setWidgetResizable(false);m_scroll->setMinimumSize(0,0);outer->addWidget(m_scroll);
    m_surface->setObjectName("dialogSurface"); m_surface->setFixedWidth(surfaceWidth); setFixedWidth(surfaceWidth + 48);
    connect(appearance, &Appearance::changed, this, refresh);refresh();
}
void SharedDialog::paintEvent(QPaintEvent *) { QPainter painter(this); painter.setCompositionMode(QPainter::CompositionMode_Source); painter.fillRect(rect(), Qt::transparent); painter.setCompositionMode(QPainter::CompositionMode_SourceOver); paintPopupShell(painter, m_scroll->geometry(), m_appearance->color("surface"), m_appearance->color("border"), 12, 18); }
void SharedDialog::showEvent(QShowEvent *event) {
    const auto available=(parentWidget()?parentWidget()->screen():screen())->availableGeometry();
    const int windowWidth=qMin(m_preferredSurfaceWidth+48,available.width());int bodyWidth=qMax(1,windowWidth-48);
    auto bodyHeight=[&](int width){m_surface->setFixedWidth(width);m_surface->layout()->activate();const auto height=m_surface->layout()->totalHeightForWidth(width);return qMax(m_surface->minimumSizeHint().height(),height<0?m_surface->sizeHint().height():height);};
    int contentHeight=bodyHeight(bodyWidth);
    if(contentHeight>available.height()-48){bodyWidth=qMax(1,bodyWidth-m_scroll->verticalScrollBar()->sizeHint().width());contentHeight=bodyHeight(bodyWidth);}
    m_surface->resize(bodyWidth,contentHeight);setFixedSize(windowWidth,qMin(contentHeight+48,available.height()));
    if (!m_overlay && parentWidget()) m_overlay = new DialogOverlay(parentWidget()); QDialog::showEvent(event);
    setGraphicsEffect(nullptr);
#ifdef Q_OS_WIN
    const auto policy = DWMNCRP_DISABLED; DwmSetWindowAttribute(reinterpret_cast<HWND>(winId()), DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
#endif
    if (parentWidget()) {auto point=parentWidget()->frameGeometry().center()-rect().center();point.setX(qBound(available.left(),point.x(),available.right()-width()+1));point.setY(qBound(available.top(),point.y(),available.bottom()-height()+1));move(point);}
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
