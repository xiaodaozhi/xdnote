#include "guarded_quick_accessibility.h"

#include <QQuickWindow>
#include <QTextCursor>
#include <QTextDocument>
#include <QtTest>

#import <AppKit/AppKit.h>

// Qt 6.8's Cocoa bridge implements these Objective-C selectors for AX writes
// and for traversing the QNSView root.
@protocol Qt68AccessibilityAttributes
- (id)accessibilityAttributeValue:(NSString *)attribute;
- (BOOL)accessibilityIsAttributeSettable:(NSString *)attribute;
- (void)accessibilitySetValue:(id)value forAttribute:(NSString *)attribute;
@end

namespace {
id findTextElement(id element, int depth = 0)
{
    if (depth >= 8)
        return nil;
    if ([element respondsToSelector:@selector(accessibilityValue)]
        && [[element accessibilityValue] isEqual:@"original"])
        return element;
    id<Qt68AccessibilityAttributes> attributes = element;
    for (id child in [attributes accessibilityAttributeValue:NSAccessibilityChildrenAttribute]) {
        if (id match = findTextElement(child, depth + 1))
            return match;
    }
    return nil;
}

QString nativeValue(id element)
{
    id value = [element accessibilityValue];
    return [value isKindOfClass:NSString.class] ? QString::fromNSString(value) : QString();
}
}

void verifyGuardedCocoaAccessibility(QQuickWindow &window, GuardedFixtureTextEdit &editor)
{
    @autoreleasepool {
        editor.setText(QStringLiteral("original"));
        NSView *view = reinterpret_cast<NSView *>(window.winId());
        id element = findTextElement(view);
        QVERIFY2(element, "The Qt editor was not found through the Cocoa accessibility tree");

        auto document = std::make_shared<QTextDocument>();
        document->setPlainText(QStringLiteral("new"));
        QTextCursor cursor(document.get());
        cursor.movePosition(QTextCursor::End);
        cursor.insertText(QStringLiteral(" source"));
        QCOMPARE(editor.replaceOwnedDocument(document), FixtureEditResult::Applied);
        QCOMPARE(nativeValue(element), QStringLiteral("new source"));
        QCOMPARE([element accessibilityNumberOfCharacters], NSInteger(10));

        editor.select(4, 10);
        QCOMPARE(QString::fromNSString([element accessibilitySelectedText]), QStringLiteral("source"));

        id<Qt68AccessibilityAttributes> attributes = element;
        editor.setReadOnly(true);
        [attributes accessibilitySetValue:@"rejected" forAttribute:NSAccessibilityValueAttribute];
        QCOMPARE(nativeValue(element), QStringLiteral("new source"));
        QVERIFY(document->isUndoAvailable());

        editor.setReadOnly(false);
        QVERIFY([attributes accessibilityIsAttributeSettable:NSAccessibilityValueAttribute]);
        [attributes accessibilitySetValue:@"accepted" forAttribute:NSAccessibilityValueAttribute];
        QCOMPARE(nativeValue(element), QStringLiteral("accepted"));
        document->undo();
        QCOMPARE(nativeValue(element), QStringLiteral("new source"));
        document->undo();
        QCOMPARE(nativeValue(element), QStringLiteral("new"));
    }
}
