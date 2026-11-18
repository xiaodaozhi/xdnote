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

QMargins nativeInsets(NSView *content, int rowTop, int rowHeight) {
    // Measure the controls in the frame outside the Qt content view,
    // including system accessories, without relying on AppKit class names.
    const NSRect bounds = content.bounds;
    const CGFloat top = content.flipped ? NSMinY(bounds) + rowTop : NSMaxY(bounds) - rowTop - rowHeight;
    const NSRect header = NSMakeRect(NSMinX(bounds), top, NSWidth(bounds), rowHeight);
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
    header->setNativeInsets(nativeInsets(content, header->mapTo(window, QPoint()).y(), header->height()));
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
