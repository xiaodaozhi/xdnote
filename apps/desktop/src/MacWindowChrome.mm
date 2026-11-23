#import <AppKit/AppKit.h>

#include "Appearance.h"
#include "WindowChrome.h"

#include <QGuiApplication>
#include <QLayout>
#include <QPointer>
#include <QtMath>

namespace xdnote {
namespace {
NSView *nativeView(const QWidget *widget) {
    return (__bridge NSView *)reinterpret_cast<void *>(widget->internalWinId());
}

void reserveControls(NSView *view, NSView *content, const NSRect &header, QMargins &insets) {
    if (view == content || view.hidden) return;
    const NSRect bounds = [view convertRect:view.bounds toView:content];
    if (!NSIntersectsRect(bounds, header)) return;
    if ([view isKindOfClass:[NSControl class]]) {
        if (NSMidX(bounds) < NSMidX(header))
            insets.setLeft(qMax(insets.left(), qCeil(NSMaxX(bounds) - NSMinX(header))));
        else
            insets.setRight(qMax(insets.right(), qCeil(NSMaxX(header) - NSMinX(bounds))));
        return;
    }
    for (NSView *child in view.subviews)
        reserveControls(child, content, header, insets);
}

NSRect headerRect(NSView *content, int rowTop, int rowHeight) {
    const NSRect bounds = content.bounds;
    const CGFloat top = content.flipped ? NSMinY(bounds) + rowTop : NSMaxY(bounds) - rowTop - rowHeight;
    return NSMakeRect(NSMinX(bounds), top, NSWidth(bounds), rowHeight);
}

void alignWindowButtons(NSView *content, const NSRect &header) {
    NSButton *anchor = [content.window standardWindowButton:NSWindowCloseButton];
    NSView *frame = content.superview;
    if (!frame || !anchor || anchor.hiddenOrHasHiddenAncestor) return;

    // Move the window's native titlebar overlay with its original controls.
    // Moving only the buttons can push part of their hit area out of a short titlebar.
    NSView *overlay = anchor;
    while (overlay.superview && overlay.superview != frame)
        overlay = overlay.superview;
    if (overlay == content || overlay.superview != frame) return;

    const NSRect button = [anchor convertRect:anchor.bounds toView:frame];
    const CGFloat center = [content convertPoint:NSMakePoint(NSMidX(header), NSMidY(header)) toView:frame].y;
    const CGFloat offset = center - NSMidY(button);
    if (qAbs(offset) < 0.01) return;
    NSPoint origin = overlay.frame.origin;
    origin.y += offset;
    [overlay setFrameOrigin:origin];
}

QMargins nativeInsets(NSView *content, const NSRect &header) {
    // Measure the controls in the frame outside the Qt content view,
    // including system accessories, without relying on AppKit class names.
    QMargins result;
    for (NSView *view in content.superview.subviews)
        reserveControls(view, content, header, result);
    return result;
}

void updateHeader(QWidget *window, WindowTitleBar *header, NSView *content) {
    // The normal caption is ours to lay out. Full-screen layout still
    // respects Qt's safe area, including the screen's camera housing.
    const bool useSafeArea = window->isFullScreen()
        || (content.window.styleMask & NSWindowStyleMaskFullScreen);
    if (window->testAttribute(Qt::WA_ContentsMarginsRespectsSafeArea) != useSafeArea) {
        window->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, useSafeArea);
        header->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, useSafeArea);
        window->layout()->invalidate();
        header->layout()->invalidate();
        window->layout()->activate();
    }
    const NSRect row = headerRect(content, header->mapTo(window, QPoint()).y(), header->height());
    if (!useSafeArea)
        alignWindowButtons(content, row);
    header->setNativeInsets(nativeInsets(content, row));
}

class TitlebarObserver final : public QObject {
public:
    TitlebarObserver(ApplicationWindow *widget, WindowTitleBar *header)
        : QObject(widget) {
        const QPointer<ApplicationWindow> owner(widget);
        const QPointer<WindowTitleBar> title(header);
        m_observer = [NSNotificationCenter.defaultCenter
            addObserverForName:NSWindowDidUpdateNotification object:nil queue:nil
            usingBlock:^(NSNotification *notification) {
                if (!owner || !owner->isWindow() || !title || !title->isVisible()) return;
                NSView *content = nativeView(owner);
                if (content.window == notification.object)
                    updateHeader(owner, title, content);
            }];
    }

    ~TitlebarObserver() override {
        [NSNotificationCenter.defaultCenter removeObserver:m_observer];
    }

private:
    id m_observer;
};
}

bool macNativeFullScreen(const QWidget *window) {
    if (QGuiApplication::platformName() != QStringLiteral("cocoa") || !window->internalWinId()) return false;
    return (nativeView(window).window.styleMask & NSWindowStyleMaskFullScreen) != 0;
}

void ApplicationWindow::applyMacWindowChrome() {
    if (!isWindow() || !m_windowHeader || QGuiApplication::platformName() != "cocoa") return;
    NSView *content = nativeView(this);
    NSWindow *window = content.window;
    if (!window) return;

    if (!(window.styleMask & NSWindowStyleMaskFullSizeContentView)) {
        const CGFloat contentHeight = NSHeight(content.bounds);
        window.styleMask |= NSWindowStyleMaskFullSizeContentView;
        // Retain the requested logical client height and the top edge when
        // AppKit expands the content view into the previous native caption.
        NSRect frame = window.frame;
        const CGFloat growth = NSHeight(content.bounds) - contentHeight;
        if (growth > 0) {
            frame.origin.y += growth;
            frame.size.height -= growth;
            [window setFrame:frame display:NO];
        }
    }
    window.titleVisibility = NSWindowTitleHidden;
    window.titlebarAppearsTransparent = YES;
    window.titlebarSeparatorStyle = NSTitlebarSeparatorStyleNone;
    NSAppearanceName scheme = windowAppearance()->dark() ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua;
    if (![window.appearance.name isEqualToString:scheme])
        window.appearance = [NSAppearance appearanceNamed:scheme];

    updateHeader(this, m_windowHeader, content);
    if (!m_macChromeObserver)
        m_macChromeObserver = new TitlebarObserver(this, m_windowHeader);
}
}
