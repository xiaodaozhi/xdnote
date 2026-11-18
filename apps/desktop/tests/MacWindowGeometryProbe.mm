#import <AppKit/AppKit.h>

#include "Appearance.h"
#include "FileManager.h"
#include "NavigationStore.h"
#include "SettingsWindow.h"
#include "SingleFileWindow.h"
#include "WorkspaceWindow.h"

#include <QDialog>
#include <QFile>
#include <QLayout>
#include <QMouseEvent>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

namespace {
NSView *nativeView(QWidget *window) {
    return reinterpret_cast<NSView *>(window->internalWinId());
}

void verifyHeader(QWidget &window, const QSize &logicalSize) {
    auto *header = window.findChild<QWidget *>("documentHeader", Qt::FindDirectChildrenOnly);
    QVERIFY(header && header->isVisible());
    auto *view = nativeView(&window);
    auto *native = view.window;
    QVERIFY(native);
    QVERIFY(native.styleMask & NSWindowStyleMaskFullSizeContentView);
    QCOMPARE(native.titleVisibility, NSWindowTitleHidden);
    QVERIFY(native.titlebarAppearsTransparent);
    QVERIFY(native.title.length > 0);
    QTRY_COMPARE(window.size(), logicalSize);
    QCOMPARE(QSize(qRound(NSWidth(view.bounds)), qRound(NSHeight(view.bounds))), logicalSize);
    QTRY_COMPARE(header->mapTo(&window, QPoint()), QPoint());
    QCOMPARE(header->height(), 48);
    for (NSWindowButton kind : {NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton}) {
        NSButton *button = [native standardWindowButton:kind];
        QVERIFY(button && !button.hiddenOrHasHiddenAncestor);
        const NSRect rect = [button convertRect:button.bounds toView:view];
        const qreal bottom = view.flipped ? NSMaxY(rect) : NSMaxY(view.bounds) - NSMinY(rect);
        QVERIFY(bottom <= header->height());
        QTRY_VERIFY(header->layout()->contentsMargins().left() >= NSMaxX(rect));
    }
}
}

// The fixture must observe completed native zoom, not just Qt's requested
// state, before requesting the opposite transition. No private Qt APIs.
bool macWindowIsZoomed(QWidget *window) {
    if (!window || !window->internalWinId()) return false;
    NSView *view = reinterpret_cast<NSView *>(window->internalWinId());
    return view.window && view.window.isZoomed;
}

void verifyMacApplicationTitlebars() {
    using namespace xdnote;
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    Appearance appearance(nullptr, temp.filePath("settings.json"));
    QVERIFY(appearance.setPreferences("light", "graphite"));
    NavigationStore navigation(nullptr, temp.filePath("navigation.json"));
    FileManager manager(nullptr, {}, false);
    WorkspaceWindow workspace(&appearance, nullptr, &navigation, &manager);
    SettingsWindow settings(&appearance, nullptr);
    SingleFileWindow document(&manager, &appearance);
    const auto name = QStringLiteral("标题栏测试").repeated(8) + ".md";
    QFile source(temp.filePath(name));
    QVERIFY(source.open(QIODevice::WriteOnly));
    source.close();
    workspace.setRootPath(temp.path());
    QSignalSpy opened(&manager, &FileManager::opened);
    manager.open(source.fileName());
    QTRY_COMPARE(opened.count(), 1);
    auto *file = qvariant_cast<FileDocument *>(opened.first().at(1));
    QVERIFY(file);
    document.setDocument(file);
    for (auto *window : {static_cast<QWidget *>(&document), static_cast<QWidget *>(&workspace),
                         static_cast<QWidget *>(&settings)}) {
        window->setAttribute(Qt::WA_ShowWithoutActivating);
        window->setAttribute(Qt::WA_DeleteOnClose, false);
        window->resize(window->minimumSize());
        const auto size = window->size();
        window->show();
        verifyHeader(*window, size);
        window->hide();
        window->show();
        verifyHeader(*window, size);
    }

    QVERIFY(QString::fromNSString(nativeView(&document).window.title).contains(name));
    QCOMPARE(QString::fromNSString(nativeView(&document).window.appearance.name),
             QString::fromNSString(NSAppearanceNameAqua));
    const auto normalSize = document.size();
    [nativeView(&document).window performMiniaturize:nil];
    QTRY_VERIFY(nativeView(&document).window.miniaturized);
    [nativeView(&document).window deminiaturize:nil];
    QTRY_VERIFY(!nativeView(&document).window.miniaturized);
    verifyHeader(document, normalSize);

    NSNotificationCenter *notifications = NSNotificationCenter.defaultCenter;
    NSWindow *documentWindow = nativeView(&document).window;
    __block bool enteredFullscreen = false;
    id entered = [notifications addObserverForName:NSWindowDidEnterFullScreenNotification
                                            object:documentWindow queue:nil usingBlock:^(NSNotification *) {
        enteredFullscreen = true;
    }];
    const auto removeObserver = qScopeGuard([&] { [notifications removeObserver:entered]; });
    document.showFullScreen();
    QTRY_VERIFY(enteredFullscreen);
    QTRY_VERIFY(document.testAttribute(Qt::WA_ContentsMarginsRespectsSafeArea));
    document.showNormal();
    QTRY_VERIFY(!(nativeView(&document).window.styleMask & NSWindowStyleMaskFullScreen));
    verifyHeader(document, normalSize);

    auto *documentHeader = document.findChild<QWidget *>("documentHeader", Qt::FindDirectChildrenOnly);
    QVERIFY(documentHeader);
    const auto origin = document.pos();
    const QPoint start(documentHeader->width() / 2, documentHeader->height() / 2);
    const auto global = documentHeader->mapToGlobal(start);
    const QPoint delta(36, 12);
    QMouseEvent press(QEvent::MouseButtonPress, start, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(documentHeader, &press);
    QMouseEvent drag(QEvent::MouseMove, start + delta, global + delta, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(documentHeader, &drag);
    QTRY_COMPARE(document.pos(), origin + delta);
    QMouseEvent release(QEvent::MouseButtonRelease, start, global + delta, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(documentHeader, &release);
    QCOMPARE(document.size(), normalSize);

    QVERIFY(appearance.setPreferences("dark", "graphite"));
    QVERIFY(appearance.dark());
    QTRY_COMPARE(QString::fromNSString(nativeView(&document).window.appearance.name),
                 QString::fromNSString(NSAppearanceNameDarkAqua));
    document.setEditing(true);
    auto *session = file->session();
    QCOMPARE(session->replace(0, 0, "unsaved", session->revision()), EditResult::Applied);
    QTRY_VERIFY(nativeView(&document).window.documentEdited);
    QSignalSpy cancelled(&document, &SingleFileWindow::closeCancelled);
    QTimer dismiss;
    QObject::connect(&dismiss, &QTimer::timeout, &document, [&] {
        if (auto *dialog = document.findChild<QDialog *>("unsavedConfirmation")) dialog->reject();
    });
    dismiss.start(10);
    [nativeView(&document).window performClose:nil];
    QCOMPARE(cancelled.count(), 1);
    QVERIFY(document.isVisible());
    QCOMPARE(session->snapshot().utf8, QByteArray("unsaved"));
    dismiss.stop();

    QVERIFY(workspace.addPane(&document));
    QCOMPARE(workspace.activePane(), &document);
    QVERIFY(!document.isWindow());
    QVERIFY(!document.findChild<QWidget *>("documentHeader", Qt::FindDirectChildrenOnly)->isVisible());
    QCOMPARE(file->session(), session);
    QCOMPARE(session->snapshot().utf8, QByteArray("unsaved"));
    auto *header = workspace.findChild<QWidget *>("documentHeader", Qt::FindDirectChildrenOnly);
    QVERIFY(header);
    NSView *view = nativeView(&workspace);
    NSWindow *window = view.window;
    NSView *container = [window standardWindowButton:NSWindowZoomButton].superview;
    QVERIFY(container);
    const auto previous = header->layout()->contentsMargins().left();
    NSButton *accessory = [NSButton buttonWithTitle:@"Test" target:nil action:nil];
    const CGFloat top = view.flipped ? NSMinY(view.bounds) : NSMaxY(view.bounds) - 24;
    accessory.frame = [view convertRect:NSMakeRect(previous - 32, top, 48, 24) toView:container];
    [container addSubview:accessory];
    [window update];
    QTRY_VERIFY(header->layout()->contentsMargins().left() > previous);
    [accessory removeFromSuperview];
    [window update];
    QTRY_COMPARE(header->layout()->contentsMargins().left(), previous);
}
