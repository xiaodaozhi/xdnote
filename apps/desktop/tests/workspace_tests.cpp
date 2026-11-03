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
#include "WindowGeometry.h"
#include "DocumentBreadcrumb.h"
#include "NavigationStore.h"
#include "WorkspaceNavigation.h"
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
#include <QLineEdit>
#include <QListWidget>
#include <QSplitterHandle>
#include <QAction>
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
bool hasColorNear(const QImage &image, QPointF logical, const QColor &color) { const auto point = logical * image.devicePixelRatio(); for (int y = qMax(0, qRound(point.y()) - 2); y <= qMin(image.height() - 1, qRound(point.y()) + 2); ++y) for (int x = qMax(0, qRound(point.x()) - 2); x <= qMin(image.width() - 1, qRound(point.x()) + 2); ++x) if (image.pixelColor(x, y) == color) return true; return false; }
void chooseClose(int choice) { QTimer::singleShot(30, qApp, [choice] { for (auto *widget : QApplication::topLevelWidgets()) if (widget->objectName() == "unsavedConfirmation") { if (auto *button = widget->findChild<SharedButton *>(QStringLiteral("choice%1").arg(choice))) button->click(); } }); }
}
class WorkspaceTests final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qApp->setQuitOnLastWindowClosed(false); SourceEditor::installAccessibility(); }
    void breadcrumbUsesWorkspaceHierarchyAndUpdatesAfterSaveAs() {
        QTemporaryDir dir, outside; QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("个人笔记/写作")))); const auto path = dir.filePath(QStringLiteral("个人笔记/写作/开始.md")); QVERIFY(fileFixtures::write(path, "# writing"));
        FileApplication desktop; desktop.open(path); auto *pane = waitStandalone(path); QVERIFY(pane); QCOMPARE(pane->findChild<ElidedLabel *>("documentPath")->fullText(), QFileInfo(path).path()); QVERIFY(!pane->findChild<DocumentBreadcrumb *>("workspaceBreadcrumb")->isVisible());
        auto *workspace = desktop.workspaceWindow(dir.path()); QVERIFY(desktop.transferToWorkspace(pane, workspace)); auto *breadcrumb = pane->findChild<DocumentBreadcrumb *>("workspaceBreadcrumb"); QVERIFY(breadcrumb->isVisible()); QVERIFY(!pane->findChild<ElidedLabel *>("documentPath")->isVisible());
        QCOMPARE(breadcrumb->fullText(), QStringLiteral("文档 / 个人笔记 / 写作 / 开始.md")); QCOMPARE(breadcrumb->accessibleName(), breadcrumb->fullText()); QCOMPARE(breadcrumb->toolTip(), QDir::toNativeSeparators(path));
        QTRY_COMPARE(workspace->tree()->currentIndex().data().toString(), QStringLiteral("开始.md"));
        const auto renamed = dir.filePath(QStringLiteral("根目录.md")); QVERIFY(pane->document()->saveAs(renamed)); QTRY_VERIFY(!pane->document()->saving()); QTRY_COMPARE(breadcrumb->fullText(), QStringLiteral("文档 / 根目录.md"));
        const auto external = outside.filePath(QStringLiteral("外部.md")); QVERIFY(pane->document()->saveAs(external)); QTRY_VERIFY(!pane->document()->saving()); QTRY_COMPARE(breadcrumb->fullText(), QStringLiteral("工作区外 / 外部.md"));
        workspace->setRootPath({}); QCOMPARE(breadcrumb->fullText(), QStringLiteral("文档 / 外部.md")); QVERIFY(!workspace->tree()->currentIndex().isValid());
    }
    void navigationStoragePreservesUnknownFieldsAndRollsBackErrors() {
        QTemporaryDir dir; const auto path = dir.filePath("navigation.json"), first = dir.filePath("one.md"), second = dir.filePath("two.markdown");
        QVERIFY(fileFixtures::write(path, "{\"schemaVersion\":1,\"future\":{\"retained\":true}}")); NavigationStore store(nullptr, path); QVERIFY(store.recordOpen(first)); QVERIFY(store.recordOpen(second)); QVERIFY(store.recordOpen(first)); QCOMPARE(store.recent().size(), 2); QCOMPARE(store.recent().first().path, first);
        QVERIFY(store.setFavorite(first, true)); QVERIFY(store.setFavorite(first, true)); QCOMPARE(store.favorites().size(), 1); NavigationStore restored(nullptr, path); QCOMPARE(restored.favorites(), QStringList{first}); QCOMPARE(restored.recent().first().path, first); QVERIFY(QJsonDocument::fromJson(disk(path)).object().value("future").toObject().value("retained").toBool());
        for (int i = 0; i < 55; ++i) QVERIFY(store.recordOpen(dir.filePath(QStringLiteral("recent%1.md").arg(i)))); QCOMPARE(store.recent().size(), 50);
        const auto broken = dir.filePath("broken.json"); QVERIFY(fileFixtures::write(broken, "invalid-original")); NavigationStore invalid(nullptr, broken); QSignalSpy failed(&invalid, &NavigationStore::storageFailed); QVERIFY(!invalid.setFavorite(first, true)); QCOMPARE(failed.size(), 1); QVERIFY(invalid.favorites().isEmpty()); QCOMPARE(disk(broken), QByteArray("invalid-original"));
        QVERIFY(QDir().mkdir(dir.filePath("blocked"))); NavigationStore blocked(nullptr, dir.filePath("blocked")); QVERIFY(!blocked.recordOpen(first)); QVERIFY(blocked.recent().isEmpty()); QVERIFY(!blocked.error().isEmpty());
        const auto livePath = dir.filePath("live.json"); NavigationStore live(nullptr, livePath); QVERIFY(live.recordOpen(first)); QVERIFY(QFile::remove(livePath)); QVERIFY(QDir().mkdir(livePath)); QSignalSpy rejected(&live, &NavigationStore::storageFailed); QVERIFY(!live.setFavorite(first, true)); QCOMPARE(rejected.size(), 1); QVERIFY(!live.favorite(first)); QCOMPARE(live.recent().size(), 1);
        QVERIFY(store.setFavorite(first, false)); QVERIFY(!store.favorite(first));
    }
    void sidebarWriteFailureFeedbackClearsAfterSuccessfulRetry() {
        QTemporaryDir dir; const auto navigationPath = dir.filePath("navigation.json"), appearancePath = dir.filePath("appearance.json"), document = dir.filePath("document.md"); QVERIFY(fileFixtures::write(document, "# retained")); NavigationStore navigation(nullptr, navigationPath); QVERIFY(navigation.recordOpen(document)); Appearance appearance(nullptr, appearancePath); QVERIFY(appearance.setPreferences("light", "graphite")); WindowGeometryStore geometry;
        auto *workspace = new WorkspaceWindow(&appearance, &geometry, &navigation); workspace->show(); auto *notice = workspace->findChild<QLabel *>("workspaceNotice");
        QVERIFY(QFile::remove(navigationPath)); QVERIFY(QDir().mkdir(navigationPath)); QVERIFY(!navigation.setFavorite(document, true)); QVERIFY(notice->isVisible()); QVERIFY(notice->text().contains(QStringLiteral("无法保存导航记录"))); QVERIFY(!navigation.favorite(document)); QVERIFY(QDir().rmdir(navigationPath)); QVERIFY(navigation.setFavorite(document, true)); QVERIFY(!notice->isVisible());
        QVERIFY(QFile::remove(appearancePath)); QVERIFY(QDir().mkdir(appearancePath)); auto *settings = workspace->findChild<SharedButton *>("workspaceSettings"); auto *dark = settings->menu()->findChild<QAction *>("theme-dark"); dark->trigger(); QCOMPARE(appearance.theme(), QString("light")); QVERIFY(notice->isVisible()); QVERIFY(settings->menu()->findChild<QAction *>("theme-light")->isChecked()); QVERIFY(QDir().rmdir(appearancePath)); dark->trigger(); QCOMPARE(appearance.theme(), QString("dark")); QVERIFY(!notice->isVisible()); QCOMPARE(disk(document), QByteArray("# retained")); delete workspace;
    }
    void sidebarSearchRecentFavoritesAndTabsKeepTheSameBuffer() {
        QTemporaryDir dir; QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("笔记/子目录")))); const auto path = dir.filePath(QStringLiteral("笔记/子目录/中文会议.md")); QVERIFY(fileFixtures::write(path, "# unchanged")); QVERIFY(fileFixtures::write(dir.filePath("other.md"), "中文会议 appears only in content"));
        FileApplication desktop(nullptr, dir.filePath("user-preferences.json")); auto *workspace = desktop.workspaceWindow(dir.path()); QTRY_VERIFY(workspace->windowGeometry()->initialized()); workspace->windowGeometry()->rememberUserGeometry(QRect(workspace->geometry().topLeft(), QSize(960, 640))); workspace->windowGeometry()->fitToScreen(); desktop.openInWorkspace(path, workspace); auto *pane = waitPane(workspace, path); QVERIFY(pane); auto *sidebar = workspace->findChild<QWidget *>("workspaceSidebar"); QTRY_VERIFY(!sidebar->isVisible()); QTest::mouseClick(pane->findChild<SharedButton *>("fileLocation"), Qt::LeftButton); QTRY_VERIFY(sidebar->isVisible()); auto *session = pane->document()->session(); QCOMPARE(session->replace(0, 0, "dirty\n", session->revision()), EditResult::Applied);
        auto *search = workspace->findChild<QLineEdit *>("workspaceSearch"); auto *results = workspace->findChild<QListWidget *>("navigationResults"); QVERIFY(search); QVERIFY(results);
        QTest::mouseClick(workspace->findChild<SharedButton *>("workspaceRecent"), Qt::LeftButton); QTRY_COMPARE(results->count(), 2); QCOMPARE(results->item(0)->text(), QStringLiteral("今天")); QCOMPARE(results->item(1)->text(), QStringLiteral("中文会议.md")); QVERIFY(!pane->isVisible());
        QTest::mouseClick(results->viewport(), Qt::LeftButton, {}, QPoint(results->visualItemRect(results->item(1)).right() - 24, results->visualItemRect(results->item(1)).center().y()));
        QTest::mouseClick(workspace->findChild<SharedButton *>("workspaceFavorites"), Qt::LeftButton); QCOMPARE(results->count(), 1); QCOMPARE(results->item(0)->text(), QStringLiteral("中文会议.md")); capture(workspace, "favorites");
        search->setText(QStringLiteral("中文会议")); QTRY_COMPARE(results->count(), 1); QCOMPARE(results->item(0)->text(), QStringLiteral("中文会议.md")); QVERIFY(!workspace->findChild<QLabel *>("navigationSummary")->text().contains(QStringLiteral("不完整")));
        search->setText(QStringLiteral("无匹配文件名")); QTRY_VERIFY(workspace->findChild<QLabel *>("navigationEmpty")->isVisible()); QTRY_COMPARE(workspace->findChild<QLabel *>("navigationEmpty")->text(), QStringLiteral("没有找到匹配的文档")); QCOMPARE(results->count(), 0); capture(workspace, "search-empty");
        QTest::mouseClick(workspace->findChild<SharedButton *>("clearWorkspaceSearch"), Qt::LeftButton); QVERIFY(pane->isVisible()); QCOMPARE(pane->document()->session(), session); QCOMPARE(session->snapshot().utf8, QByteArray("dirty\n# unchanged"));
        QTest::mouseClick(workspace->findChild<SharedButton *>("workspaceRecent"), Qt::LeftButton); auto *tabs = workspace->findChild<QTabBar *>("workspaceTabs"); QTest::mouseClick(tabs, Qt::LeftButton, {}, tabs->tabRect(0).center()); QVERIFY(pane->isVisible());
        QTest::mouseClick(workspace->findChild<SharedButton *>("workspaceRecent"), Qt::LeftButton); QTest::mouseClick(results->viewport(), Qt::LeftButton, {}, QPoint(90, results->visualItemRect(results->item(1)).center().y())); QTRY_VERIFY(pane->isVisible()); QTRY_COMPARE(workspace->panes().size(), 1); QCOMPARE(pane->document()->session(), session);
        auto *settings = workspace->findChild<SharedButton *>("workspaceSettings"); QVERIFY(settings && settings->menu()); QCOMPARE(settings->menu()->findChild<QMenu *>("accentMenu")->actions().size(), 6); settings->menu()->findChild<QAction *>("theme-dark")->trigger(); QVERIFY(pane->appearance()->dark()); QTest::mouseClick(workspace->findChild<SharedButton *>("workspaceTheme"), Qt::LeftButton); QVERIFY(!pane->appearance()->dark());
        NavigationStore persisted(nullptr, dir.filePath("user-preferences.json.navigation")); QVERIFY(persisted.favorite(path)); QCOMPARE(persisted.recent().size(), 1); QCOMPARE(disk(path), QByteArray("# unchanged")); QVERIFY(!QFileInfo::exists(dir.filePath("assets")));
        workspace->raise(); workspace->activateWindow(); QTRY_VERIFY(workspace->isActiveWindow()); auto *selector = workspace->findChild<SharedButton *>("workspaceSelector"); selector->setFocus(Qt::OtherFocusReason); QTRY_VERIFY(selector->hasFocus()); QTest::keyClick(selector, Qt::Key_Tab); QTRY_VERIFY(search->hasFocus()); QVERIFY(workspace->findChild<QWidget *>("workspaceSearchBox")->property("keyboardOutline").toBool()); QTest::mouseClick(search, Qt::LeftButton); QVERIFY(!workspace->findChild<QWidget *>("workspaceSearchBox")->property("keyboardOutline").toBool());
        workspace->activatePane(pane->paneId()); pane->setViewMode(SingleFileWindow::ViewMode::Source); QTest::mouseClick(pane->findChild<SharedButton *>("fileLocation"), Qt::LeftButton); QVERIFY(!sidebar->isVisible()); QTest::keyClick(pane->editor(), Qt::Key_K, Qt::ControlModifier); QTRY_VERIFY(sidebar->isVisible()); QTRY_VERIFY(search->hasFocus()); QEvent activation(QEvent::WindowActivate); QCoreApplication::sendEvent(workspace, &activation); QVERIFY(search->hasFocus()); QTest::keyClick(search, Qt::Key_Escape); QVERIFY(pane->isVisible());
        QCOMPARE(session->undo(), EditResult::Applied);
    }
    void selectedFileRecoversFromStaleDirectorySnapshot() {
        QTemporaryDir dir; QVERIFY(QDir().mkpath(dir.filePath("notes"))); const auto path = dir.filePath("notes/current.md"); QVERIFY(fileFixtures::write(path, "retained")); FileApplication desktop; auto *workspace = desktop.workspaceWindow(dir.path()); auto *tree = workspace->tree(); tree->selectPath(path); QTRY_COMPARE(tree->currentIndex().data().toString(), QString("current.md")); auto *model = qobject_cast<QStandardItemModel *>(tree->model()); QVERIFY(model); const auto index = tree->currentIndex(); QVERIFY(model->removeRow(index.row(), index.parent())); QVERIFY(QFileInfo::exists(path)); tree->selectPath(path); QTRY_COMPARE(tree->currentIndex().data().toString(), QString("current.md"));
        tree->refresh(); QTRY_COMPARE(tree->currentIndex().data().toString(), QString("current.md")); QCOMPARE(disk(path), QByteArray("retained"));
    }
    void filenameSearchCancelsStaleRootsAndSkipsLinks() {
        QTemporaryDir dir, outside; QVERIFY(fileFixtures::write(dir.filePath("first.md"), "first")); QVERIFY(fileFixtures::write(outside.filePath("second.md"), "second"));
#ifdef Q_OS_WIN
        fileFixtures::Junction link; QVERIFY(link.create(dir.filePath("link"), outside.path()));
#else
        QVERIFY(::symlink(outside.path().toUtf8().constData(), dir.filePath("link").toUtf8().constData()) == 0);
#endif
        Appearance appearance; WorkspaceTree tree(&appearance, nullptr); tree.setRootPath(dir.path()); QSignalSpy results(&tree, &WorkspaceTree::searchFinished); tree.searchFiles("second"); QTRY_COMPARE(results.size(), 1); QVERIFY(results.takeFirst().at(1).toStringList().isEmpty());
        tree.searchFiles("first"); tree.setRootPath(outside.path()); tree.searchFiles("SECOND"); QTRY_COMPARE(results.size(), 1); QCOMPARE(results.first().at(1).toStringList(), QStringList{outside.filePath("second.md")});
        results.clear(); tree.setRootPath({}); tree.searchFiles("first"); QTRY_COMPARE(results.size(), 1); QVERIFY(!results.first().at(2).toString().isEmpty());
    }
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
    void directVisibleWindowDestructionFlushesControllersBeforeBaseTeardown() {
        FileManager manager; Appearance appearance; WindowGeometryStore store;
        auto *standalone = new SingleFileWindow(&manager, &appearance, &store); standalone->show(); QTRY_VERIFY(standalone->windowGeometry()->initialized()); QPointer<WindowGeometry> first = standalone->windowGeometry(); QVERIFY(first); delete standalone; QVERIFY(first.isNull()); QVERIFY(store.placement("standalone"));
        auto *workspace = new WorkspaceWindow(&appearance, &store); workspace->show(); QTRY_VERIFY(workspace->windowGeometry()->initialized()); auto *header = workspace->findChild<QWidget *>("documentHeader"); QVERIFY(hasColorNear(header->grab().toImage(), QPointF(500, 47), appearance.color("border")));
        auto *pane = new SingleFileWindow(&manager, &appearance, &store); QVERIFY(workspace->addPane(pane)); auto *welcome = pane->findChild<SharedButton *>("welcomeWorkspace"); QTRY_VERIFY(welcome->width() >= QFontMetricsF(welcome->font(), welcome).horizontalAdvance(welcome->text()) + 48); QSignalSpy chooseTab(workspace, &WorkspaceWindow::chooseFileRequested); auto *addTab = workspace->findChild<SharedButton *>("workspaceAddTab"); QTest::mouseClick(addTab, Qt::LeftButton); QCOMPARE(chooseTab.size(), 1); QTest::keyClick(addTab, Qt::Key_Tab); QVERIFY(InputModality::forApplication()->keyboardNavigation()); QPointer<WindowGeometry> second = workspace->windowGeometry(); QPointer<SingleFileWindow> child = pane; delete workspace; QVERIFY(second.isNull()); QVERIFY(child.isNull()); QVERIFY(store.placement("main"));
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
        qInfo() << "transfer-fixture: starting";
        QTemporaryDir dir; const auto path = dir.filePath("composition.md"); QVERIFY(fileFixtures::write(path, "base")); FileApplication desktop; desktop.open(path); auto *pane = waitStandalone(path); QVERIFY(pane); pane->setEditing(true);
        auto *target = desktop.workspaceWindow(); QInputMethodEvent preedit(QStringLiteral("pin"), {}); QApplication::sendEvent(pane->editor(), &preedit); QVERIFY(pane->document()->session()->isComposing()); QVERIFY(!desktop.transferToWorkspace(pane, target)); QVERIFY(pane->isWindow()); QVERIFY(target->panes().isEmpty());
        qInfo() << "transfer-fixture: preedit deferred";
        QInputMethodEvent commit; commit.setCommitString(QStringLiteral("中")); QApplication::sendEvent(pane->editor(), &commit); QTRY_VERIFY(pane->embedded()); QCOMPARE(pane->window(), static_cast<QWidget *>(target)); QVERIFY(pane->document()->session()->snapshot().utf8.contains(QStringLiteral("中").toUtf8()));
        qInfo() << "transfer-fixture: transferred";
        const auto otherPath = dir.filePath("preserve.md"); QVERIFY(fileFixtures::write(otherPath, "preserved")); desktop.open(otherPath); auto *other = waitStandalone(otherPath); QVERIFY(other);
        auto *closing = desktop.workspaceWindow(dir.path()); qInfo() << "transfer-fixture: closing destination"; closing->close(); qInfo() << "transfer-fixture: destination closed"; QVERIFY(closing->closingWorkspace()); QVERIFY(!desktop.transferToWorkspace(other, closing)); QVERIFY(other->isWindow()); QCOMPARE(other->document()->session()->snapshot().utf8, QByteArray("preserved"));
        qInfo() << "transfer-fixture: leaving scope";
    }
    void readingPositionSurvivesNativeHostTransfer() {
        QTemporaryDir dir; const auto path = dir.filePath("reader.md"); QByteArray text; for (int i = 0; i < 180; ++i) text += "paragraph\n\n"; QVERIFY(fileFixtures::write(path, text)); FileApplication desktop; desktop.open(path); auto *pane = waitStandalone(path); QVERIFY(pane);
        QTRY_VERIFY(pane->preview()->view()->page()->contentsSize().height() > 1000);
        pane->preview()->restoreReadingPosition({0, 500}); QTRY_VERIFY(pane->preview()->view()->page()->scrollPosition().y() >= 490);
        const auto position = pane->preview()->readingPosition(); auto *view = pane->preview()->view(); QVERIFY(desktop.transferToWorkspace(pane)); QCOMPARE(pane->preview()->view(), view); QTest::qWait(200); QTRY_VERIFY(qAbs(view->page()->scrollPosition().y() - position.y()) <= 2); capture(pane->window(), "transferred-reader"); QCOMPARE(disk(path), text);
    }
    void failedOpenTabsOverflowAndKeyboardCloseRemainUsable_data() { QTest::addColumn<QString>("theme"); QTest::newRow("light") << QString("light"); QTest::newRow("dark") << QString("dark"); }
    void failedOpenTabsOverflowAndKeyboardCloseRemainUsable() {
        QFETCH(QString, theme); QTemporaryDir dir; FileApplication desktop; auto *workspace = desktop.workspaceWindow(dir.path()); QVERIFY(workspace->windowAppearance()->setPreferences(theme, "graphite"));
        for (int i = 0; i < 12; ++i) desktop.openInWorkspace(dir.filePath(QStringLiteral("missing-document-with-a-long-name-%1.md").arg(i)), workspace);
        auto *tabs = workspace->findChild<QTabBar *>("workspaceTabs"); QTRY_COMPARE(tabs->count(), 12); QVERIFY(tabs->usesScrollButtons());
        auto *addTab = workspace->findChild<SharedButton *>("workspaceAddTab"); QVERIFY(addTab->isVisible()); QTRY_VERIFY(tabs->width() >= workspace->findChild<QWidget *>("workspaceTabStrip")->width() - addTab->width() - 12); QVERIFY(addTab->mapTo(workspace, addTab->rect().bottomRight()).x() < workspace->width()); QVERIFY(tabs->geometry().right() < addTab->geometry().left());
        if (auto *caption = workspace->findChild<SharedButton *>("captionMinimize")) QVERIFY(addTab->mapTo(workspace, addTab->rect().bottomRight()).x() < caption->mapTo(workspace, QPoint()).x()); capture(workspace, theme + "-tabs-overflow");
        auto *pane = workspace->activePane(); QVERIFY(pane); QPointer<SingleFileWindow> guard = pane; QTest::keyClick(tabs, Qt::Key_Delete); QTRY_VERIFY(guard.isNull()); QCOMPARE(tabs->count(), 11);
        const auto path = dir.filePath("valid.md"); QVERIFY(fileFixtures::write(path, "valid")); desktop.openInWorkspace(path, workspace); pane = waitPane(workspace, path); QVERIFY(pane);
        workspace->windowGeometry()->rememberUserGeometry(QRect(workspace->geometry().topLeft(), QSize(960, 640))); workspace->windowGeometry()->fitToScreen(); QTRY_COMPARE(workspace->size(), QSize(960, 640)); auto *sidebar = workspace->findChild<QWidget *>("workspaceSidebar"); QTRY_VERIFY(!sidebar->isVisible());
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
        QFETCH(QString, theme); QTemporaryDir dir; const auto root = dir.filePath(QStringLiteral("我的文档")); QVERIFY(QDir().mkpath(root + QStringLiteral("/个人笔记"))); QVERIFY(QDir().mkpath(root + QStringLiteral("/项目文档"))); QVERIFY(QDir().mkpath(root + QStringLiteral("/技术笔记")));
        const auto path = root + QStringLiteral("/个人笔记/写作，从这里开始.md"), other = root + QStringLiteral("/项目文档/产品设计思考.md");
        const auto content = QStringLiteral("# 写作，从这里开始\n\n把想法写下来，让思考慢慢清晰。\n\n> 少一点干扰，多一点专注。\n\n## 一个安静的写作空间\n\n- [x] 记录一个新的想法\n- [ ] 留一点时间，自由写作\n- [ ] 把灵感整理成一篇文档\n\n## 用熟悉的 Markdown 表达\n\n**清楚的文字**，加上一点 *恰到好处的强调*。\n\n```javascript\nconst title = \"写作，从这里开始\";\nconsole.log(title);\n```\n\n| 功能 | 说明 |\n| :--- | :--- |\n| 编辑 | 在左侧专注写作 |\n| 阅读 | 在右侧查看结果 |\n").toUtf8();
        QVERIFY(fileFixtures::write(path, content)); QVERIFY(fileFixtures::write(other, QStringLiteral("# 产品设计思考\n\n让写作保持简单。\n").toUtf8())); QVERIFY(fileFixtures::write(root + QStringLiteral("/个人笔记/灵感随笔.md"), "# Notes")); QVERIFY(fileFixtures::write(root + QStringLiteral("/个人笔记/阅读清单.md"), "# Reading"));
        FileApplication desktop; auto *workspace = desktop.workspaceWindow(root); desktop.openInWorkspace(other, workspace); QVERIFY(waitPane(workspace, other)); desktop.openInWorkspace(path, workspace); auto *pane = waitPane(workspace, path); QVERIFY(pane); QVERIFY(pane->appearance()->setPreferences(theme, "graphite")); auto *sidebar = workspace->findChild<QWidget *>("workspaceSidebar"); if (!sidebar->isVisible()) QTest::mouseClick(pane->findChild<SharedButton *>("fileLocation"), Qt::LeftButton); QTRY_VERIFY(sidebar->isVisible());
        auto *session = pane->document()->session(); const auto id = session->documentId(); pane->setViewMode(SingleFileWindow::ViewMode::Split); QTRY_VERIFY(pane->editor()->isVisible()); QTRY_VERIFY(pane->preview()->isVisible());
        const auto length = QString::fromUtf8(content).size(); const auto addition = QStringLiteral("\n写下此刻的想法。\n"); QCOMPARE(session->replace(int(length), int(length), addition, session->revision()), EditResult::Applied); QTRY_VERIFY(pane->document()->dirty()); QVERIFY(pane->document()->save()); QTRY_VERIFY(!pane->document()->saving()); QVERIFY(!pane->document()->dirty()); QCOMPARE(disk(path), content + addition.toUtf8());
        pane->setViewMode(SingleFileWindow::ViewMode::Reading); pane->setViewMode(SingleFileWindow::ViewMode::Source); pane->setViewMode(SingleFileWindow::ViewMode::Split); QCOMPARE(pane->document()->session()->documentId(), id); QCOMPARE(pane->document()->session(), session);
        QCOMPARE(workspace->findChild<QWidget *>("documentHeader")->height(), 48); QCOMPARE(workspace->findChild<QWidget *>("workspaceFooters")->height(), 26); QCOMPARE(pane->footer()->width(), workspace->width()); QVERIFY(!pane->windowGeometry()); QVERIFY(workspace->windowGeometry());
        QCOMPARE(pane->footer()->geometry(), workspace->findChild<QWidget *>("workspaceFooters")->rect());
        auto *splitter = pane->findChild<QSplitter *>("documentSplit"); auto *handle = splitter->handle(1); QVERIFY(handle->isVisible());
        QTRY_COMPARE(workspace->tree()->currentIndex().data().toString(), QStringLiteral("写作，从这里开始.md")); QTRY_VERIFY(pane->preview()->view()->page()->contentsSize().height() > 100); QTest::qWait(250);
        const auto treeImage = workspace->tree()->viewport()->grab().toImage(); const auto treeRatio = treeImage.devicePixelRatio(); const auto selectedRow = workspace->tree()->visualRect(workspace->tree()->currentIndex()); QCOMPARE(treeImage.pixelColor(qRound((workspace->tree()->viewport()->width() - 16) * treeRatio), qRound(selectedRow.center().y() * treeRatio)), pane->appearance()->accentColor("soft"));
        const auto divider = handle->grab().toImage(); const auto ratio = divider.devicePixelRatio(); QCOMPARE(divider.pixelColor(int((handle->width() / 2.0) * ratio), qRound(60 * ratio)), pane->appearance()->color("border")); QCOMPARE(divider.pixelColor(0, qRound(60 * ratio)), pane->appearance()->color("background"));
        const auto original = splitter->sizes(); QTest::mousePress(handle, Qt::LeftButton); QTest::mouseMove(handle, handle->rect().center() + QPoint(40, 0), 30); QTest::mouseRelease(handle, Qt::LeftButton); QVERIFY(splitter->sizes().first() != original.first()); splitter->setSizes(original);
        auto *tabs = workspace->findChild<QTabBar *>("workspaceTabs"); const auto tabImage = tabs->grab().toImage(); const auto active = tabs->tabRect(tabs->currentIndex()), inactive = tabs->tabRect(1 - tabs->currentIndex()); const auto border = pane->appearance()->color("border"), background = pane->appearance()->color("background");
        QVERIFY(hasColorNear(tabImage, QPointF(active.center().x(), 6), border)); QVERIFY(hasColorNear(tabImage, QPointF(active.left() + 2.5, 24), border)); QVERIFY(hasColorNear(tabImage, QPointF(active.right() - 2.5, 24), border)); QVERIFY(hasColorNear(tabImage, QPointF(active.center().x(), tabs->height() - 1), background)); QVERIFY(!hasColorNear(tabImage, QPointF(active.center().x(), tabs->height() - 1), border));
        QVERIFY(hasColorNear(tabImage, QPointF(inactive.center().x(), tabs->height() - 1), border)); auto *header = workspace->findChild<QWidget *>("documentHeader"); auto *tabStrip = workspace->findChild<QWidget *>("workspaceTabStrip"); const auto headerImage = header->grab().toImage(); QVERIFY(hasColorNear(headerImage, QPointF(tabStrip->geometry().right() - 12, header->height() - 1), border));
        auto *edge = workspace->findChild<QWidget *>("workspaceSidebarBorder"); QVERIFY(edge); QCOMPARE(edge->height(), workspace->findChild<QWidget *>("workspaceSidebar")->height()); const auto edgeImage = edge->grab().toImage(); for (const int y : {2, edge->height() / 2, edge->height() - 3}) QVERIFY(hasColorNear(edgeImage, QPointF(0, y), border));
        capture(workspace, theme + "-split");
        workspace->findChild<QAction *>("favoriteDocument")->trigger(); QTest::mouseClick(workspace->findChild<SharedButton *>("workspaceRecent"), Qt::LeftButton); capture(workspace, theme + "-recent"); QTest::mouseClick(workspace->findChild<SharedButton *>("workspaceFavorites"), Qt::LeftButton); capture(workspace, theme + "-favorites");
        auto *search = workspace->findChild<QLineEdit *>("workspaceSearch"); search->setText(QStringLiteral("不存在的文件名")); QTRY_COMPARE(workspace->findChild<QLabel *>("navigationEmpty")->text(), QStringLiteral("没有找到匹配的文档")); capture(workspace, theme + "-search-empty"); search->clear(); QVERIFY(pane->isVisible()); QCOMPARE(pane->document()->session(), session);
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
