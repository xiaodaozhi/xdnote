#include "FileApplication.h"
#include "WorkspaceWindow.h"
#include "WorkspaceTree.h"
#include "SingleFileWindow.h"
#include "SourceEditor.h"
#include "MarkdownPreview.h"
#include "Appearance.h"
#include "SharedWidgets.h"
#include "GuiRuntime.h"
#include "InputModality.h"
#include "file_fixtures.h"
#include <QApplication>
#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTabBar>
#include <QStackedWidget>
#include <QInputMethodEvent>
#include <QDialog>
#include <QElapsedTimer>
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QWebEngineScript>
#include <QJsonDocument>
#include <QJsonObject>
#ifdef Q_OS_WIN
#include <sddl.h>
#endif

using namespace xdnote;
namespace {
SingleFileWindow *waitPane(WorkspaceWindow *workspace, const QString &path) {
    QElapsedTimer timer; timer.start(); while (timer.elapsed() < 10000) { for (auto *pane : workspace->panes()) if (pane->document() && pane->document()->path() == path) return pane; QTest::qWait(10); } return nullptr;
}
SingleFileWindow *waitStandalone(const QString &path) {
    QElapsedTimer timer; timer.start(); while (timer.elapsed() < 10000) { for (auto *widget : QApplication::topLevelWidgets()) if (auto *pane = qobject_cast<SingleFileWindow *>(widget)) if (pane->document() && pane->document()->path() == path) return pane; QTest::qWait(10); } return nullptr;
}
QByteArray disk(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
void capture(QWidget *window, const QString &name) { const auto directory = qEnvironmentVariable("XDNOTE_TEST_EVIDENCE"); if (!directory.isEmpty()) { QDir().mkpath(directory + "/workspace"); window->grab().save(directory + "/workspace/" + name + ".png"); } }
void chooseClose(int choice) { QTimer::singleShot(30, qApp, [choice] { for (auto *widget : QApplication::topLevelWidgets()) if (widget->objectName() == "unsavedConfirmation") { if (auto *button = widget->findChild<SharedButton *>(QStringLiteral("choice%1").arg(choice))) button->click(); } }); }
}
class WorkspaceTests final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qApp->setQuitOnLastWindowClosed(false); SourceEditor::installAccessibility(); }
    void directoryLoadingEmptyMissingAndNoFilesystemWrites() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); FileApplication desktop; auto *workspace = desktop.workspaceWindow(dir.path());
        QTRY_COMPARE(workspace->tree()->state(), WorkspaceTree::State::Empty); QVERIFY(!QFileInfo::exists(dir.filePath("assets")));
        QVERIFY(fileFixtures::write(dir.filePath(QStringLiteral("中文.md")), "# document")); QVERIFY(fileFixtures::write(dir.filePath("ignore.txt"), "unrelated")); QVERIFY(QDir().mkdir(dir.filePath("notes")));
        workspace->tree()->refresh(); QTRY_COMPARE(workspace->tree()->state(), WorkspaceTree::State::Ready); QCOMPARE(workspace->tree()->model()->rowCount(), 2);
        QVERIFY(workspace->tree()->acceptsFile(dir.filePath(QStringLiteral("中文.md")))); QVERIFY(!workspace->tree()->acceptsFile(dir.filePath("ignore.txt"))); QVERIFY(!workspace->tree()->acceptsFile(dir.filePath("../outside.md")));
        workspace->setRootPath(dir.filePath("missing")); QTRY_COMPARE(workspace->tree()->state(), WorkspaceTree::State::Missing); QVERIFY(!workspace->tree()->message().isEmpty());
        workspace->setRootPath(dir.path()); QTRY_COMPARE(workspace->tree()->state(), WorkspaceTree::State::Ready); QVERIFY(!QFileInfo::exists(dir.filePath("assets")));
        QCOMPARE(disk(dir.filePath("ignore.txt")), QByteArray("unrelated"));
    }
    void keyboardModesSkipHiddenControlsAndFollowWorkspaceOrder() {
        QTemporaryDir dir; const auto path = dir.filePath("keyboard.md"); QVERIFY(fileFixtures::write(path, "keyboard")); FileApplication desktop; desktop.open(path); auto *pane = waitStandalone(path); QVERIFY(pane);
        auto *read = pane->findChild<SharedButton *>("readMode"), *edit = pane->findChild<SharedButton *>("editMode"), *split = pane->findChild<SharedButton *>("splitMode"); QVERIFY(!split->isVisible());
        QTest::keyClick(read, Qt::Key_Right); QCOMPARE(pane->viewMode(), SingleFileWindow::ViewMode::Source); QTest::keyClick(edit, Qt::Key_Right); QCOMPARE(pane->viewMode(), SingleFileWindow::ViewMode::Reading);
        QVERIFY(desktop.transferToWorkspace(pane)); QTest::keyClick(edit, Qt::Key_Right); QCOMPARE(pane->viewMode(), SingleFileWindow::ViewMode::Split); QTest::keyClick(split, Qt::Key_Right); QCOMPARE(pane->viewMode(), SingleFileWindow::ViewMode::Reading);
        QCOMPARE(pane->findChild<QWidget *>("modeSwitch")->font().pixelSize(), 14);
    }
    void unreadableDirectoryRecoversAfterPermissionRestored() {
        QTemporaryDir dir; const auto path = dir.filePath("retained.md"); QVERIFY(fileFixtures::write(path, "retained"));
#ifdef Q_OS_WIN
        const auto native = QDir::toNativeSeparators(dir.path()); const auto *name = reinterpret_cast<LPCWSTR>(native.utf16()); DWORD needed = 0;
        GetFileSecurityW(name, DACL_SECURITY_INFORMATION, nullptr, 0, &needed); QVERIFY(needed > 0); QByteArray original(int(needed), '\0'); QVERIFY(GetFileSecurityW(name, DACL_SECURITY_INFORMATION, original.data(), needed, &needed));
        struct Restore { QString path; QByteArray descriptor; ~Restore() { if (!path.isEmpty()) SetFileSecurityW(reinterpret_cast<LPCWSTR>(path.utf16()), DACL_SECURITY_INFORMATION, descriptor.data()); } } restore{native, original};
        PSECURITY_DESCRIPTOR denied = nullptr; QVERIFY(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(D;;0x1;;;WD)(A;;FA;;;OW)", SDDL_REVISION_1, &denied, nullptr)); const bool changed = SetFileSecurityW(name, DACL_SECURITY_INFORMATION, denied); LocalFree(denied); QVERIFY(changed);
#else
        if (::geteuid() == 0) QSKIP("Root bypasses discretionary directory permissions");
        struct Restore { QString path; ~Restore() { if (!path.isEmpty()) ::chmod(path.toUtf8().constData(), 0700); } } restore{dir.path()}; QVERIFY(::chmod(dir.path().toUtf8().constData(), 0000) == 0);
#endif
        FileApplication desktop; auto *workspace = desktop.workspaceWindow(dir.path()); QTRY_COMPARE(workspace->tree()->state(), WorkspaceTree::State::Unreadable); QVERIFY(!workspace->tree()->message().isEmpty());
#ifdef Q_OS_WIN
        QVERIFY(SetFileSecurityW(name, DACL_SECURITY_INFORMATION, original.data()));
#else
        QVERIFY(::chmod(dir.path().toUtf8().constData(), 0700) == 0);
#endif
        restore.path.clear(); workspace->tree()->refresh(); QTRY_COMPARE(workspace->tree()->state(), WorkspaceTree::State::Ready); QCOMPARE(disk(path), QByteArray("retained")); QVERIFY(!QFileInfo::exists(dir.filePath("assets")));
    }
    void navigationSkipsLinkedDirectories() {
        QTemporaryDir dir, outside; QVERIFY(fileFixtures::write(outside.filePath("outside.md"), "outside"));
#ifdef Q_OS_WIN
        fileFixtures::Junction link; QVERIFY(link.create(dir.filePath("link"), outside.path()));
#else
        QVERIFY(::symlink(outside.path().toUtf8().constData(), dir.filePath("link").toUtf8().constData()) == 0);
#endif
        FileApplication desktop; auto *workspace = desktop.workspaceWindow(dir.path()); QTRY_COMPARE(workspace->tree()->state(), WorkspaceTree::State::Empty); QVERIFY(!workspace->tree()->acceptsFile(dir.filePath("link/outside.md")));
        workspace->setRootPath(dir.filePath("link")); QTRY_COMPARE(workspace->tree()->state(), WorkspaceTree::State::Unreadable);
    }
    void duplicateOpenAndReorderedInactiveTabCloseKeepUniqueDirtyBuffer() {
        QTemporaryDir dir; const auto first = dir.filePath("first.md"), second = dir.filePath("second.md"); QVERIFY(fileFixtures::write(first, "first")); QVERIFY(fileFixtures::write(second, "second"));
        FileApplication desktop; auto *workspace = desktop.workspaceWindow(dir.path()); desktop.openInWorkspace(first, workspace); auto *one = waitPane(workspace, first); QVERIFY(one);
        desktop.openInWorkspace(second, workspace); auto *two = waitPane(workspace, second); QVERIFY(two); QPointer<SingleFileWindow> guardedTwo = two;
        auto *session = one->document()->session(); const auto snapshot = session->snapshot(); QCOMPARE(session->replace(5, 5, "-dirty", snapshot.revision), EditResult::Applied); QTRY_VERIFY(one->document()->dirty());
        QVERIFY(fileFixtures::write(first, "external version")); desktop.openInWorkspace(first, workspace); QTRY_COMPARE(workspace->panes().size(), 2); QTRY_COMPARE(workspace->activePane(), one); QCOMPARE(one->document()->session(), session); QCOMPARE(session->snapshot().utf8, QByteArray("first-dirty"));
        desktop.open(first); QTest::qWait(100); QTRY_COMPARE(workspace->panes().size(), 2); QCOMPARE(session->snapshot().utf8, QByteArray("first-dirty"));
        auto *tabs = workspace->findChild<QTabBar *>("workspaceTabs"); tabs->moveTab(0, 1); workspace->activatePane(one->paneId());
        workspace->closePane(two->paneId()); QTRY_VERIFY(guardedTwo.isNull()); QCOMPARE(workspace->panes().size(), 1); QCOMPARE(workspace->activePane(), one); QCOMPARE(session->snapshot().utf8, QByteArray("first-dirty"));
    }
    void transferRetainsEditorUndoSelectionAndScrollWithoutImportingParent() {
        QTemporaryDir dir; const auto path = dir.filePath(QStringLiteral("接管.md")); QByteArray original; for (int i = 0; i < 150; ++i) original += "line\n"; QVERIFY(fileFixtures::write(path, original));
        FileApplication desktop; desktop.open(path); auto *pane = waitStandalone(path); QVERIFY(pane); pane->setEditing(true); auto *editor = pane->editor(); auto *session = pane->document()->session(); const auto id = session->documentId();
        QCOMPARE(session->replace(0, 0, "dirty\n", session->revision()), EditResult::Applied); editor->send(SCI_SETSEL, 3, 9); editor->send(SCI_SETFIRSTVISIBLELINE, 20); const auto line = editor->send(SCI_GETFIRSTVISIBLELINE);
        QVERIFY(desktop.transferToWorkspace(pane)); QVERIFY(pane->embedded()); auto *workspace = qobject_cast<WorkspaceWindow *>(pane->window()); QVERIFY(workspace); QVERIFY(workspace->rootPath().isEmpty()); QCOMPARE(workspace->tree()->state(), WorkspaceTree::State::Unselected);
        QCOMPARE(pane->editor(), editor); QCOMPARE(pane->document()->session(), session); QCOMPARE(session->documentId(), id); QCOMPARE(editor->send(SCI_GETANCHOR), sptr_t(3)); QCOMPARE(editor->send(SCI_GETCURRENTPOS), sptr_t(9)); QCOMPARE(editor->send(SCI_GETFIRSTVISIBLELINE), line);
        QCOMPARE(session->snapshot().utf8, QByteArray("dirty\n") + original); QCOMPARE(disk(path), original); QCOMPARE(session->undo(), EditResult::Applied); QCOMPARE(session->snapshot().utf8, original); QVERIFY(!QFileInfo::exists(dir.filePath("assets")));
    }
    void saveShortcutFromTabAndFooterTargetsOnlyActiveDocument() {
        QTemporaryDir dir; const auto first = dir.filePath("active.md"), second = dir.filePath("inactive.md"); QVERIFY(fileFixtures::write(first, "one")); QVERIFY(fileFixtures::write(second, "two")); FileApplication desktop; auto *workspace = desktop.workspaceWindow(dir.path()); desktop.openInWorkspace(first, workspace); auto *one = waitPane(workspace, first); QVERIFY(one); desktop.openInWorkspace(second, workspace); auto *two = waitPane(workspace, second); QVERIFY(two);
        QCOMPARE(one->document()->session()->replace(3, 3, "-save", one->document()->session()->revision()), EditResult::Applied); QCOMPARE(two->document()->session()->replace(3, 3, "-keep", two->document()->session()->revision()), EditResult::Applied);
        workspace->activatePane(one->paneId()); QApplication::setActiveWindow(workspace); auto *tabs = workspace->findChild<QTabBar *>("workspaceTabs"); tabs->setFocus(Qt::OtherFocusReason); QTRY_VERIFY(tabs->hasFocus()); QTest::keySequence(tabs, QKeySequence::Save); QTRY_VERIFY(!one->document()->dirty()); QVERIFY(two->document()->dirty()); QCOMPARE(disk(first), QByteArray("one-save")); QCOMPARE(disk(second), QByteArray("two"));
        workspace->activatePane(two->paneId()); auto *zoom = two->footer()->findChild<SharedButton *>("contentZoom"); zoom->setFocus(Qt::OtherFocusReason); QTRY_VERIFY(zoom->hasFocus()); QTest::keySequence(zoom, QKeySequence::Save); QTRY_VERIFY(!two->document()->dirty()); QCOMPARE(disk(second), QByteArray("two-keep"));
    }
    void compositionDefersTransferAndClosedDestinationPreservesStandalone() {
        QTemporaryDir dir; const auto path = dir.filePath("composition.md"); QVERIFY(fileFixtures::write(path, "base")); FileApplication desktop; desktop.open(path); auto *pane = waitStandalone(path); QVERIFY(pane); pane->setEditing(true);
        auto *target = desktop.workspaceWindow(); QInputMethodEvent preedit(QStringLiteral("pin"), {}); QApplication::sendEvent(pane->editor(), &preedit); QVERIFY(pane->document()->session()->isComposing()); QVERIFY(!desktop.transferToWorkspace(pane, target)); QVERIFY(pane->isWindow()); QVERIFY(target->panes().isEmpty());
        QInputMethodEvent commit; commit.setCommitString(QStringLiteral("中")); QApplication::sendEvent(pane->editor(), &commit); QTRY_VERIFY(pane->embedded()); QCOMPARE(pane->window(), static_cast<QWidget *>(target)); QVERIFY(pane->document()->session()->snapshot().utf8.contains(QStringLiteral("中").toUtf8()));
        const auto otherPath = dir.filePath("preserve.md"); QVERIFY(fileFixtures::write(otherPath, "preserved")); desktop.open(otherPath); auto *other = waitStandalone(otherPath); QVERIFY(other);
        auto *closing = desktop.workspaceWindow(dir.path()); closing->close(); QVERIFY(closing->closingWorkspace()); QVERIFY(!desktop.transferToWorkspace(other, closing)); QVERIFY(other->isWindow()); QCOMPARE(other->document()->session()->snapshot().utf8, QByteArray("preserved"));
    }
    void readingPositionSurvivesNativeHostTransfer() {
        QTemporaryDir dir; const auto path = dir.filePath("reader.md"); QByteArray text; for (int i = 0; i < 180; ++i) text += "paragraph\n\n"; QVERIFY(fileFixtures::write(path, text)); FileApplication desktop; desktop.open(path); auto *pane = waitStandalone(path); QVERIFY(pane);
        QTRY_VERIFY(pane->preview()->view()->page()->contentsSize().height() > 1000);
        pane->preview()->restoreReadingPosition({0, 500}); QTRY_VERIFY(pane->preview()->view()->page()->scrollPosition().y() >= 490);
        const auto position = pane->preview()->readingPosition(); auto *view = pane->preview()->view(); QVERIFY(desktop.transferToWorkspace(pane)); QCOMPARE(pane->preview()->view(), view); QTest::qWait(200); QTRY_VERIFY(qAbs(view->page()->scrollPosition().y() - position.y()) <= 2); capture(pane->window(), "transferred-reader"); QCOMPARE(disk(path), text);
    }
    void failedOpenTabsOverflowAndKeyboardCloseRemainUsable() {
        QTemporaryDir dir; FileApplication desktop; auto *workspace = desktop.workspaceWindow(dir.path());
        for (int i = 0; i < 12; ++i) desktop.openInWorkspace(dir.filePath(QStringLiteral("missing-document-with-a-long-name-%1.md").arg(i)), workspace);
        auto *tabs = workspace->findChild<QTabBar *>("workspaceTabs"); QTRY_COMPARE(tabs->count(), 12); QVERIFY(tabs->usesScrollButtons());
        auto *pane = workspace->activePane(); QVERIFY(pane); QPointer<SingleFileWindow> guard = pane; QTest::keyClick(tabs, Qt::Key_Delete); QTRY_VERIFY(guard.isNull()); QCOMPARE(tabs->count(), 11);
        const auto path = dir.filePath("valid.md"); QVERIFY(fileFixtures::write(path, "valid")); desktop.openInWorkspace(path, workspace); pane = waitPane(workspace, path); QVERIFY(pane);
        workspace->resize(960, 640); QTest::qWait(30); auto *sidebar = workspace->findChild<QWidget *>("workspaceSidebar"); QVERIFY(!sidebar->isVisible());
        QTest::mouseClick(pane->findChild<SharedButton *>("fileLocation"), Qt::LeftButton); QVERIFY(sidebar->isVisible()); QTest::mouseClick(pane->findChild<SharedButton *>("fileLocation"), Qt::LeftButton); QVERIFY(!sidebar->isVisible());
    }
    void windowsTabsAreClientControlsInsideSharedCaption() {
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName() != "windows") QSKIP("Requires the Windows platform plugin");
        QTemporaryDir dir; const auto path = dir.filePath("caption.md"); QVERIFY(fileFixtures::write(path, "caption")); FileApplication desktop; auto *workspace = desktop.workspaceWindow(dir.path()); desktop.openInWorkspace(path, workspace); QVERIFY(waitPane(workspace, path));
        const auto hwnd = reinterpret_cast<HWND>(workspace->winId()); auto hit = [workspace, hwnd](QPoint logical) { POINT origin{}; ClientToScreen(hwnd, &origin); return SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(origin.x + qRound(logical.x() * workspace->devicePixelRatioF()), origin.y + qRound(logical.y() * workspace->devicePixelRatioF()))); };
        auto *tabs = workspace->findChild<QTabBar *>("workspaceTabs"); QCOMPARE(tabs->font().pixelSize(), 14); QCOMPARE(hit(tabs->mapTo(workspace, tabs->tabRect(0).center())), LRESULT(HTCLIENT)); QCOMPARE(hit(QPoint(180, 24)), LRESULT(HTCAPTION));
        auto *maximize = workspace->findChild<SharedButton *>("captionMaximize"); QVERIFY(maximize); QCOMPARE(maximize->size(), QSize(48, 48)); QCOMPARE(hit(maximize->mapTo(workspace, maximize->rect().center())), LRESULT(HTMAXBUTTON));
#else
        QSKIP("Windows native caption is platform specific");
#endif
    }
    void splitThemeGeometryAndSaveUseTheSameSession_data() { QTest::addColumn<QString>("theme"); QTest::newRow("light") << QString("light"); QTest::newRow("dark") << QString("dark"); }
    void splitThemeGeometryAndSaveUseTheSameSession() {
        QFETCH(QString, theme); QTemporaryDir dir; const auto path = dir.filePath(QStringLiteral("长文档标题用于核对标签省略与位置.md")); QVERIFY(fileFixtures::write(path, "# Heading\n\nbody\n")); FileApplication desktop; auto *workspace = desktop.workspaceWindow(dir.path()); desktop.openInWorkspace(path, workspace); auto *pane = waitPane(workspace, path); QVERIFY(pane); QVERIFY(pane->appearance()->setPreferences(theme, "graphite"));
        auto *session = pane->document()->session(); const auto id = session->documentId(); pane->setViewMode(SingleFileWindow::ViewMode::Split); QTRY_VERIFY(pane->editor()->isVisible()); QTRY_VERIFY(pane->preview()->isVisible());
        QCOMPARE(session->replace(0, 0, "edit\n", session->revision()), EditResult::Applied); QTRY_VERIFY(pane->document()->dirty()); QVERIFY(pane->document()->save()); QTRY_VERIFY(!pane->document()->saving()); QVERIFY(!pane->document()->dirty()); QCOMPARE(disk(path), QByteArray("edit\n# Heading\n\nbody\n"));
        pane->setViewMode(SingleFileWindow::ViewMode::Reading); pane->setViewMode(SingleFileWindow::ViewMode::Source); pane->setViewMode(SingleFileWindow::ViewMode::Split); QCOMPARE(pane->document()->session()->documentId(), id); QCOMPARE(pane->document()->session(), session);
        QCOMPARE(workspace->findChild<QWidget *>("documentHeader")->height(), 48); QCOMPARE(workspace->findChild<QWidget *>("workspaceFooters")->height(), 26); QCOMPARE(pane->footer()->width(), workspace->width()); QVERIFY(!pane->windowGeometry()); QVERIFY(workspace->windowGeometry());
        QTest::qWait(250); capture(workspace, theme + "-split");
    }
    void tabCloseCancelAndWorkspaceCloseSaveProtectOtherBuffers() {
        QTemporaryDir dir; const auto first = dir.filePath("first.md"), second = dir.filePath("second.md"); QVERIFY(fileFixtures::write(first, "one")); QVERIFY(fileFixtures::write(second, "two")); FileApplication desktop; QPointer<WorkspaceWindow> workspace = desktop.workspaceWindow(dir.path()); desktop.openInWorkspace(first, workspace); auto *one = waitPane(workspace, first); QVERIFY(one); desktop.openInWorkspace(second, workspace); auto *two = waitPane(workspace, second); QVERIFY(two);
        QCOMPARE(one->document()->session()->replace(3, 3, "-edit", one->document()->session()->revision()), EditResult::Applied); QCOMPARE(two->document()->session()->replace(3, 3, "-edit", two->document()->session()->revision()), EditResult::Applied);
        chooseClose(0); workspace->closePane(one->paneId()); QCOMPARE(workspace->panes().size(), 2); QCOMPARE(one->document()->session()->snapshot().utf8, QByteArray("one-edit")); QCOMPARE(two->document()->session()->snapshot().utf8, QByteArray("two-edit"));
        chooseClose(0); desktop.requestQuit(); QTRY_VERIFY(!workspace->closingWorkspace()); QCOMPARE(workspace->panes().size(), 2);
        QPointer<SingleFileWindow> guardedOne = one; chooseClose(1); workspace->closePane(one->paneId()); QTRY_VERIFY(guardedOne.isNull()); QCOMPARE(disk(first), QByteArray("one-edit")); QCOMPARE(two->document()->session()->snapshot().utf8, QByteArray("two-edit"));
        chooseClose(2); workspace->close(); QTRY_VERIFY(workspace.isNull()); QCOMPARE(disk(second), QByteArray("two"));
    }
};
int main(int argc, char **argv) { initializeGuiRuntime(); QApplication app(argc, argv); configureGuiApplication(); WorkspaceTests tests; return QTest::qExec(&tests, argc, argv); }
#include "workspace_tests.moc"
