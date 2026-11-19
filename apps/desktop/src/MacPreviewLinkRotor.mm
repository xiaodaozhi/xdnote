#include "MacPreviewLinkRotor.h"
#include "NativePreviewView.h"

#import <AppKit/AppKit.h>
#import <objc/runtime.h>
#include <QGuiApplication>
#include <QPointer>
#include <QWindow>

namespace {
NSWindow *previewWindow(xdnote::NativePreviewView *preview)
{
    if (QGuiApplication::platformName() != QStringLiteral("cocoa"))
        return nil;
    QWindow *handle = preview->window()->windowHandle();
    if (!handle)
        return nil;
    auto *nativeView = (__bridge NSView *)reinterpret_cast<void *>(handle->winId());
    return nativeView.window;
}

id<NSAccessibility> findPreview(id<NSAccessibility> element)
{
    if ([element respondsToSelector:@selector(accessibilityIdentifier)]
        && [element.accessibilityIdentifier isEqualToString:@(xdnote::MacPreviewAccessibleId)])
        return element;
    if ([element respondsToSelector:@selector(accessibilityChildren)]) {
        for (id<NSAccessibility> child in element.accessibilityChildren) {
            if (auto reader = findPreview(child))
                return reader;
        }
    }
    return nil;
}

QAccessible::Id currentLinkId(NSAccessibilityCustomRotorItemResult *current)
{
    id token = current.itemLoadingToken;
    if ([token isKindOfClass:NSNumber.class])
        return [token unsignedIntValue];
    id<NSAccessibility> target = (id<NSAccessibility>)current.targetElement;
    if (![target respondsToSelector:@selector(accessibilityIdentifier)])
        return 0;
    NSString *identifier = target.accessibilityIdentifier;
    NSString *prefix = @(xdnote::MacPreviewLinkIdPrefix);
    if (![identifier hasPrefix:prefix])
        return 0;
    return QString::fromNSString([identifier substringFromIndex:prefix.length]).toUInt();
}
}

@interface XdnotePreviewLinkRotor : NSObject <NSAccessibilityCustomRotorItemSearchDelegate, NSAccessibilityElementLoading> {
@public
    QPointer<xdnote::NativePreviewView> preview;
}
@property (weak) NSWindow *window;
@property (strong) NSAccessibilityCustomRotor *rotor;
@end

@implementation XdnotePreviewLinkRotor
- (NSAccessibilityCustomRotorItemResult *)rotor:(NSAccessibilityCustomRotor *)rotor
                      resultForSearchParameters:(NSAccessibilityCustomRotorSearchParameters *)search
{
    if (!preview || !preview->isVisible() || !preview->ready())
        return nil;
    auto *reader = QAccessible::queryAccessibleInterface(preview);
    if (!reader)
        return nil;
    auto *current = QAccessible::accessibleInterface(currentLinkId(search.currentItem));
    const int indexOfCurrent = current && current->isValid() ? reader->indexOfChild(current) : -1;
    const int count = reader->childCount();
    const bool forward = search.searchDirection == NSAccessibilityCustomRotorSearchDirectionNext;
    const int end = forward ? count : -1;
    const int step = forward ? 1 : -1;
    int index = indexOfCurrent < 0 ? (forward ? 0 : count - 1) : indexOfCurrent + step;
    for (; index != end; index += step) {
        auto *child = reader->child(index);
        if (!child || !child->isValid() || child->role() != QAccessible::Link)
            continue;
        NSString *label = child->text(QAccessible::Name).toNSString();
        if (search.filterString.length && ![label localizedCaseInsensitiveContainsString:search.filterString])
            continue;
        return [[NSAccessibilityCustomRotorItemResult alloc] initWithItemLoadingToken:@(QAccessible::uniqueId(child))
                                                                          customLabel:label];
    }
    return nil;
}

- (id<NSAccessibilityElement>)accessibilityElementWithToken:(NSAccessibilityLoadingToken)token
{
    if (!preview || !preview->isVisible() || ![token isKindOfClass:NSNumber.class])
        return nil;
    const QAccessible::Id linkId = [(NSNumber *)token unsignedIntValue];
    auto *reader = QAccessible::queryAccessibleInterface(preview);
    auto *link = QAccessible::accessibleInterface(linkId);
    if (!reader || !link || !link->isValid() || link->role() != QAccessible::Link || link->parent() != reader)
        return nil;

    // Qt Cocoa drops offscreen elements from the native tree. Reveal the link
    // only when the rotor loads it, then hand VoiceOver the original AXLink.
    if (link->state().offscreen) {
        if (auto *actions = link->actionInterface())
            actions->doAction(QAccessibleActionInterface::setFocusAction());
    }
    if (!preview || !preview->isVisible())
        return nil;
    NSString *identifier = xdnote::macPreviewLinkId(linkId).toNSString();
    for (id<NSAccessibility> child in findPreview(self.window).accessibilityChildren) {
        if ([child.accessibilityIdentifier isEqualToString:identifier])
            return (id<NSAccessibilityElement>)child;
    }
    return nil;
}
@end

namespace xdnote {
void setMacPreviewLinkRotor(NativePreviewView *preview, bool visible)
{
    NSWindow *window = previewWindow(preview);
    if (!window)
        return;

    static char association;
    auto *delegate = (XdnotePreviewLinkRotor *)objc_getAssociatedObject(window, &association);
    if (!visible) {
        if (delegate && delegate->preview == preview) {
            NSMutableArray *rotors = [window.accessibilityCustomRotors mutableCopy];
            [rotors removeObjectIdenticalTo:delegate.rotor];
            window.accessibilityCustomRotors = rotors;
            objc_setAssociatedObject(window, &association, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        }
        return;
    }
    if (!delegate) {
        delegate = [[XdnotePreviewLinkRotor alloc] init];
        delegate.window = window;
        delegate.rotor = [[NSAccessibilityCustomRotor alloc] initWithRotorType:NSAccessibilityCustomRotorTypeLink
                                                             itemSearchDelegate:delegate];
        delegate.rotor.label = @"链接";
        delegate.rotor.itemLoadingDelegate = delegate;
        window.accessibilityCustomRotors = [(window.accessibilityCustomRotors ?: @[]) arrayByAddingObject:delegate.rotor];
        objc_setAssociatedObject(window, &association, delegate, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    delegate->preview = preview;
}

void notifyMacPreviewNavigation(NativePreviewView *preview)
{
    if (!NSWorkspace.sharedWorkspace.voiceOverEnabled)
        return;
    NSWindow *window = previewWindow(preview);
    if (!window.isKeyWindow)
        return;
    id<NSAccessibility> reader = findPreview(window);
    id<NSAccessibility> body = [(id)reader accessibilityFocusedUIElement];
    if (!body.isAccessibilityFocused)
        return;
    // VoiceOver can retain the source link as its reading cursor after a Qt focus event.
    NSAccessibilityPostNotificationWithUserInfo(reader, NSAccessibilityLayoutChangedNotification,
                                               @{NSAccessibilityUIElementsKey: @[body]});
}
}
