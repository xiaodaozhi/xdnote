#include "FileApplication.h"
#include "WorkspaceWindow.h"
#include "WorkspaceTree.h"
#include "WorkspaceReferenceController.h"
#include "ResourceMaintenanceController.h"
#include "ResourceMaintenance.h"
#include "MarkdownHeadings.h"
#include "SingleFileWindow.h"
#include "SourceEditor.h"
#include "MarkdownPreview.h"
#include "FileService.h"
#include "Appearance.h"
#include "Preferences.h"
#include "SharedWidgets.h"
#include "GuiRuntime.h"
#include "InputModality.h"
#include "WindowGeometry.h"
#include "VisualTokens.h"
#include "DocumentBreadcrumb.h"
#include "NavigationStore.h"
#include "WorkspaceNavigation.h"
#include "WorkspaceWelcome.h"
#include "file_fixtures.h"
#include <QApplication>
#include <QScreen>
#include <QEnterEvent>
#include <QMoveEvent>
#include <QThreadPool>
#include <QSemaphore>
#include <QtConcurrentRun>
#include <QAction>
#include <QBuffer>
#include <QCryptographicHash>
#include <QClipboard>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEventLoop>
#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QTabBar>
#include <QStackedWidget>
#include <QInputMethodEvent>
#include <QDialog>
#include <QFileDialog>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QScopedPointer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QSplitterHandle>
#include <QScrollArea>
#include <QScrollBar>
#include <QAction>
#include <QtMath>
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
void capture(QWidget *window, const QString &name) {
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < 15000) {
        bool pending = false;
        for (auto *preview : window->findChildren<MarkdownPreview *>()) if (preview->isVisible() && !preview->view()->ready()) pending = true;
        if (!pending) break;
        QTest::qWait(2);
    }
    for (auto *preview : window->findChildren<MarkdownPreview *>())
        if (preview->isVisible() && !preview->view()->ready()) { QTest::qFail("Cannot capture a pending native preview", __FILE__, __LINE__); return; }
    const auto directory = qEnvironmentVariable("XDNOTE_TEST_EVIDENCE");
    if (directory.isEmpty()) return;
    const QFontMetrics metrics(window->font());
    if (!metrics.inFont(QChar('M')) || !metrics.inFont(QChar(0x5199))) {
        QTest::qFail("Visual capture needs real Latin and Chinese glyphs; configure the test font directory first", __FILE__, __LINE__); return;
    }
    QDir().mkpath(directory + "/workspace");
    if (!window->grab().save(directory + "/workspace/" + name + ".png")) QTest::qFail("Visual capture could not be saved", __FILE__, __LINE__);
}
bool hasColorNear(const QImage &image, QPointF logical, const QColor &color) { const auto point = logical * image.devicePixelRatio(); for (int y = qMax(0, qRound(point.y()) - 2); y <= qMin(image.height() - 1, qRound(point.y()) + 2); ++y) for (int x = qMax(0, qRound(point.x()) - 2); x <= qMin(image.width() - 1, qRound(point.x()) + 2); ++x) if (image.pixelColor(x, y) == color) return true; return false; }
bool hasRuleAt(const QImage &image, QPoint logical, const QColor &color) {
    const qreal ratio = image.devicePixelRatio();
    const int x = qFloor((logical.x() + 0.5) * ratio), y = qFloor((logical.y() + 0.5) * ratio);
    if (x < 0 || x >= image.width()) return false;
    for (int row = qMax(0, y - 1); row <= qMin(image.height() - 1, y + 1); ++row) if (image.pixelColor(x, row) == color) return true;
    return false;
}
void chooseClose(int choice) { QTimer::singleShot(30, qApp, [choice] { for (auto *widget : QApplication::topLevelWidgets()) if (widget->objectName() == "unsavedConfirmation") { if (auto *button = widget->findChild<SharedButton *>(QStringLiteral("choice%1").arg(choice))) button->click(); } }); }
int loadedImages(MarkdownPreview *preview) { return preview->view()->admittedImageCount(); }
class InitializationWarning final : public QObject {
public:
    bool injected = false;
protected:
    bool eventFilter(QObject *target, QEvent *event) override {
        if (injected || event->type() != QEvent::ChildAdded) return false;
        auto *widget = qobject_cast<QWidget *>(target);
        auto *pane = widget ? qobject_cast<SingleFileWindow *>(widget->window()) : nullptr;
        if (!pane || !pane->document() || pane->preview() || !pane->windowGeometry()) return false;
        injected = true;
        pane->windowGeometry()->geometryWarning(QStringLiteral("初始化期间的窗口提示"));
        return false;
    }
};
}
class WorkspaceTests final : public QObject {
    Q_OBJECT
private slots:
    void welcomeFollowsApprovedComposition_data() {
        QTest::addColumn<QString>("theme"); QTest::addColumn<bool>("compact");
        QTest::newRow("light") << QString("light") << false;
        QTest::newRow("dark") << QString("dark") << false;
        QTest::newRow("compact-light") << QString("light") << true;
        QTest::newRow("compact-dark") << QString("dark") << true;
    }
    void welcomeFollowsApprovedComposition() {
        QFETCH(QString, theme); QFETCH(bool, compact); QTemporaryDir dir;
        FileApplication desktop(nullptr, dir.filePath("settings.json")); auto *workspace = desktop.workspaceWindow();
        QVERIFY(workspace->windowAppearance()->setPreferences(theme, "graphite")); QTRY_VERIFY(workspace->windowGeometry()->initialized());
        workspace->resize(compact ? 960 : 1280, compact ? 640 : 820); QApplication::processEvents();
        auto *welcome = workspace->findChild<WorkspaceWelcome *>(); QVERIFY(welcome && welcome->isVisible());
        auto *panel = welcome->findChild<QWidget *>("welcomeContent"); auto *brand = welcome->findChild<QLabel *>("welcomeBrandMark");
        auto *title = welcome->findChild<QLabel *>("welcomeHeading"); auto *description = welcome->findChild<QLabel *>("welcomeDescription");
        auto *open = welcome->findChild<SharedButton *>("workspaceOpenFile"); auto *folder = welcome->findChild<SharedButton *>("workspaceOpenDirectory"); auto *create = welcome->findChild<SharedButton *>("workspaceNewDocument");
        auto *empty = welcome->findChild<QLabel *>("welcomeEmptyRecent"); auto *scroll = welcome->findChild<QScrollArea *>();
        QVERIFY(panel && brand && title && description && open && folder && create && empty && scroll); QVERIFY(!brand->pixmap().isNull());
        QCOMPARE(title->text(), QStringLiteral("欢迎使用 xdnote")); QCOMPARE(description->text(), QStringLiteral("打开一个文件，开始阅读与写作。"));
        QCOMPARE(title->font().pixelSize(), 24); QCOMPARE(description->font().pixelSize(), 16); QCOMPARE(open->text(), QStringLiteral("打开文件")); QCOMPARE(folder->text(), QStringLiteral("打开文件夹"));
        const auto centerX = [panel](QWidget *widget) { return widget->mapTo(panel, widget->rect().center()).x(); };
        for (auto *button : {open, folder, create}) { QCOMPARE(button->height(), 32); QCOMPARE(button->width(), 192); QVERIFY(qAbs(centerX(button) - panel->rect().center().x()) <= 1); }
        QCOMPARE(open->geometry().left(), folder->geometry().left()); QCOMPARE(open->geometry().right(), folder->geometry().right());
        QCOMPARE(open->geometry().left(), create->geometry().left()); QCOMPARE(open->geometry().right(), create->geometry().right());
        QVERIFY(qAbs(centerX(brand) - centerX(open)) <= 1); QVERIFY(brand->geometry().bottom() < title->geometry().top());
        QVERIFY(title->geometry().bottom() < description->geometry().top()); QVERIFY(description->geometry().bottom() < open->geometry().top());
        QCOMPARE(folder->geometry().top() - open->geometry().bottom() - 1, 8); QCOMPARE(create->geometry().top() - folder->geometry().bottom() - 1, 8);
        QVERIFY(panel->width() <= 480); QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0); QVERIFY(empty->isVisible()); QCOMPARE(empty->text(), QStringLiteral("暂无最近文件"));
        capture(workspace, "welcome-" + theme + (compact ? "-compact" : "-empty"));
        auto *navigation = desktop.findChild<NavigationStore *>(); QVERIFY(navigation);
        for (const auto &name : {QStringLiteral("更早的文档.md"), QStringLiteral("Markdown 使用手册.md"), QStringLiteral("写作，从这里开始.md"), QStringLiteral("产品计划.md")}) {
            const auto path = dir.filePath(name); QVERIFY(fileFixtures::write(path, "# document")); QVERIFY(navigation->recordOpen(path));
        }
        auto *recent = welcome->findChild<QListWidget *>("welcomeRecentResults"); QVERIFY(recent); QTRY_COMPARE(recent->count(), 3); QVERIFY(recent->isVisible()); QVERIFY(!empty->isVisible());
        QTRY_VERIFY(recent->parentWidget()->height() >= 3 * 64); QTRY_VERIFY(recent->viewport()->height() >= 3 * 64);
        QTRY_VERIFY(recent->mapTo(panel, recent->rect().bottomRight()).y() <= panel->rect().bottom());
        QCOMPARE(recent->item(0)->data(Qt::UserRole + 1).toString(), QStringLiteral("文档"));
        QCOMPARE(recent->item(0)->text(), QStringLiteral("产品计划.md")); capture(workspace, "welcome-" + theme + (compact ? "-compact-recent" : "-recent"));
        QTest::mouseClick(welcome->findChild<SharedButton *>("welcomeAllRecent"), Qt::LeftButton); QVERIFY(!welcome->isVisible());
        auto *all = workspace->findChild<QListWidget *>("navigationResults"); QVERIFY(all && all->count() >= 4); QCOMPARE(workspace->panes().size(), 0);
    }
    void welcomeNewDocumentUsesStandaloneAndFirstSaveProtection() {
        QTemporaryDir dir; FileApplication desktop(nullptr, dir.filePath("settings.json")); auto *workspace = desktop.workspaceWindow(dir.path());
        auto *create = workspace->findChild<SharedButton *>("workspaceNewDocument"); QVERIFY(create); QTest::mouseClick(create, Qt::LeftButton);
        const auto untitled = []()->SingleFileWindow * { for (auto *widget : QApplication::topLevelWidgets()) if (auto *pane = qobject_cast<SingleFileWindow *>(widget)) if (pane->document() && pane->document()->untitled() && !pane->document()->recoveredDraft()) return pane; return nullptr; };
        QTRY_VERIFY(untitled()); auto *pane = untitled(); auto *document = pane->document(); auto *session = document->session();
        QVERIFY(!pane->embedded()); QVERIFY(workspace->panes().isEmpty()); QCOMPARE(pane->displayPath(), QStringLiteral("未命名.md"));
        QCOMPARE(pane->viewMode(), SingleFileWindow::ViewMode::Source); QVERIFY(!document->dirty()); QCOMPARE(document->statusText(), QStringLiteral("未命名"));
        QVERIFY(pane->findChild<QAction *>("saveAction")->isEnabled()); QVERIFY(session->snapshot().utf8.isEmpty());
        QCOMPARE(session->replace(0, 0, QStringLiteral("新想法 😀"), session->revision()), EditResult::Applied); QVERIFY(document->dirty());
        QCOMPARE(session->undo(), EditResult::Applied); QVERIFY(!document->dirty()); QCOMPARE(session->redo(), EditResult::Applied);
        QTimer::singleShot(0, pane, [] { if (auto *dialog = QApplication::activeModalWidget()) { if (auto *cancel = dialog->findChild<QPushButton *>("choice0")) cancel->click(); else qobject_cast<QDialog *>(dialog)->reject(); } });
        pane->close(); QVERIFY(pane->isVisible()); QVERIFY(document->dirty());
        const auto path = dir.filePath(QStringLiteral("新笔记.md")); QVERIFY(document->saveAs(path)); QTRY_VERIFY(!document->saving());
        QVERIFY(!document->untitled()); QVERIFY(!document->dirty()); QCOMPARE(document->session(), session); QCOMPARE(disk(path), QStringLiteral("新想法 😀").toUtf8());
        QCOMPARE(pane->displayPath(), path); QCOMPARE(workspace->panes().size(), 0);
    }
    void filePickerRoutesMenusToStandaloneAndPlusOnlyToInternalTabs_data() {
        QTest::addColumn<int>("entry"); QTest::addColumn<bool>("inside");
        for (int entry = 0; entry < 3; ++entry) for (bool inside : {false, true})
            QTest::newRow(qPrintable(QString("%1-%2").arg(entry).arg(inside))) << entry << inside;
    }
    void filePickerRoutesMenusToStandaloneAndPlusOnlyToInternalTabs() {
        QFETCH(int, entry); QFETCH(bool, inside);
        const bool native = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        const auto restore = qScopeGuard([&] { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, native); });
        QTemporaryDir dir, outside; const auto base = dir.filePath("base.md");
        const auto path = inside ? dir.filePath("chosen.md") : outside.filePath("chosen.md");
        QVERIFY(fileFixtures::write(base, "base")); QVERIFY(fileFixtures::write(path, "chosen"));
        FileApplication desktop(nullptr, dir.filePath("settings.json")); auto *workspace = desktop.workspaceWindow(dir.path());
        SingleFileWindow *source = nullptr;
        if (entry == 1) { desktop.openInWorkspace(base, workspace); source = waitPane(workspace, base); QVERIFY(source); }
        // A controlled Qt dialog, never the operating system's file picker.
        QStringList selected; QTimer select; select.setInterval(20);
        connect(&select, &QTimer::timeout, &select, [&] {
            for (auto *widget : QApplication::topLevelWidgets()) if (auto *dialog = qobject_cast<QFileDialog *>(widget); dialog && dialog->isVisible()) {
                select.stop();
                // As with the document-link fixture, clear the filename edit's
                // focus before Qt 6.8.2 selectFile replaces its current text.
                if (auto *focused = dialog->focusWidget()) focused->clearFocus();
                dialog->selectFile(path); selected = dialog->selectedFiles();
                if (selected == QStringList{path}) QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection); else dialog->reject();
                return;
            }
        });
        QTimer::singleShot(3000, &select, [] { if (auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) dialog->reject(); });
        select.start();
        if (entry == 0) workspace->findChild<SharedButton *>("workspaceOpenFile")->click();
        else if (entry == 1) source->findChild<QAction *>("openAction")->trigger();
        else workspace->findChild<SharedButton *>("workspaceAddTab")->click();
        select.stop(); QCOMPARE(selected, QStringList{path});
        if (entry == 2 && inside) QVERIFY(waitPane(workspace, path)); else QVERIFY(waitStandalone(path));
        QCOMPARE(workspace->panes().size(), (entry == 1 ? 1 : 0) + (entry == 2 && inside ? 1 : 0));
        QCOMPARE(desktop.findChild<FileManager *>()->documentCount(), entry == 1 ? 2 : 1);
    }
    void outsideNavigationAndDropsReuseStandaloneDirtySession() {
        QTemporaryDir dir, outside; const auto a = dir.filePath("inside.md"), b = outside.filePath("outside.md");
        QVERIFY(fileFixtures::write(a, "inside")); QVERIFY(fileFixtures::write(b, "outside"));
        FileApplication desktop(nullptr, dir.filePath("settings.json")); auto *workspace = desktop.workspaceWindow(dir.path());
        desktop.openInWorkspace(a, workspace); auto *source = waitPane(workspace, a); QVERIFY(source);
        workspace->openRequested(b); auto *pane = waitStandalone(b); QVERIFY(pane); auto *session = pane->document()->session();
        QCOMPARE(session->replace(0, 0, "dirty ", session->revision()), EditResult::Applied);
        QVERIFY(fileFixtures::write(b, "disk changed")); source->openRequested(b); desktop.openInWorkspace(b, workspace);
        auto *manager = desktop.findChild<FileManager *>(); QTRY_VERIFY(manager->idle());
        QCOMPARE(manager->documentCount(), 2); QCOMPARE(workspace->panes().size(), 1);
        QCOMPARE(waitStandalone(b), pane); QCOMPARE(session->snapshot().utf8, QByteArray("dirty outside"));
        QCOMPARE(session->undo(), EditResult::Applied); QCOMPARE(session->snapshot().utf8, QByteArray("outside"));
    }
    void externalPreferenceRequiresAnAlreadySelectedContainingWorkspace() {
        QTemporaryDir dir, outside; const auto a = dir.filePath("inside.md"), b = outside.filePath("outside.md"), c = outside.filePath("no-root.md");
        QVERIFY(fileFixtures::write(a, "inside")); QVERIFY(fileFixtures::write(b, "outside")); QVERIFY(fileFixtures::write(c, "rootless"));
        FileApplication desktop(nullptr, dir.filePath("settings.json")); auto *workspace = desktop.workspaceWindow(dir.path());
        QVERIFY(workspace->windowAppearance()->preferences()->setValues("desktop", {{"externalOpenTarget", "workspace-tab"}}));
        desktop.openExternal(a); QVERIFY(waitPane(workspace, a)); desktop.openExternal(b); auto *pane = waitStandalone(b); QVERIFY(pane);
        QVERIFY(!desktop.transferToWorkspace(pane, workspace)); QVERIFY(!workspace->addPane(pane)); QVERIFY(pane->isWindow());
        auto *welcome = desktop.workspaceWindow(); desktop.openInWorkspace(c, welcome); QVERIFY(waitStandalone(c));
        QVERIFY(welcome->panes().isEmpty()); QCOMPARE(workspace->panes().size(), 1); QVERIFY(welcome->rootPath().isEmpty());
        QCOMPARE(workspace->windowAppearance()->preferences()->value("desktop", "externalOpenTarget").toString(), QString("workspace-tab"));
    }
    void pendingOpenRechecksMembershipAfterRootChanges() {
        QTemporaryDir dir, replacement; const auto path = dir.filePath("pending.md"); QVERIFY(fileFixtures::write(path, "pending"));
        FileApplication desktop(nullptr, dir.filePath("settings.json")); auto *workspace = desktop.workspaceWindow(dir.path());
        auto *pool = QThreadPool::globalInstance(); const int previous = pool->maxThreadCount(); pool->setMaxThreadCount(1);
        QSemaphore entered, release; auto worker = QtConcurrent::run([&] { entered.release(); release.acquire(); }); bool released = false;
        const auto restore = qScopeGuard([&] { if (!released) release.release(); worker.waitForFinished(); pool->setMaxThreadCount(previous); });
        QVERIFY(entered.tryAcquire(1, 5000)); desktop.openInWorkspace(path, workspace); workspace->setRootPath(replacement.path());
        release.release(); released = true; worker.waitForFinished(); QVERIFY(waitStandalone(path)); QVERIFY(workspace->panes().isEmpty());
        QCOMPARE(workspace->findChild<QTabBar *>("workspaceTabs")->count(), 0);
    }
    void pendingTransferRechecksChangedDestinationWithoutMovingBuffer() {
        QTemporaryDir dir, replacement; const auto path = dir.filePath("pending.md"); QVERIFY(fileFixtures::write(path, "base"));
        FileApplication desktop(nullptr, dir.filePath("settings.json")); desktop.open(path); auto *pane = waitStandalone(path); QVERIFY(pane); pane->setEditing(true);
        auto *workspace = desktop.workspaceWindow(dir.path()); QInputMethodEvent preedit(QStringLiteral("pin"), {}); QApplication::sendEvent(pane->editor(), &preedit);
        QVERIFY(!desktop.transferToWorkspace(pane, workspace)); workspace->setRootPath(replacement.path());
        QInputMethodEvent commit; commit.setCommitString(QStringLiteral("中")); QApplication::sendEvent(pane->editor(), &commit);
        QTRY_VERIFY(!pane->document()->session()->isComposing()); QTest::qWait(50); QVERIFY(pane->isWindow()); QVERIFY(workspace->panes().isEmpty());
        QVERIFY(pane->document()->session()->snapshot().utf8.contains(QStringLiteral("中").toUtf8())); QCOMPARE(disk(path), QByteArray("base"));
    }
    void workspaceMembershipRejectsSiblingPrefixesAndLinkedParents() {
        QTemporaryDir dir; const auto root = dir.filePath("notes"), outside = dir.filePath("notes-other");
        QVERIFY(QDir().mkdir(root)); QVERIFY(QDir().mkdir(outside)); QVERIFY(fileFixtures::write(outside + "/outside.md", "outside"));
#ifdef Q_OS_WIN
        fileFixtures::Junction link; QVERIFY(link.create(root + "/link", outside));
#else
        QVERIFY(::symlink(outside.toUtf8().constData(), (root + "/link").toUtf8().constData()) == 0);
#endif
        Appearance appearance; WorkspaceTree tree(&appearance, nullptr); tree.setRootPath(root);
        QVERIFY(tree.containsPath(root + "/missing.md")); QVERIFY(!tree.containsPath(root));
        QVERIFY(!tree.containsPath(outside + "/outside.md")); QVERIFY(!tree.containsPath(root + "/../notes-other/outside.md"));
        QVERIFY(!tree.containsPath(root + "/link/outside.md")); tree.setRootPath({}); QVERIFY(!tree.containsPath(root + "/missing.md"));
    }
    void resourceMoveRewritesOnlyParsedTargetsAndRecovers() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); QVERIFY(QDir().mkpath(dir.filePath("old"))); QVERIFY(QDir().mkpath(dir.filePath("nested")));
        const QByteArray a = "# A\r\n[B](../b.md \"keep title\")\r\n`[B](../b.md)`\r\n";
        const QByteArray b = "[first][r] and [second][r]\r\n\r\n[r]: <old/a.md#A> \"unchanged\"\r\n\r\n![alt](old/image.png \"image title\")\r\n```md\r\n[x](old/a.md)\r\n```\r\n";
        QImage image(2, 2, QImage::Format_ARGB32); image.fill(Qt::green); QVERIFY(image.save(dir.filePath("old/image.png"), "PNG"));
        QVERIFY(fileFixtures::write(dir.filePath("old/a.md"), a)); QVERIFY(fileFixtures::write(dir.filePath("b.md"), QByteArray::fromHex("efbbbf") + b));
        auto root = WorkspaceResourceReader::open(dir.path()).reader; QVERIFY(root);
        ReferenceIndexInput input{QUuid::createUuid(), 1, 1, false, {}, {}};
        const auto preview = prepareResourceMove(root, input, {"old", "nested/moved", {}, true});
        QVERIFY2(preview.plan.valid(), qPrintable(preview.plan.problem)); QCOMPARE(preview.plan.documents.size(), 2);
        const auto result = executeResourceMove(root, preview, true);
        QVERIFY2(result.move.status == WorkspaceMoveStatus::Moved, qPrintable(result.problem));
        QCOMPARE(result.rows.size(), 2); for (const auto &row : result.rows) QVERIFY2(row.status == ResourceMaintenanceRow::Saved, qPrintable(row.message));
        QCOMPARE(disk(dir.filePath("nested/moved/a.md")), QByteArray("# A\r\n[B](../../b.md \"keep title\")\r\n`[B](../b.md)`\r\n"));
        auto expected = b; expected.replace("<old/a.md#A>", "<nested/moved/a.md#A>");
        expected.replace("(old/image.png", "(nested/moved/image.png");
        QCOMPARE(disk(dir.filePath("b.md")), QByteArray::fromHex("efbbbf") + expected);
        QVERIFY(!QFileInfo::exists(dir.filePath("old")));
        const auto reverse = reverseResourceMove(preview, result, result.movedIdentity);
        const auto recovered = executeResourceMove(root, reverse, true);
        QVERIFY2(recovered.move.status == WorkspaceMoveStatus::Moved, qPrintable(recovered.problem));
        for (const auto &row : recovered.rows) QVERIFY2(row.status == ResourceMaintenanceRow::Saved, qPrintable(row.message));
        QCOMPARE(disk(dir.filePath("old/a.md")), a); QCOMPARE(disk(dir.filePath("b.md")), QByteArray::fromHex("efbbbf") + b);
    }
    void resourceMoveRejectsStaleSourcesCollisionAndHardLinkAlias() {
        QTemporaryDir dir; QVERIFY(fileFixtures::write(dir.filePath("a.md"), "# a\n")); QVERIFY(fileFixtures::write(dir.filePath("b.md"), "[a](a.md)\n"));
        auto root = WorkspaceResourceReader::open(dir.path()).reader; QVERIFY(root); ReferenceIndexInput input{QUuid::createUuid(), 1, 1, false, {}, {}};
        const auto preview = prepareResourceMove(root, input, {"a.md", "new.md", {}, false}); QVERIFY(preview.plan.valid());
        QVERIFY(fileFixtures::write(dir.filePath("b.md"), "later [a](a.md)\n"));
        const auto stale = executeResourceMove(root, preview, true); QVERIFY(stale.move.status != WorkspaceMoveStatus::Moved); QVERIFY(!stale.problem.isEmpty());
        QVERIFY(QFileInfo::exists(dir.filePath("a.md"))); QVERIFY(!QFileInfo::exists(dir.filePath("new.md")));
        const auto collision = root->moveEntry("a.md", "b.md", root->inspect("a.md").identity, false); QVERIFY(collision.status == WorkspaceMoveStatus::Failed);
        QCOMPARE(disk(dir.filePath("b.md")), QByteArray("later [a](a.md)\n"));
        QVERIFY(fileFixtures::hardLink(dir.filePath("a.md"), dir.filePath("alias.md")));
        QVERIFY(root->moveEntry("a.md", "alias.md", root->inspect("a.md").identity, false).status == WorkspaceMoveStatus::Failed);
        QVERIFY(root->moveEntry("a.md", "A.md", root->inspect("a.md").identity, false).status == WorkspaceMoveStatus::Moved);
        QCOMPARE(disk(dir.filePath("alias.md")), QByteArray("# a\n"));
        root->revoke(); QVERIFY(root->moveEntry("A.md", "another.md", root->inspect("A.md").identity, false).status != WorkspaceMoveStatus::Moved);
    }
    void resourceMoveReportsPartialWritesAndPreservesLaterEditsOnRecovery() {
        QTemporaryDir dir; QVERIFY(fileFixtures::write(dir.filePath("a.md"), "a")); QVERIFY(fileFixtures::write(dir.filePath("b.md"), "[a](a.md)"));
        QVERIFY(fileFixtures::hardLink(dir.filePath("b.md"), dir.filePath("alias.txt")));
        auto root = WorkspaceResourceReader::open(dir.path()).reader; QVERIFY(root); ReferenceIndexInput input{QUuid::createUuid(), 1, 1, false, {}, {}};
        const auto preview = prepareResourceMove(root, input, {"a.md", "new.md", {}, false}); QVERIFY(preview.plan.valid());
        const auto result = executeResourceMove(root, preview, true); QVERIFY(result.move.status == WorkspaceMoveStatus::Moved);
        QVERIFY(std::any_of(result.rows.cbegin(), result.rows.cend(), [](const auto &row) { return row.from == "b.md" && row.status == ResourceMaintenanceRow::Failed; })); QCOMPARE(disk(dir.filePath("b.md")), QByteArray("[a](a.md)"));
        QVERIFY(QFileInfo::exists(dir.filePath("new.md"))); QCOMPARE(disk(dir.filePath("alias.txt")), QByteArray("[a](a.md)"));
        QVERIFY(QFile::remove(dir.filePath("alias.txt")));
        const auto next = prepareResourceMove(root, input, {"new.md", "a.md", {}, false}); QVERIFY(next.plan.valid());
        const auto restored = executeResourceMove(root, next, false); QVERIFY(restored.move.status == WorkspaceMoveStatus::Moved);
        const auto valid = prepareResourceMove(root, input, {"a.md", "new.md", {}, false}); const auto success = executeResourceMove(root, valid, true);
        QVERIFY(success.move.status == WorkspaceMoveStatus::Moved); QVERIFY(std::any_of(success.rows.cbegin(), success.rows.cend(), [](const auto &row) { return row.from == "b.md" && row.status == ResourceMaintenanceRow::Saved; }));
        QVERIFY(fileFixtures::write(dir.filePath("b.md"), "user later content"));
        const auto reverse = reverseResourceMove(valid, success, success.movedIdentity);
        const auto blocked = executeResourceMove(root, reverse, true); QVERIFY(blocked.move.status != WorkspaceMoveStatus::Moved);
        QCOMPARE(disk(dir.filePath("b.md")), QByteArray("user later content")); QVERIFY(QFileInfo::exists(dir.filePath("new.md")));
    }
    void resourceMoveKeepsDirtySessionUndoAndUniqueOpen() {
        QTemporaryDir dir; QVERIFY(fileFixtures::write(dir.filePath("a.md"), "# a\n")); QVERIFY(fileFixtures::write(dir.filePath("b.md"), "[a](a.md)\n"));
        FileApplication app(nullptr, dir.filePath("settings.ini")); auto *workspace = app.workspaceWindow(dir.path()); app.openInWorkspace(dir.filePath("b.md"), workspace);
        auto *pane = waitPane(workspace, dir.filePath("b.md")); QVERIFY(pane); pane->setEditing(true);
        auto *session = pane->document()->session(); auto before = session->snapshot(); QVERIFY(session->replace(0, 0, QStringLiteral("unsaved\n"), before.revision) == EditResult::Applied);
        before = session->snapshot(); auto *maintenance = workspace->resourceMaintenance(); QSignalSpy ready(maintenance, &ResourceMaintenanceController::previewReady), done(maintenance, &ResourceMaintenanceController::completed);
        QTRY_VERIFY_WITH_TIMEOUT(!workspace->references()->running(), 20000); QTest::qWait(300);
        maintenance->prepare("a.md", "new.md"); QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 30000); QVERIFY(maintenance->preview().plan.valid());
        bool started = false; QTRY_VERIFY2_WITH_TIMEOUT(started || (started = maintenance->apply()), qPrintable(maintenance->result().problem), 10000); QVERIFY(!workspace->close());
        QCOMPARE(session->replace(0, 0, "bad", before.revision), EditResult::Busy);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 30000); QCOMPARE(session->snapshot().utf8, QByteArray("unsaved\n[a](new.md)\n"));
        QVERIFY(session->isDirty()); QCOMPARE(disk(dir.filePath("b.md")), QByteArray("[a](a.md)\n"));
        app.openInWorkspace(dir.filePath("b.md"), workspace); QTest::qWait(300); QCOMPARE(workspace->panes().size(), 1);
        QCOMPARE(session->undo(), EditResult::Applied); QCOMPARE(session->snapshot().utf8, before.utf8);
        QCOMPARE(session->redo(), EditResult::Applied); QCOMPARE(session->snapshot().utf8, QByteArray("unsaved\n[a](new.md)\n"));
        QVERIFY(maintenance->recover()); QTRY_COMPARE_WITH_TIMEOUT(done.size(), 2, 30000); QCOMPARE(session->snapshot().utf8, before.utf8);
        QVERIFY(session->isDirty()); QVERIFY(QFileInfo::exists(dir.filePath("a.md"))); QVERIFY(!QFileInfo::exists(dir.filePath("new.md")));
    }
    void acceptedFileCapabilitySurvivesSourceRootRevocation() {
        QTemporaryDir dir; const auto path = dir.filePath("doc.md"); QVERIFY(fileFixtures::write(path, "original"));
        auto root = WorkspaceResourceReader::open(dir.path()).reader; QVERIFY(root); const auto probe = FileService::probeWithin(root, "doc.md"); QVERIFY(probe.file);
        const auto read = FileService::read({probe.file}); QVERIFY(read.baseline); root->revoke();
        FileSaveRequest request; request.path = path; request.baseline = read.baseline; request.utf8 = "changed";
        const auto saved = FileService::save(request); QVERIFY2(saved.status == SaveStatus::Committed, qPrintable(saved.problem.description())); QCOMPARE(disk(path), QByteArray("changed"));
    }
    void directoryMoveRebindsLiveDocumentAndRetainsUnsavedBaseline() {
        QTemporaryDir dir; QVERIFY(QDir().mkpath(dir.filePath("old"))); const auto path = dir.filePath("old/doc.md"); QVERIFY(fileFixtures::write(path, "[root](../root.md)\n")); QVERIFY(fileFixtures::write(dir.filePath("root.md"), "# root\n"));
        FileApplication app(nullptr, dir.filePath("settings.ini")); auto *workspace = app.workspaceWindow(dir.path()); app.openInWorkspace(path, workspace); auto *pane = waitPane(workspace, path); QVERIFY(pane); pane->setEditing(true);
        auto *session = pane->document()->session(); auto before = session->snapshot(); QVERIFY(session->replace(0, 0, "dirty\n", before.revision) == EditResult::Applied); before = session->snapshot();
        auto *maintenance = workspace->resourceMaintenance(); QSignalSpy ready(maintenance, &ResourceMaintenanceController::previewReady), done(maintenance, &ResourceMaintenanceController::completed);
        maintenance->prepare("old", "renamed"); QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 30000); QVERIFY2(maintenance->preview().plan.valid(), qPrintable(maintenance->preview().plan.problem));
        bool started = false; QTRY_VERIFY2_WITH_TIMEOUT(started || (started = maintenance->apply()), qPrintable(maintenance->result().problem), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 30000); QVERIFY2(maintenance->result().move.status == WorkspaceMoveStatus::Moved, qPrintable(maintenance->result().problem));
        QCOMPARE(pane->document()->path(), dir.filePath("renamed/doc.md")); QCOMPARE(session->snapshot().utf8, before.utf8); QVERIFY(session->isDirty());
        app.openInWorkspace(dir.filePath("renamed/doc.md"), workspace); QTest::qWait(300); QCOMPARE(workspace->panes().size(), 1);
        QSignalSpy saved(pane->document(), &FileDocument::saveFinished); QVERIFY(pane->document()->save()); QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 1, 20000); QVERIFY(saved.first().first().value<SaveStatus>() == SaveStatus::Committed);
        QCOMPARE(disk(dir.filePath("renamed/doc.md")), before.utf8); QVERIFY(!session->isDirty());
    }
    void externalMoveRepairRequiresConfirmedTargetAndDoesNotGuessSourcePaths() {
        QTemporaryDir dir; QVERIFY(fileFixtures::write(dir.filePath("target.md"), "# target\n")); QVERIFY(fileFixtures::write(dir.filePath("source.md"), "[a](missing.md#target)"));
        auto root = WorkspaceResourceReader::open(dir.path()).reader; QVERIFY(root); ReferenceIndexInput input{QUuid::createUuid(), 1, 1, false, {}, {}};
        const auto rejected = prepareResourceMove(root, input, {"missing.md", "absent.md", {}, false, true}); QVERIFY(!rejected.plan.valid());
        const auto preview = prepareResourceMove(root, input, {"missing.md", "target.md", {}, false, true}); QVERIFY2(preview.plan.valid(), qPrintable(preview.plan.problem)); QVERIFY(!preview.plan.gaps.isEmpty());
        const auto result = executeResourceMove(root, preview, true); QVERIFY(result.move.status == WorkspaceMoveStatus::Moved);
        QCOMPARE(disk(dir.filePath("source.md")), QByteArray("[a](target.md#target)")); QCOMPARE(disk(dir.filePath("target.md")), QByteArray("# target\n")); QVERIFY(!QFileInfo::exists(dir.filePath("missing.md")));
        const auto reverse = reverseResourceMove(preview, result, result.movedIdentity); const auto restored = executeResourceMove(root, reverse, true);
        QVERIFY(restored.move.status == WorkspaceMoveStatus::Moved); QCOMPARE(disk(dir.filePath("source.md")), QByteArray("[a](missing.md#target)")); QVERIFY(QFileInfo::exists(dir.filePath("target.md")));
    }
    void headingPickerUsesDirtyTargetAndInboundLocationRejectsStaleOffsets() {
        QTemporaryDir dir; const auto a = dir.filePath("a.md"), b = dir.filePath("b.md");
        QVERIFY(fileFixtures::write(a, "# a\n")); QVERIFY(fileFixtures::write(b, "# disk\n"));
        FileApplication app(nullptr, dir.filePath("settings.ini")); auto *workspace = app.workspaceWindow(dir.path()); app.openInWorkspace(a, workspace); auto *source = waitPane(workspace, a); QVERIFY(source);
        app.openInWorkspace(b, workspace); auto *target = waitPane(workspace, b); QVERIFY(target); auto snapshot = target->document()->session()->snapshot();
        QVERIFY(target->document()->session()->replace(0, int(QString::fromUtf8(snapshot.utf8).size()), QStringLiteral("# 当前标题\n\n# 当前标题\n"), snapshot.revision) == EditResult::Applied);
        source->setEditing(true); source->editor()->send(SCI_GOTOPOS, uptr_t(4));
        QTimer select; select.setInterval(20); connect(&select, &QTimer::timeout, qApp, [&] {
            for (auto *widget : QApplication::topLevelWidgets()) if (widget->objectName() == "workspaceLinkHeadingMenu") {
                auto *menu = qobject_cast<QMenu *>(widget); for (auto *action : menu->actions()) if (action->data().toString().endsWith("-1")) { menu->setActiveAction(action); QTest::keyClick(menu, Qt::Key_Return); select.stop(); return; }
            }
        }); select.start(); QSignalSpy inserted(source, &SingleFileWindow::workspaceLinkFinished);
        source->insertWorkspaceDocumentLink(b, {}, true); QTRY_COMPARE_WITH_TIMEOUT(inserted.size(), 1, 20000); select.stop(); QVERIFY2(inserted.first().first().toBool(), qPrintable(inserted.first().last().toString()));
        auto sourceSnapshot = source->document()->session()->snapshot(); QVERIFY(sourceSnapshot.utf8.contains("b.md#")); QVERIFY(sourceSnapshot.utf8.contains("-1")); QVERIFY(!sourceSnapshot.utf8.contains("#disk"));
        QCOMPARE(disk(b), QByteArray("# disk\n")); const auto identity = source->document()->identity();
        source->navigateToReference(identity, QCryptographicHash::hash(sourceSnapshot.utf8, QCryptographicHash::Sha256), 4, sourceSnapshot.utf8.size());
        QCOMPARE(qsizetype(source->editor()->send(SCI_GETANCHOR)), qsizetype(4));
        QVERIFY(source->document()->session()->replace(0, 0, "later", sourceSnapshot.revision) == EditResult::Applied);
        source->editor()->send(SCI_GOTOPOS, uptr_t(0)); source->navigateToReference(identity, QCryptographicHash::hash(sourceSnapshot.utf8, QCryptographicHash::Sha256), 4, sourceSnapshot.utf8.size());
        QCOMPARE(source->editor()->send(SCI_GETCURRENTPOS), sptr_t(0));
        QTRY_VERIFY_WITH_TIMEOUT(workspace->references()->current(workspace->references()->generation()), 20000);
        const auto diagnostics = workspace->references()->result().fragmentProblems; QVERIFY(diagnostics.isEmpty());
    }
    void standaloneImageImportUsesExplicitDirectoryAndKeepsOriginalBytes() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); QVERIFY(QDir().mkpath(dir.filePath("docs"))); QVERIFY(QDir().mkpath(dir.filePath("pictures")));
        const auto path = dir.filePath("docs/doc.md"), source = dir.filePath(QStringLiteral("图 #%.png"));
        QVERIFY(fileFixtures::write(path, "# doc\n")); QImage image(4, 3, QImage::Format_ARGB32); image.fill(Qt::red);
        QByteArray bytes; QBuffer buffer(&bytes); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&buffer, "PNG")); QVERIFY(fileFixtures::write(source, bytes));
        FileApplication app(nullptr, dir.filePath("settings/state.ini")); app.open(path); auto *pane = waitStandalone(path); QVERIFY(pane); pane->setEditing(true);
        pane->editor()->send(SCI_GOTOPOS, uptr_t(6)); QSignalSpy imported(pane, &SingleFileWindow::imageImportFinished);
        pane->importImageFile(source, dir.filePath("pictures")); QTRY_COMPARE_WITH_TIMEOUT(imported.size(), 1, 20000);
        QVERIFY2(imported.first().first().toBool(), qPrintable(imported.first().last().toString()));
        const auto saved = dir.filePath(QStringLiteral("pictures/图 #%.png")); QCOMPARE(disk(saved), bytes); QCOMPARE(disk(source), bytes);
        const auto snapshot = pane->document()->session()->snapshot(); QVERIFY(snapshot.utf8.contains("../pictures/")); QVERIFY(snapshot.utf8.contains("%23%25"));
        pane->setEditing(false); QTRY_COMPARE_WITH_TIMEOUT(loadedImages(pane->preview()), 1, 15000);
        QVERIFY(QFile::rename(saved, saved + ".original")); QVERIFY(fileFixtures::write(saved, "changed image"));
        QTRY_COMPARE_WITH_TIMEOUT(loadedImages(pane->preview()), 0, 15000);
        QVERIFY(QFile::remove(saved)); QVERIFY(QFile::rename(saved + ".original", saved));
        QTRY_COMPARE_WITH_TIMEOUT(loadedImages(pane->preview()), 1, 15000);
        QCOMPARE(pane->document()->session()->undo(), EditResult::Applied); QCOMPARE(pane->document()->session()->snapshot().utf8, QByteArray("# doc\n")); QCOMPARE(disk(saved), bytes);
        QVERIFY(!QFileInfo::exists(dir.filePath("assets"))); QVERIFY(!pane->embedded());
        for (auto *widget : QApplication::topLevelWidgets()) QVERIFY(!qobject_cast<WorkspaceWindow *>(widget));
    }
    void imageDiagnosticsDistinguishBrokenAndMissingResources() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); QVERIFY(QDir().mkpath(dir.filePath("assets/images")));
        QVERIFY(fileFixtures::write(dir.filePath("doc.md"), "![bad](assets/images/bad.png)\n![missing](assets/images/missing.png)"));
        QVERIFY(fileFixtures::write(dir.filePath("assets/images/bad.png"), "not an image"));
        FileApplication app(nullptr, dir.filePath("settings/state.ini")); auto *workspace = app.workspaceWindow(dir.path());
        app.openInWorkspace(dir.filePath("doc.md"), workspace); QVERIFY(waitPane(workspace, dir.filePath("doc.md")));
        auto *references = workspace->references(); QTRY_VERIFY_WITH_TIMEOUT(references->current(references->generation()), 20000);
        const auto &targets = references->result().targets;
        QVERIFY(targets.value("assets/images/bad.png").imageChecked); QVERIFY(!targets.value("assets/images/bad.png").diagnostic.isEmpty());
        QCOMPARE(targets.value("assets/images/missing.png").error, WorkspaceReadError::Missing);
        QCOMPARE(disk(dir.filePath("assets/images/bad.png")), QByteArray("not an image"));
    }
    void workspaceReferencesTrackSessionsFilesAndMenuCoverage() {
#if !defined(Q_OS_WIN) && !defined(Q_OS_LINUX) && !defined(Q_OS_MACOS)
        QSKIP("Confined resource indexing is not available on this platform");
#else
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto source = dir.filePath("source.md"), other = dir.filePath("other.md");
        QVERIFY(fileFixtures::write(source, "[old](old.md)\n![missing](assets/images/missing.png)"));
        QVERIFY(fileFixtures::write(other, "[outside-pane](source.md)"));
        QVERIFY(QDir().mkpath(dir.filePath("assets/images"))); QVERIFY(fileFixtures::write(dir.filePath("assets/images/unused.png"), "candidate"));
        FileApplication app(nullptr, dir.filePath("settings/state.ini")); auto *workspace = app.workspaceWindow(dir.path());
        app.openInWorkspace(source, workspace); auto *pane = waitPane(workspace, source); QVERIFY(pane);
        app.open(other); auto *standalone = waitStandalone(other); QVERIFY(standalone);
        auto *index = workspace->references();
        QTRY_VERIFY_WITH_TIMEOUT(index->current(index->generation()), 15000);
        QCOMPARE(index->result().index.inbound("source.md").size(), 1);
        QCOMPARE(index->result().targets.value("assets/images/missing.png").error, WorkspaceReadError::Missing);
        QVERIFY(index->result().imageCandidates.contains("assets/images/unused.png"));
        const auto before = pane->document()->session()->snapshot();
        QCOMPARE(pane->document()->session()->replace(0, 0, "[dirty](new.md)\n", before.revision), EditResult::Applied);
        QTRY_VERIFY_WITH_TIMEOUT(index->current(index->generation()) && index->result().index.inbound("new.md").size() == 1, 15000);
        QVERIFY(!disk(source).contains("new.md"));
        auto *menu = pane->findChild<QMenu *>("workspaceReferencesMenu"); QVERIFY(menu);
        QVERIFY(QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection));
        QVERIFY(menu->findChild<QMenu *>("outboundReferencesMenu")); QVERIFY(menu->findChild<QMenu *>("inboundReferencesMenu"));
        QVERIFY(menu->findChild<QMenu *>("referenceProblemsMenu")); QVERIFY(menu->findChild<QMenu *>("unreferencedImagesMenu"));
        const auto generation = index->generation(); pane->document()->stateChanged(); QCOMPARE(index->generation(), generation);
        QVERIFY(fileFixtures::write(other, "[changed](another.md)"));
        // The opened session remains authoritative despite its external disk change.
        QTRY_VERIFY_WITH_TIMEOUT(index->generation() > generation && index->current(index->generation()), 15000);
        QCOMPARE(index->result().index.inbound("source.md").size(), 1); QVERIFY(index->result().index.inbound("another.md").isEmpty());
        QVERIFY(fileFixtures::write(dir.filePath("unopened.md"), "[new-file](fresh.md)"));
        QTRY_VERIFY_WITH_TIMEOUT(index->current(index->generation()) && index->result().index.inbound("fresh.md").size() == 1, 15000);
#endif
    }
    void workspaceReferencesCancelAndDiscardPreviousRoot() {
#if !defined(Q_OS_WIN) && !defined(Q_OS_LINUX) && !defined(Q_OS_MACOS)
        QSKIP("Confined resource indexing is not available on this platform");
#else
        QTemporaryDir first, second; QVERIFY(first.isValid() && second.isValid());
        QVERIFY(fileFixtures::write(first.filePath("a.md"), "[old](old.md)"));
        QVERIFY(fileFixtures::write(second.filePath("b.md"), "[new](new.md)"));
        FileApplication app(nullptr, first.filePath("settings/state.ini")); auto *workspace = app.workspaceWindow(first.path()); auto *index = workspace->references();
        QTRY_VERIFY_WITH_TIMEOUT(index->current(index->generation()), 15000);
        auto *pool = QThreadPool::globalInstance(); const int previous = pool->maxThreadCount(); pool->setMaxThreadCount(1);
        QSemaphore entered, gate; auto blocker = QtConcurrent::run([&] { entered.release(); gate.acquire(); });
        const auto release = qScopeGuard([&] { gate.release(); blocker.waitForFinished(); pool->setMaxThreadCount(previous); });
        QVERIFY(entered.tryAcquire(1, 10000)); index->refresh(); QTest::qWait(250); QVERIFY(index->running());
        index->cancel(); QVERIFY(!index->current(index->generation())); QVERIFY(index->result().index.references.isEmpty());
        workspace->setRootPath(second.path()); gate.release();
        QTRY_VERIFY_WITH_TIMEOUT(index->current(index->generation()), 15000);
        QVERIFY(index->result().index.inbound("old.md").isEmpty()); QCOMPARE(index->result().index.inbound("new.md").size(), 1);
#endif
    }
    void documentLinksReuseDirtyTargetAndNavigateHeadings() {
#if !defined(Q_OS_WIN) && !defined(Q_OS_LINUX) && !defined(Q_OS_MACOS)
        QSKIP("Confined document links are not available on this platform");
#else
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto source = dir.filePath("source.md"), target = dir.filePath(QStringLiteral("中文 #%.md"));
        QVERIFY(fileFixtures::write(source, "[link](target.md)")); QVERIFY(fileFixtures::write(target, "# old\n"));
        FileApplication app(nullptr, dir.filePath("settings/state.ini")); auto *workspace = app.workspaceWindow(dir.path());
        app.openInWorkspace(source, workspace); auto *pane = waitPane(workspace, source); QVERIFY(pane);
        app.open(target); auto *destination = waitStandalone(target); QVERIFY(destination); destination->setEditing(true);
        auto *session = destination->document()->session(); const auto before = session->snapshot();
        const auto text = QStringLiteral("# 已修改\n\n正文\n\n# 已修改\n");
        QCOMPARE(session->replace(0, QString::fromUtf8(before.utf8).size(), text, before.revision), EditResult::Applied);
        const auto live = session->snapshot(); const auto headings = analyzeMarkdownHeadings(live); QVERIFY(headings.success);
        const auto url = relativeWorkspaceUrl("source.md", QFileInfo(target).fileName()); QVERIFY(url.success);
        pane->activateDocumentLink(url.path + QStringLiteral("#%E5%B7%B2%E4%BF%AE%E6%94%B9-1"), pane->document()->session()->revision());
        QTRY_COMPARE(destination->editor()->send(SCI_GETCURRENTPOS), sptr_t(headings.headings.last().range.start));
        QCOMPARE(session->snapshot().utf8, live.utf8); QVERIFY(session->isDirty()); QVERIFY(!destination->embedded());
        QTRY_COMPARE(workspace->panes().size(), 1); QCOMPARE(disk(target), QByteArray("# old\n"));
        pane->activateDocumentLink(url.path + "#missing", pane->document()->session()->revision());
        QTRY_VERIFY(destination->findChild<QLabel *>("documentNotice")->text().contains(QStringLiteral("未找到标题")));
        QCOMPARE(session->undo(), EditResult::Applied); QCOMPARE(session->snapshot().utf8, before.utf8);
        const auto fresh = dir.filePath("fresh.md");
        QVERIFY(fileFixtures::write(fresh, QByteArray("paragraph\n\n").repeated(80) + "# destination\n"));
        pane->activateDocumentLink("fresh.md#destination", pane->document()->session()->revision());
        auto *newPane = waitPane(workspace, fresh); QVERIFY(newPane);
        const auto expected = analyzeMarkdownHeadings(newPane->document()->session()->snapshot()); QVERIFY(expected.success);
        QCOMPARE(newPane->editor()->send(SCI_GETCURRENTPOS), sptr_t(expected.headings.first().range.start));
        QTRY_VERIFY_WITH_TIMEOUT(newPane->preview()->view()->readingPosition().y() > 0, 15000);
#endif
    }
    void documentLinksRejectUnsafeTargetsAndStaleSource() {
#if !defined(Q_OS_WIN) && !defined(Q_OS_LINUX) && !defined(Q_OS_MACOS)
        QSKIP("Confined document links are not available on this platform");
#else
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto source = dir.filePath("source.md");
        QVERIFY(fileFixtures::write(source, "[target](target.md)")); QVERIFY(fileFixtures::write(dir.filePath("target.md"), "# target\n"));
        FileApplication app(nullptr, dir.filePath("settings/state.ini")); auto *workspace = app.workspaceWindow(dir.path());
        app.openInWorkspace(source, workspace); auto *pane = waitPane(workspace, source); QVERIFY(pane);
        for (const auto &target : {"../outside.md", "%2e%2e/outside.md", "file:///private.md", "javascript:bad", "image.png"}) {
            pane->activateDocumentLink(target, pane->document()->session()->revision()); QCOMPARE(workspace->panes().size(), 1);
        }
        auto *pool = QThreadPool::globalInstance(); const int previous = pool->maxThreadCount(); pool->setMaxThreadCount(1);
        QSemaphore entered, gate; auto blocker = QtConcurrent::run([&] { entered.release(); gate.acquire(); });
        const auto release = qScopeGuard([&] { gate.release(); blocker.waitForFinished(); pool->setMaxThreadCount(previous); });
        QVERIFY(entered.tryAcquire(1, 10000));
        pane->activateDocumentLink("target.md#target", pane->document()->session()->revision());
        QCOMPARE(pane->document()->session()->replace(0, 0, "changed ", pane->document()->session()->revision()), EditResult::Applied);
        gate.release();
        QTRY_COMPARE_WITH_TIMEOUT(workspace->panes().size(), 1, 15000);
        QTRY_VERIFY(pane->findChild<QLabel *>("documentNotice")->text().contains(QStringLiteral("已变化")));
        QVERIFY(pane->document()->session()->snapshot().utf8.startsWith("changed ")); QCOMPARE(disk(dir.filePath("target.md")), QByteArray("# target\n"));
#endif
    }
    void workspaceReferencesDoNotUseDiskAliasesOfOutsideSessions() {
#if !defined(Q_OS_WIN) && !defined(Q_OS_LINUX) && !defined(Q_OS_MACOS)
        QSKIP("Confined resource indexing is not available on this platform");
#else
        QTemporaryDir dir; QVERIFY(dir.isValid()); QVERIFY(QDir().mkpath(dir.filePath("root")));
        const auto outside = dir.filePath("outside.md"), alias = dir.filePath("root/alias.md");
        QVERIFY(fileFixtures::write(outside, "[old](old.md)")); QVERIFY(fileFixtures::hardLink(outside, alias));
        FileApplication app(nullptr, dir.filePath("settings/state.ini")); app.open(outside);
        auto *pane = waitStandalone(outside); QVERIFY(pane);
        QCOMPARE(pane->document()->session()->replace(0, 0, "[dirty](dirty.md) ", pane->document()->session()->revision()), EditResult::Applied);
        auto *workspace = app.workspaceWindow(dir.filePath("root")); auto *index = workspace->references();
        QTRY_VERIFY_WITH_TIMEOUT(index->current(index->generation()), 15000);
        QVERIFY(!index->result().index.complete); QVERIFY(index->result().index.inbound("old.md").isEmpty());
        QVERIFY(!index->result().index.gaps.isEmpty());
        chooseClose(2); pane->close();
        QTRY_VERIFY_WITH_TIMEOUT(index->current(index->generation()) && index->result().index.inbound("old.md").size() == 1, 15000);
#endif
    }
    void documentLinksPreviewClicksKeepStandaloneRouting() {
#if !defined(Q_OS_WIN) && !defined(Q_OS_LINUX) && !defined(Q_OS_MACOS)
        QSKIP("Confined document links are not available on this platform");
#else
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto source = dir.filePath("source.md"), target = dir.filePath("target.md");
        QVERIFY(fileFixtures::write(source, "[open](target.md#target)")); QVERIFY(fileFixtures::write(target, "intro\n\n# target\n"));
        FileApplication app(nullptr, dir.filePath("settings/state.ini")); app.open(source); auto *pane = waitStandalone(source); QVERIFY(pane);
        auto *preview = pane->preview();
        QTRY_VERIFY_WITH_TIMEOUT(preview->view()->content() && !preview->view()->content()->actions.isEmpty() && preview->view()->ready(), 15000);
        QSignalSpy requested(pane, &SingleFileWindow::documentLinkRequested);
        auto *view = preview->view(); const auto &action = view->content()->actions.first();
        const auto point = view->viewport()->mapFromGlobal(view->characterRect(action.start).center());
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        auto *destination = waitStandalone(target); QVERIFY(destination); QVERIFY(!destination->embedded());
        QCOMPARE(requested.size(), 1); QCOMPARE(destination->document()->session()->snapshot().utf8, QByteArray("intro\n\n# target\n"));
        QCOMPARE(destination->editor()->send(SCI_GETCURRENTPOS), sptr_t(7));
        for (auto *widget : QApplication::topLevelWidgets()) QVERIFY(!qobject_cast<WorkspaceWindow *>(widget));
#endif
    }
    void initTestCase() { qApp->setQuitOnLastWindowClosed(false); SourceEditor::installAccessibility(); }
    void toolbarBordersAndHeadingDropdown_data() { QTest::addColumn<QString>("theme"); QTest::newRow("light") << QString("light"); QTest::newRow("dark") << QString("dark"); }
    void toolbarBordersAndHeadingDropdown() {
        QFETCH(QString, theme); QTemporaryDir dir; const auto path = dir.filePath("toolbar.md"); QVERIFY(fileFixtures::write(path, "original\n"));
        FileApplication desktop; desktop.open(path); auto *pane = waitStandalone(path); QVERIFY(pane); QVERIFY(pane->appearance()->setPreferences(theme, "graphite"));
        auto *format = pane->findChild<QWidget *>("formatToolbar"); auto *preview = pane->findChild<QWidget *>("previewToolbar"); auto *body = pane->findChild<QStackedWidget *>("documentBody");
        auto *heading = pane->findChild<SharedButton *>("format2"); auto *label = pane->findChild<QLabel *>("formatLabel"); auto *divider = pane->findChild<QWidget *>("formatDivider");
        QVERIFY(format && preview && body && heading && label && divider); pane->setViewMode(SingleFileWindow::ViewMode::Source); QTRY_VERIFY(format->isVisible()); QVERIFY(!preview->isVisible());
        QTRY_COMPARE(divider->mapTo(format, QPoint()).x() - label->mapTo(format, QPoint(label->width(), 0)).x(), 16); QCOMPARE(heading->width(), 48);
        auto *menu = heading->menu(); QVERIFY(menu); QCOMPARE(menu->actions().size(), 6); for (int i = 0; i < 6; ++i) QCOMPARE(menu->actions()[i]->data().toInt(), i + 1);
        capture(pane, theme + "-toolbar"); const auto indicator = pane->grab().toImage(); const qreal ratio = indicator.devicePixelRatio(); const auto origin = heading->mapTo(pane, QPoint());
        const auto background = indicator.pixelColor(qFloor((origin.x() + 2) * ratio), qFloor((origin.y() + 2) * ratio)); int contrast = 0;
        for (int y = qFloor((origin.y() + 8) * ratio); y < qFloor((origin.y() + 24) * ratio); ++y) for (int x = qFloor((origin.x() + 28) * ratio); x < qFloor((origin.x() + 40) * ratio); ++x) { const auto pixel = indicator.pixelColor(x, y); if (qAbs(pixel.red() - background.red()) + qAbs(pixel.green() - background.green()) + qAbs(pixel.blue() - background.blue()) > 150) ++contrast; }
        QVERIFY(contrast >= 2);
        bool opened = false; connect(menu, &QMenu::aboutToShow, menu, [menu, &opened] { opened = true; QTimer::singleShot(0, menu, [menu] { menu->actions()[3]->trigger(); menu->close(); }); });
        QTimer::singleShot(1000, menu, &QMenu::close); QTest::mouseClick(heading, Qt::LeftButton); QTRY_VERIFY_WITH_TIMEOUT(opened, 2000); auto *session = pane->document()->session(); QTRY_COMPARE_WITH_TIMEOUT(session->snapshot().utf8, QByteArray("#### original\n"), 2000); QCOMPARE(session->undo(), EditResult::Applied);
        auto checkRightRule = [&] { const auto image = body->grab().toImage(); return hasRuleAt(image, QPoint(body->width() - qMax(1, body->contentsMargins().right() / 2), format->height() - 1), pane->appearance()->color("border")); };
        QTRY_VERIFY(checkRightRule()); pane->setViewMode(SingleFileWindow::ViewMode::Reading); QVERIFY(!format->isVisible()); QVERIFY(!preview->isVisible());
        auto *workspace = desktop.workspaceWindow(dir.path()); QVERIFY(desktop.transferToWorkspace(pane, workspace)); pane->setViewMode(SingleFileWindow::ViewMode::Source); QTRY_VERIFY(format->isVisible()); QVERIFY(!preview->isVisible()); QTRY_VERIFY(checkRightRule());
        pane->setViewMode(SingleFileWindow::ViewMode::Split); QTRY_VERIFY(format->isVisible()); QTRY_VERIFY(preview->isVisible()); QCOMPARE(format->height(), preview->height());
        auto *splitter = pane->findChild<QSplitter *>("documentSplit"); QVERIFY(splitter); auto *handle = splitter->handle(1); QTRY_VERIFY(handle->isVisible()); const auto bridge = handle->grab().toImage();
        QVERIFY(hasRuleAt(bridge, QPoint(1, format->height() - 1), pane->appearance()->color("border"))); QVERIFY(hasRuleAt(bridge, QPoint(handle->width() - 2, format->height() - 1), pane->appearance()->color("border"))); QTRY_VERIFY(checkRightRule());
        pane->setViewMode(SingleFileWindow::ViewMode::Reading); QVERIFY(!format->isVisible()); QVERIFY(!preview->isVisible());
    }
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
        QVERIFY(QFile::remove(appearancePath)); QVERIFY(QDir().mkdir(appearancePath)); QVERIFY(!appearance.setPreferences("dark", appearance.accent())); QCOMPARE(appearance.theme(), QString("light")); QVERIFY(notice->isVisible()); QVERIFY(QDir().rmdir(appearancePath)); QVERIFY(appearance.setPreferences("dark", appearance.accent())); QCOMPARE(appearance.theme(), QString("dark")); QVERIFY(!notice->isVisible()); QCOMPARE(disk(document), QByteArray("# retained")); delete workspace;
    }
    void navigationTabsAreUniqueClosableAndPreserveDirtyDocument() {
        QTemporaryDir dir; const auto path = dir.filePath("retained.md"); QVERIFY(fileFixtures::write(path, "original"));
        FileApplication desktop(nullptr, dir.filePath("settings.json")); auto *workspace = desktop.workspaceWindow(dir.path()); desktop.openInWorkspace(path, workspace);
        auto *pane = waitPane(workspace, path); QVERIFY(pane); auto *session = pane->document()->session();
        QCOMPARE(session->replace(0, 0, "dirty ", session->revision()), EditResult::Applied); const auto snapshot = session->snapshot();
        auto *tabs = workspace->findChild<QTabBar *>("workspaceTabs"); QCOMPARE(tabs->count(), 1);
        auto *recent = workspace->findChild<SharedButton *>("workspaceRecent"); auto *favorites = workspace->findChild<SharedButton *>("workspaceFavorites");
        recent->click(); QCOMPARE(tabs->count(), 2); const auto recentId = tabs->tabData(tabs->currentIndex()).toMap().value("id").toUuid(); QVERIFY(!workspace->activePane());
        recent->click(); QCOMPARE(tabs->count(), 2); QCOMPARE(tabs->tabData(tabs->currentIndex()).toMap().value("id").toUuid(), recentId);
        favorites->click(); QCOMPARE(tabs->count(), 3); const auto favoriteId = tabs->tabData(tabs->currentIndex()).toMap().value("id").toUuid();
        tabs->moveTab(2, 0); favorites->click(); QCOMPARE(tabs->count(), 3); QCOMPARE(tabs->tabData(tabs->currentIndex()).toMap().value("id").toUuid(), favoriteId);
        workspace->activatePane(pane->paneId()); QVERIFY(pane->isVisible()); QCOMPARE(workspace->activePane(), pane); QCOMPARE(session->snapshot().utf8, snapshot.utf8); QCOMPARE(session->revision(), snapshot.revision);
        workspace->closePane(recentId); QCOMPARE(tabs->count(), 2); QCOMPARE(workspace->panes().size(), 1); QVERIFY(pane->document()->dirty());
        workspace->closePane(favoriteId); QCOMPARE(tabs->count(), 1); QCOMPARE(workspace->activePane(), pane); QCOMPARE(session->undo(), EditResult::Applied); QCOMPARE(session->snapshot().utf8, QByteArray("original")); QCOMPARE(disk(path), QByteArray("original"));
    }
    void favoritesRemainUniqueWhileReopeningDocuments() {
        QTemporaryDir dir; const auto path = dir.filePath("favorite.md"); QVERIFY(fileFixtures::write(path, "favorite"));
        FileApplication desktop(nullptr, dir.filePath("settings.json")); auto *workspace = desktop.workspaceWindow(dir.path()); desktop.openInWorkspace(path, workspace); QVERIFY(waitPane(workspace, path));
        auto *store = desktop.findChild<NavigationStore *>(); auto *manager = desktop.findChild<FileManager *>(); QVERIFY(store); QVERIFY(manager);
        auto *favorites = workspace->findChild<SharedButton *>("workspaceFavorites"); auto *list = workspace->findChild<QListWidget *>("navigationResults");
        favorites->click(); QCOMPARE(list->count(), 0); QVERIFY(store->setFavorite(path, true)); QCOMPARE(list->count(), 1);
        for (int turn = 0; turn < 6; ++turn) {
            desktop.openInWorkspace(path, workspace); QTRY_VERIFY(manager->idle()); favorites->click();
            QCOMPARE(store->favorites().size(), 1); QCOMPARE(list->count(), 1); QCOMPARE(list->item(0)->data(Qt::UserRole).toString(), QFileInfo(path).canonicalFilePath());
        }
        QVERIFY(store->setFavorite(path, false)); QCOMPARE(list->count(), 0); QVERIFY(store->favorites().isEmpty());
        for (int turn = 0; turn < 3; ++turn) { QVERIFY(store->recordOpen(path)); QCOMPARE(list->count(), 0); }
    }
    void favoritesRefreshIsAtomicDuringOpenNotification() {
        QTemporaryDir dir; const auto first = dir.filePath("first.md"), second = dir.filePath("second.md"); QVERIFY(fileFixtures::write(first, "first")); QVERIFY(fileFixtures::write(second, "second"));
        NavigationStore store(nullptr, dir.filePath("navigation.json")); QVERIFY(store.setFavorite(first, true)); QVERIFY(store.setFavorite(second, true)); Appearance appearance;
        WorkspaceNavigation page(&appearance, &store, nullptr); page.showPage(WorkspaceNavigation::Page::Favorites, dir.path()); auto *list = page.findChild<QListWidget *>("navigationResults"); list->setCurrentRow(0);
        bool reopened = false; connect(list, &QListWidget::currentRowChanged, &page, [&](int row) { if (row == 0 && !reopened) { reopened = true; store.recordOpen(first); } });
        QVERIFY(store.recordOpen(first)); QVERIFY(reopened); QCOMPARE(store.favorites().size(), 2); QCOMPARE(list->count(), 2); QCOMPARE(list->item(0)->data(Qt::UserRole).toString(), first); QCOMPARE(list->item(1)->data(Qt::UserRole).toString(), second);
    }
    void reopeningDocumentNeverPublishesDuplicateTabs() {
        QTemporaryDir dir; const auto path = dir.filePath("retained.md"); QVERIFY(fileFixtures::write(path, "retained"));
        FileApplication desktop(nullptr, dir.filePath("settings.json")); auto *workspace = desktop.workspaceWindow(dir.path()); desktop.openInWorkspace(path, workspace);
        auto *pane = waitPane(workspace, path); QVERIFY(pane); auto *tabs = workspace->findChild<QTabBar *>("workspaceTabs"); auto *manager = desktop.findChild<FileManager *>(); QVERIFY(manager);
        auto *session = pane->document()->session(); QCOMPARE(session->replace(0, 0, "dirty ", session->revision()), EditResult::Applied); const auto snapshot = session->snapshot();
        int peak = tabs->count(); connect(tabs, &QTabBar::currentChanged, this, [&] { peak = qMax(peak, tabs->count()); });
        for (const auto &alias : QStringList{path, dir.path() + "/./retained.md", QDir::toNativeSeparators(path)}) {
            desktop.openInWorkspace(alias, workspace); QCOMPARE(tabs->count(), 1); QTRY_VERIFY(manager->idle()); QCOMPARE(peak, 1); QCOMPARE(workspace->activePane(), pane);
        }
        QCOMPARE(session->snapshot().utf8, snapshot.utf8); QCOMPARE(session->revision(), snapshot.revision); QCOMPARE(disk(path), QByteArray("retained"));
    }
    void footerControlsShareTypographyAndSpacing() {
        QTemporaryDir dir; const auto path = dir.filePath("footer.md"); QVERIFY(fileFixtures::write(path, "footer"));
        FileApplication desktop(nullptr, dir.filePath("settings.json")); desktop.open(path); auto *pane = waitStandalone(path); QVERIFY(pane);
        const auto check = [&] {
            auto *footer = pane->footer(); auto *zoom = footer->findChild<SharedButton *>("contentZoom");
            if (!zoom || zoom->font().pixelSize() != 12) return false;
            for (auto *label : footer->findChildren<QLabel *>()) if (!label->text().isEmpty() && label->font().pixelSize() != zoom->font().pixelSize()) return false;
            return zoom->geometry().top() >= 3 && footer->height() - zoom->geometry().bottom() - 1 >= 3;
        };
        QApplication::processEvents(); QVERIFY(check()); auto *workspace = desktop.workspaceWindow(dir.path()); QVERIFY(desktop.transferToWorkspace(pane, workspace)); QApplication::processEvents(); QVERIFY(check()); capture(pane->window(), "footer-compact");
    }
    void windowsWorkspaceNativeResizeAndRestoreKeepPreference() {
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName() != "windows") QSKIP("Requires the Windows platform plugin");
        QTemporaryDir dir; const auto settings = dir.filePath("settings.json"); const QSize normal(1040, 700);
        {
            FileApplication desktop(nullptr, settings); auto *workspace = desktop.workspaceWindow(dir.path()); QTRY_VERIFY(workspace->windowGeometry()->initialized());
            workspace->setWindowState(Qt::WindowActive); const auto hwnd = reinterpret_cast<HWND>(workspace->winId()); const auto ratio = workspace->devicePixelRatioF();
            QVERIFY(SetWindowPos(hwnd, nullptr, 0, 0, qRound(normal.width() * ratio), qRound(normal.height() * ratio), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
            QTRY_COMPARE(workspace->size(), normal);
            qInfo() << "native workspace normal" << workspace->size() << "preferred" << workspace->windowGeometry()->preferredClientSize() << "state" << workspace->windowState();
            QTRY_COMPARE(workspace->windowGeometry()->preferredClientSize(), normal);
            auto *button = workspace->findChild<SharedButton *>("captionMaximize"); QVERIFY(button);
            for (int turn = 0; turn < 3; ++turn) {
                button->click(); QTRY_VERIFY(workspace->isMaximized()); button->click(); QTRY_VERIFY(!workspace->isMaximized()); QTRY_COMPARE(workspace->size(), normal); QCOMPARE(workspace->windowGeometry()->preferredClientSize(), normal);
            }
            QVERIFY(workspace->windowGeometry()->flush());
        }
        WindowGeometryStore persisted(nullptr, settings + ".windows"); const auto saved = persisted.placement("main"); QVERIFY(saved); QCOMPARE(saved->normal.size(), normal); QVERIFY(!saved->maximized);
#else
        QSKIP("Windows native workspace regression");
#endif
    }
    void tabCloseAndSidebarBorderShareGeometryAndHover() {
        QTemporaryDir dir; const auto path = dir.filePath("a-long-document-title-that-needs-elision.md"); QVERIFY(fileFixtures::write(path, "# title"));
        FileApplication desktop(nullptr, dir.filePath("settings.json")); auto *workspace = desktop.workspaceWindow(dir.path()); desktop.openInWorkspace(path, workspace);
        auto *pane = waitPane(workspace, path); QVERIFY(pane);
        auto *tabs = workspace->findChild<QTabBar *>("workspaceTabs"); auto *border = workspace->findChild<QWidget *>("workspaceSidebarBorder");
        workspace->resize(1280, 820); QApplication::processEvents();
        QCOMPARE(tabs->mapTo(workspace, QPoint()).x(), border->mapTo(workspace, QPoint()).x());
        auto *button = qobject_cast<SharedButton *>(tabs->tabButton(0, QTabBar::RightSide)); QVERIFY(button); QCOMPARE(button->size(), QSize(24, 24));
        const auto tab = tabs->tabRect(0); QVERIFY(tab.contains(button->geometry()));
        QCOMPARE(button->geometry().center().y(), QRect(0, 6, tab.width(), tabs->height() - 6).center().y());
        QVERIFY(button->geometry().right() <= tab.right() - 7);
        for (const auto *theme : {"light", "dark"}) {
            QVERIFY(pane->appearance()->setPreferences(theme, "graphite"));
            QEvent leave(QEvent::Leave); QApplication::sendEvent(button, &leave); const auto normal = button->grab().toImage();
            QEnterEvent enter(QPointF(3, 12), QPointF(3, 12), QPointF(3, 12)); QApplication::sendEvent(button, &enter); const auto hover = button->grab().toImage();
            QVERIFY(normal != hover); QVERIFY(!button->keyboardOutline()); capture(workspace, QString::fromLatin1(theme) + "-tab-alignment");
            auto *nav = workspace->findChild<SharedButton *>("workspaceRecent"); QApplication::sendEvent(nav, &leave); const auto navNormal = nav->grab().toImage(); QApplication::sendEvent(nav, &enter); QVERIFY(navNormal != nav->grab().toImage());
        }
    }
    void pendingWorkspaceCloseCancelsUnpublishedTab() {
        QTemporaryDir dir; const auto path = dir.filePath("pending.md"); QVERIFY(fileFixtures::write(path, "pending")); FileApplication desktop(nullptr, dir.filePath("settings.json")); auto *workspace = desktop.workspaceWindow(dir.path());
        auto *pool = QThreadPool::globalInstance(); const int previous = pool->maxThreadCount(); pool->setMaxThreadCount(1); QSemaphore entered, release; auto worker = QtConcurrent::run([&] { entered.release(); release.acquire(); });
        bool released = false; const auto restore = qScopeGuard([&] { if (!released) release.release(); worker.waitForFinished(); pool->setMaxThreadCount(previous); }); QVERIFY(entered.tryAcquire(1, 5000));
        desktop.openInWorkspace(path, workspace); QCOMPARE(workspace->findChild<QTabBar *>("workspaceTabs")->count(), 0); delete workspace; release.release(); released = true; worker.waitForFinished();
        auto *manager = desktop.findChild<FileManager *>(); QVERIFY(manager); QTRY_VERIFY(manager->idle()); QCOMPARE(manager->documentCount(), 0);
    }
    void windowsLegacyMaximizedBoundsRecoverOnlyOnce() {
#ifdef Q_OS_WIN
        QTemporaryDir dir; const auto settings = dir.filePath("settings.json"), state = settings + ".windows"; const auto available = QGuiApplication::primaryScreen()->availableGeometry();
        WindowGeometryStore store(nullptr, state); WindowPlacement legacy{available, available, true, 1}; QVERIFY(store.save("main", legacy));
        const auto defaults = visualTokens()["windows"].toObject()["main"].toObject(); const QSize normal(defaults["defaultWidth"].toInt(), defaults["defaultHeight"].toInt());
        { FileApplication desktop(nullptr, settings); auto *workspace = desktop.workspaceWindow(dir.path()); QTRY_VERIFY(workspace->windowGeometry()->initialized()); QCOMPARE(workspace->windowGeometry()->preferredClientSize(), normal); QVERIFY(workspace->windowGeometry()->flush()); }
        WindowGeometryStore migrated(nullptr, state); QVERIFY(migrated.placement("main")); QCOMPARE(migrated.placement("main")->normalVersion, 2); QCOMPARE(migrated.placement("main")->normal.size(), normal);
        legacy.normalVersion = 2; QVERIFY(migrated.save("main", legacy));
        { FileApplication desktop(nullptr, settings); auto *workspace = desktop.workspaceWindow(dir.path()); QTRY_VERIFY(workspace->windowGeometry()->initialized()); QCOMPARE(workspace->windowGeometry()->preferredClientSize(), available.size()); }
#else
        QSKIP("Windows legacy preference migration");
#endif
    }
    void tabControlsStayAlignedThroughoutChanges() {
        QTemporaryDir dir; const auto path = dir.filePath("retained.md"); QVERIFY(fileFixtures::write(path, "original"));
        FileApplication desktop(nullptr, dir.filePath("settings.json")); auto *workspace = desktop.workspaceWindow(dir.path()); desktop.openInWorkspace(path, workspace);
        auto *pane = waitPane(workspace, path); QVERIFY(pane); workspace->resize(1280, 820);
        workspace->findChild<SharedButton *>("workspaceRecent")->click(); workspace->findChild<SharedButton *>("workspaceFavorites")->click();
        auto *tabs = workspace->findChild<QTabBar *>("workspaceTabs"); auto *add = workspace->findChild<SharedButton *>("workspaceAddTab"); QVERIFY(tabs); QVERIFY(add); QApplication::processEvents();
        class Moves final : public QObject {
        public:
            int expectedY = 0, samples = 0; bool displaced = false;
            bool eventFilter(QObject *, QEvent *event) override {
                if (event->type() == QEvent::Move) { ++samples; displaced |= static_cast<QMoveEvent *>(event)->pos().y() != expectedY; }
                return false;
            }
        } moves;
        moves.expectedY = (tabs->height() + 6 - 24) / 2;
        for (int i = 0; i < tabs->count(); ++i) tabs->tabButton(i, QTabBar::RightSide)->installEventFilter(&moves);
        const auto check = [&] {
            for (int i = 0; i < tabs->count(); ++i) {
                const auto *button = tabs->tabButton(i, QTabBar::RightSide); const auto rect = tabs->tabRect(i);
                if (button->geometry().top() != moves.expectedY || button->geometry().right() != rect.right() - 8) return false;
                const auto center = button->mapTo(workspace, button->rect().center());
                if (center.y() != add->mapTo(workspace, add->rect().center()).y()) return false;
            }
            return !moves.displaced;
        };
        for (int turn = 0; turn < 3; ++turn) for (int i = 0; i < tabs->count(); ++i) { tabs->setCurrentIndex(i); QVERIFY2(check(), "Tab controls moved before the next event loop"); QApplication::processEvents(); QVERIFY(check()); }
        tabs->moveTab(2, 0); QVERIFY(check()); tabs->setTabText(1, "changed title"); QVERIFY(check());
        const auto otherPath = dir.filePath("other.md"); QVERIFY(fileFixtures::write(otherPath, "other")); desktop.openInWorkspace(otherPath, workspace);
        auto *other = waitPane(workspace, otherPath); QVERIFY(other); QVERIFY(check()); workspace->closePane(other->paneId()); QTRY_COMPARE(tabs->count(), 3); QVERIFY(check());
        workspace->resize(960, 640); QApplication::processEvents(); QVERIFY(check()); workspace->resize(1280, 820); QApplication::processEvents(); QVERIFY(check());
        workspace->activatePane(pane->paneId()); QVERIFY(check());
        capture(workspace, "stable-tab-controls");
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
        QTest::mouseClick(workspace->findChild<SharedButton *>("clearWorkspaceSearch"), Qt::LeftButton); QVERIFY(!workspace->activePane()); workspace->activatePane(pane->paneId()); QVERIFY(pane->isVisible()); QCOMPARE(pane->document()->session(), session); QCOMPARE(session->snapshot().utf8, QByteArray("dirty\n# unchanged"));
        QTest::mouseClick(workspace->findChild<SharedButton *>("workspaceRecent"), Qt::LeftButton); auto *tabs = workspace->findChild<QTabBar *>("workspaceTabs"); QTest::mouseClick(tabs, Qt::LeftButton, {}, tabs->tabRect(0).center()); QVERIFY(pane->isVisible());
        QTest::mouseClick(workspace->findChild<SharedButton *>("workspaceRecent"), Qt::LeftButton); QTest::mouseClick(results->viewport(), Qt::LeftButton, {}, QPoint(90, results->visualItemRect(results->item(1)).center().y())); QTRY_VERIFY(pane->isVisible()); QTRY_COMPARE(workspace->panes().size(), 1); QCOMPARE(pane->document()->session(), session);
        auto *settings = workspace->findChild<SharedButton *>("workspaceSettings"); QVERIFY(settings && !settings->menu()); QSignalSpy settingsRequested(workspace, &WorkspaceWindow::settingsRequested); QTest::mouseClick(settings, Qt::LeftButton); QCOMPARE(settingsRequested.size(), 1); pane->findChild<QAction *>("theme-dark")->trigger(); QVERIFY(pane->appearance()->dark()); QTest::mouseClick(workspace->findChild<SharedButton *>("workspaceTheme"), Qt::LeftButton); QVERIFY(!pane->appearance()->dark());
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
        auto *workspace = desktop.workspaceWindow(dir.path()); QVERIFY(desktop.transferToWorkspace(pane, workspace)); QTest::keyClick(edit, Qt::Key_Right); QCOMPARE(pane->viewMode(), SingleFileWindow::ViewMode::Split); QTest::keyClick(split, Qt::Key_Right); QCOMPARE(pane->viewMode(), SingleFileWindow::ViewMode::Reading);
        QCOMPARE(pane->findChild<QWidget *>("modeSwitch")->font().pixelSize(), 14);
    }
    void directVisibleWindowDestructionFlushesControllersBeforeBaseTeardown() {
        FileManager manager; Appearance appearance; WindowGeometryStore store;
        auto *standalone = new SingleFileWindow(&manager, &appearance, &store); standalone->show(); QTRY_VERIFY(standalone->windowGeometry()->initialized()); QPointer<WindowGeometry> first = standalone->windowGeometry(); QVERIFY(first); delete standalone; QVERIFY(first.isNull()); QVERIFY(store.placement("standalone"));
        QScopedPointer<WorkspaceWindow> owner(new WorkspaceWindow(&appearance, &store)); auto *workspace = owner.data(); workspace->show(); QTRY_VERIFY(workspace->windowGeometry()->initialized()); auto *header = workspace->findChild<QWidget *>("documentHeader"); QVERIFY(hasColorNear(header->grab().toImage(), QPointF(500, 47), appearance.color("border")));
        auto *pane = new SingleFileWindow(&manager, &appearance, &store); QVERIFY(workspace->addPane(pane)); auto *welcome = pane->findChild<SharedButton *>("welcomeWorkspace"); QTRY_VERIFY(welcome->width() >= QFontMetricsF(welcome->font(), welcome).horizontalAdvance(welcome->text()) + 48); QSignalSpy chooseTab(workspace, &WorkspaceWindow::chooseWorkspaceFileRequested); auto *addTab = workspace->findChild<SharedButton *>("workspaceAddTab"); QTest::mouseClick(addTab, Qt::LeftButton); QCOMPARE(chooseTab.size(), 1); QTest::keyClick(addTab, Qt::Key_Tab); QVERIFY(InputModality::forApplication()->keyboardNavigation()); QPointer<WindowGeometry> second = workspace->windowGeometry(); QPointer<SingleFileWindow> child = pane; owner.reset(); QVERIFY(second.isNull()); QVERIFY(child.isNull()); QVERIFY(store.placement("main"));
    }
    void documentInitializationToleratesSynchronousWindowWarning() {
        QTemporaryDir dir; const auto path = dir.filePath("reentrant.md"); QVERIFY(fileFixtures::write(path, "hello"));
        FileApplication desktop; InitializationWarning warning; qApp->installEventFilter(&warning);
        desktop.open(path); auto *pane = waitStandalone(path);
        qApp->removeEventFilter(&warning);
        QVERIFY(pane); QVERIFY(warning.injected); QVERIFY(pane->preview()); QVERIFY(pane->editor());
        QCOMPARE(pane->document()->session()->snapshot().utf8, QByteArray("hello"));
        pane->setEditing(true); QCOMPARE(pane->viewMode(), SingleFileWindow::ViewMode::Source);
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
        QVERIFY(!desktop.transferToWorkspace(pane)); QVERIFY(pane->isWindow());
        auto *workspace = desktop.workspaceWindow(dir.path()); QVERIFY(desktop.transferToWorkspace(pane, workspace)); QVERIFY(pane->embedded()); QCOMPARE(pane->window(), static_cast<QWidget *>(workspace)); QCOMPARE(workspace->rootPath(), dir.path());
        QCOMPARE(pane->editor(), editor); QCOMPARE(pane->document()->session(), session); QCOMPARE(session->documentId(), id); QCOMPARE(editor->send(SCI_GETANCHOR), sptr_t(3)); QCOMPARE(editor->send(SCI_GETCURRENTPOS), sptr_t(9)); QCOMPARE(editor->send(SCI_GETFIRSTVISIBLELINE), line);
        QCOMPARE(session->snapshot().utf8, QByteArray("dirty\n") + original); QCOMPARE(disk(path), original); QCOMPARE(session->undo(), EditResult::Applied); QCOMPARE(session->snapshot().utf8, original); QVERIFY(!QFileInfo::exists(dir.filePath("assets")));
    }
    void imageAccessFollowsStandaloneAndWorkspaceAuthorization() {
#if !defined(Q_OS_WIN) && !defined(Q_OS_LINUX) && !defined(Q_OS_MACOS)
        QSKIP("Confined resource reads are not available on this platform");
#else
        QTemporaryDir dir, replacement; QVERIFY(dir.isValid() && replacement.isValid());
        QVERIFY(QDir().mkpath(dir.filePath("docs"))); QVERIFY(QDir().mkpath(dir.filePath("assets")));
        const auto path = dir.filePath("docs/doc.md");
        const auto source = QByteArray("![local](local.png) ![shared](../assets/shared.png)");
        QVERIFY(fileFixtures::write(path, source));
        QImage image(3, 2, QImage::Format_ARGB32); image.fill(Qt::red);
        QByteArray png; QBuffer buffer(&png); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&buffer, "PNG"));
        QVERIFY(fileFixtures::write(dir.filePath("docs/local.png"), png)); QVERIFY(fileFixtures::write(dir.filePath("assets/shared.png"), png));
        FileApplication desktop; desktop.open(path); auto *pane = waitStandalone(path); QVERIFY(pane);
        QSignalSpy ready(pane->preview(), &MarkdownPreview::previewReady); pane->preview()->refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 30000); QTRY_COMPARE_WITH_TIMEOUT(loadedImages(pane->preview()), 1, 10000);
        auto *workspace = desktop.workspaceWindow(dir.path()); ready.clear();
        QVERIFY(desktop.transferToWorkspace(pane, workspace));
        QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 30000); QTRY_COMPARE_WITH_TIMEOUT(loadedImages(pane->preview()), 2, 10000);
        ready.clear(); workspace->setRootPath(replacement.path());
        QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 30000); QTRY_COMPARE_WITH_TIMEOUT(loadedImages(pane->preview()), 0, 10000);
        QCOMPARE(disk(path), source);
#endif
    }
    void imageImportPreservesBytesUsesRelativeLinkAndUndo() {
#ifndef Q_OS_WIN
        QSKIP("Safe image publication is currently implemented on Windows");
#else
        QTemporaryDir dir, outside; QVERIFY(dir.isValid() && outside.isValid());
        QVERIFY(QDir().mkpath(dir.filePath("notes")));
        const auto documentPath = dir.filePath("notes/doc.md"), imagePath = outside.filePath(QStringLiteral("图 #%.png"));
        const QByteArray original = QStringLiteral("前选择后").toUtf8(); QVERIFY(fileFixtures::write(documentPath, original));
        QImage image(3, 2, QImage::Format_ARGB32); image.fill(Qt::blue); image.setText("description", "source metadata");
        QByteArray png; QBuffer buffer(&png); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&buffer, "PNG"));
        QVERIFY(fileFixtures::write(imagePath, png));
        FileApplication desktop; auto *workspace = desktop.workspaceWindow(dir.path()); desktop.openInWorkspace(documentPath, workspace);
        auto *pane = waitPane(workspace, documentPath); QVERIFY(pane); pane->setEditing(true);
        auto *session = pane->document()->session(); auto *editor = pane->editor();
        const auto start = QStringLiteral("前").toUtf8().size(), end = QStringLiteral("前选择").toUtf8().size();
        editor->send(SCI_SETSEL, uptr_t(start), sptr_t(end)); QSignalSpy finished(pane, &SingleFileWindow::imageImportFinished);
        pane->importImageFile(imagePath); QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000);
        QVERIFY2(finished.first().first().toBool(), qPrintable(finished.first().at(1).toString()));
        const auto asset = dir.filePath(QStringLiteral("assets/images/图 #%.png")); QCOMPARE(disk(asset), png); QCOMPARE(disk(imagePath), png);
        QCOMPARE(session->snapshot().utf8, QStringLiteral("前![图片](../assets/images/%E5%9B%BE%20%23%25.png)后").toUtf8());
        QCOMPARE(disk(documentPath), original); QVERIFY(session->isDirty());
        QCOMPARE(session->undo(), EditResult::Applied); QCOMPARE(session->snapshot().utf8, original); QCOMPARE(disk(asset), png);
        finished.clear(); editor->send(SCI_SETSEL, 0, 0); pane->importClipboardImage(image);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000); QVERIFY2(finished.first().first().toBool(), qPrintable(finished.first().at(1).toString()));
        QVERIFY(session->snapshot().utf8.startsWith("!["));
        QCOMPARE(QDir(dir.filePath("assets/images")).entryList(QDir::Files).size(), 2);
#endif
    }
    void workspaceDocumentLinkMenuKeyboardUsesFilePicker() {
#if !defined(Q_OS_WIN) && !defined(Q_OS_LINUX) && !defined(Q_OS_MACOS)
        QSKIP("Confined workspace resources are not available on this platform");
#else
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QVERIFY(QDir().mkpath(dir.filePath("notes"))); QVERIFY(QDir().mkpath(dir.filePath("docs")));
        const auto documentPath = dir.filePath("notes/doc.md"), targetPath = dir.filePath("docs/target.md");
        QVERIFY(fileFixtures::write(documentPath, "source")); QVERIFY(fileFixtures::write(targetPath, "# target"));
        FileApplication desktop; auto *workspace = desktop.workspaceWindow(dir.path()); desktop.openInWorkspace(documentPath, workspace);
        auto *pane = waitPane(workspace, documentPath); QVERIFY(pane); pane->setEditing(true);
        auto *editor = pane->editor(); auto *session = pane->document()->session();
        auto *button = pane->findChild<SharedButton *>("moreActions"); QVERIFY(button);
        auto *menu = button->menu(); QVERIFY(menu);
        auto *insert = menu->findChild<QAction *>("insertWorkspaceLinkAction"); QVERIFY(insert); QVERIFY(insert->isEnabled());
        const bool previous = QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        const auto restore = qScopeGuard([previous] { QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, previous); });
        QApplication::setActiveWindow(workspace); editor->setFocus(Qt::OtherFocusReason); editor->send(SCI_SETSEL, 0, 0);
        QSignalSpy finished(pane, &SingleFileWindow::workspaceLinkFinished);
        QSignalSpy triggered(insert, &QAction::triggered);
        bool pickerSeen = false, selectionVerified = false, headingsSeen = false, timedOut = false;
        QTimer pickerProbe, watchdog; pickerProbe.setInterval(20); watchdog.setSingleShot(true);
        connect(&pickerProbe, &QTimer::timeout, this, [&] {
            for (auto *widget : QApplication::topLevelWidgets()) if (widget->objectName() == "workspaceLinkHeadingMenu") {
                headingsSeen = true;
                auto *headings = qobject_cast<QMenu *>(widget); headings->setActiveAction(headings->actions().first()); QTest::keyClick(headings, Qt::Key_Return); pickerProbe.stop(); return;
            }
            QFileDialog *dialog = nullptr;
            for (auto *widget : QApplication::topLevelWidgets())
                if (auto *candidate = qobject_cast<QFileDialog *>(widget); candidate && candidate->isVisible()) { dialog = candidate; break; }
            if (!dialog) return;
            if (pickerSeen) return;
            pickerSeen = true;
            // Qt 6.8.2 selectFile() does not replace the filename edit while
            // that visible edit owns focus (the initial state on macOS).
            if (auto *focused = dialog->focusWidget()) focused->clearFocus();
            dialog->selectFile(targetPath);
            const auto selected = dialog->selectedFiles();
            selectionVerified = selected.size() == 1 && QFileInfo(selected.first()).canonicalFilePath() == QFileInfo(targetPath).canonicalFilePath();
            if (!selectionVerified) { dialog->reject(); return; }
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        });
        connect(&watchdog, &QTimer::timeout, this, [&] {
            timedOut = true; menu->close();
            for (auto *widget : QApplication::topLevelWidgets())
                if (auto *dialog = qobject_cast<QFileDialog *>(widget); dialog && dialog->isVisible()) dialog->reject();
        });
        // Qt's test picker is intentional: native chooser behavior and real
        // mouse input require separate desktop verification.
        menu->popup(button->mapToGlobal(button->rect().bottomLeft()));
        QTRY_VERIFY(menu->isVisible());
        QVERIFY(QTest::qWaitForWindowExposed(menu, 5000));
        menu->activateWindow(); menu->setFocus(Qt::PopupFocusReason); QTest::qWait(50);
        menu->setActiveAction(insert); QCOMPARE(menu->activeAction(), insert);
        pickerProbe.start(); watchdog.start(10000);
        QTest::keyClick(menu, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(pickerSeen || timedOut, 12000);
        QVERIFY2(!timedOut, qPrintable(QStringLiteral("Qt picker timeout: action=%1 picker=%2 selected=%3 headings=%4").arg(triggered.size()).arg(pickerSeen).arg(selectionVerified).arg(headingsSeen)));
        QVERIFY2(selectionVerified, "The Qt test picker did not select the fixture target");
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000); watchdog.stop();
        QCOMPARE(triggered.size(), 1);
        QVERIFY2(finished.first().first().toBool(), qPrintable(finished.first().at(1).toString()));
        QCOMPARE(session->snapshot().utf8, QByteArray("[target.md](../docs/target.md)source"));
        QCOMPARE(disk(documentPath), QByteArray("source")); QCOMPARE(disk(targetPath), QByteArray("# target"));
        QCOMPARE(session->undo(), EditResult::Applied); QCOMPARE(session->snapshot().utf8, QByteArray("source"));
#endif
    }

    void workspaceDocumentLinksUseVerifiedRelativePathsAndUndo() {
#if !defined(Q_OS_WIN) && !defined(Q_OS_LINUX) && !defined(Q_OS_MACOS)
        QSKIP("Confined workspace resources are not available on this platform");
#else
        QTemporaryDir dir; QVERIFY(dir.isValid()); QVERIFY(QDir().mkpath(dir.filePath("notes"))); QVERIFY(QDir().mkpath(dir.filePath("docs")));
        const auto documentPath = dir.filePath("notes/doc.md"), targetPath = dir.filePath(QStringLiteral("docs/目标 #%.md"));
        const auto original = QStringLiteral("前选[择]后").toUtf8(); QVERIFY(fileFixtures::write(documentPath, original)); QVERIFY(fileFixtures::write(targetPath, "# target"));
        FileApplication desktop; auto *workspace = desktop.workspaceWindow(dir.path()); desktop.openInWorkspace(documentPath, workspace);
        auto *pane = waitPane(workspace, documentPath); QVERIFY(pane); pane->setEditing(true); auto *editor = pane->editor(); auto *session = pane->document()->session();
        const auto start = QStringLiteral("前").toUtf8().size(), end = QStringLiteral("前选[择]").toUtf8().size();
        editor->send(SCI_SETSEL, uptr_t(start), sptr_t(end)); QSignalSpy finished(pane, &SingleFileWindow::workspaceLinkFinished);
        pane->insertWorkspaceDocumentLink(targetPath); QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000);
        QVERIFY2(finished.first().first().toBool(), qPrintable(finished.first().at(1).toString()));
        QCOMPARE(session->snapshot().utf8, QStringLiteral("前[选\\[择\\]](../docs/%E7%9B%AE%E6%A0%87%20%23%25.md)后").toUtf8());
        QVERIFY(session->isDirty()); QCOMPARE(disk(documentPath), original); QCOMPARE(disk(targetPath), QByteArray("# target"));
        QCOMPARE(session->undo(), EditResult::Applied); QCOMPARE(session->snapshot().utf8, original);
        finished.clear(); editor->send(SCI_SETSEL, 0, 0); pane->insertWorkspaceDocumentLink(targetPath);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000); QVERIFY2(finished.first().first().toBool(), qPrintable(finished.first().at(1).toString()));
        QVERIFY(session->snapshot().utf8.startsWith(QStringLiteral("[目标 #%.md](../docs/").toUtf8()));
        const auto bom = QByteArray::fromHex("efbbbf");
        QVERIFY(fileFixtures::write(targetPath, bom + QByteArray(FileService::SourceLimit, 'x')));
        finished.clear(); editor->send(SCI_SETSEL, 0, 0); pane->insertWorkspaceDocumentLink(targetPath);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000); QVERIFY2(finished.first().first().toBool(), qPrintable(finished.first().at(1).toString()));
        const auto beforeTooLarge = session->snapshot().utf8;
        QVERIFY(fileFixtures::write(targetPath, bom + QByteArray(FileService::SourceLimit + 1, 'x')));
        finished.clear(); editor->send(SCI_SETSEL, 0, 0); pane->insertWorkspaceDocumentLink(targetPath);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000); QVERIFY(!finished.first().first().toBool());
        QCOMPARE(session->snapshot().utf8, beforeTooLarge);
#endif
    }
    void workspaceDocumentLinkRejectsOutsideAndStaleTargets() {
#if !defined(Q_OS_WIN) && !defined(Q_OS_LINUX) && !defined(Q_OS_MACOS)
        QSKIP("Confined workspace resources are not available on this platform");
#else
        QTemporaryDir dir, outside; QVERIFY(dir.isValid() && outside.isValid());
        const auto documentPath = dir.filePath("doc.md"), targetPath = dir.filePath("target.md"), outsidePath = outside.filePath("outside.md");
        QVERIFY(fileFixtures::write(documentPath, "source")); QVERIFY(fileFixtures::write(targetPath, "target")); QVERIFY(fileFixtures::write(outsidePath, "outside"));
        FileApplication desktop; auto *workspace = desktop.workspaceWindow(dir.path()); desktop.openInWorkspace(documentPath, workspace);
        auto *pane = waitPane(workspace, documentPath); QVERIFY(pane); pane->setEditing(true); auto *editor = pane->editor(); auto *session = pane->document()->session();
        QSignalSpy finished(pane, &SingleFileWindow::workspaceLinkFinished);
        pane->insertWorkspaceDocumentLink(outsidePath); QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000); QVERIFY(!finished.first().first().toBool());
        QCOMPARE(session->snapshot().utf8, QByteArray("source")); QCOMPARE(disk(outsidePath), QByteArray("outside"));
        const auto invalidPath = dir.filePath("invalid.md"); QVERIFY(fileFixtures::write(invalidPath, QByteArray::fromHex("fffe")));
        finished.clear(); pane->insertWorkspaceDocumentLink(invalidPath); QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000);
        QVERIFY(!finished.first().first().toBool()); QVERIFY(finished.first().at(1).toString().contains("UTF-8"));
        QCOMPARE(session->snapshot().utf8, QByteArray("source"));
#ifdef Q_OS_WIN
        fileFixtures::Junction junction; QVERIFY(junction.create(dir.filePath("linked"), outside.path()));
#else
        QVERIFY(::symlink(outside.path().toUtf8().constData(), dir.filePath("linked").toUtf8().constData()) == 0);
#endif
        finished.clear(); pane->insertWorkspaceDocumentLink(dir.filePath("linked/outside.md")); QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000);
        QVERIFY(!finished.first().first().toBool()); QCOMPARE(session->snapshot().utf8, QByteArray("source"));
        finished.clear(); editor->send(SCI_SETSEL, 0, 0); pane->insertWorkspaceDocumentLink(targetPath); editor->send(SCI_SETSEL, 1, 1);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000); QVERIFY(!finished.first().first().toBool()); QCOMPARE(session->snapshot().utf8, QByteArray("source"));
        finished.clear(); editor->send(SCI_SETSEL, 0, 0); pane->insertWorkspaceDocumentLink(targetPath);
        QCOMPARE(session->replace(0, 0, "changed ", session->revision()), EditResult::Applied);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000); QVERIFY(!finished.first().first().toBool()); QCOMPARE(session->snapshot().utf8, QByteArray("changed source"));
        QCOMPARE(disk(documentPath), QByteArray("source")); QCOMPARE(disk(targetPath), QByteArray("target"));
        const auto maximum = QString(FileService::SourceLimit, QLatin1Char('x'));
        QCOMPARE(session->replace(0, 14, maximum, session->revision()), EditResult::Applied);
        finished.clear(); editor->send(SCI_SETSEL, 0, 0); pane->insertWorkspaceDocumentLink(targetPath);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000); QVERIFY(!finished.first().first().toBool());
        QVERIFY(finished.first().at(1).toString().contains(QStringLiteral("超过")));
        QCOMPARE(session->snapshot().utf8.size(), FileService::SourceLimit);
        QCOMPARE(disk(documentPath), QByteArray("source"));
#endif
    }
    void imageImportDoesNotInsertIntoAChangedSession() {
#ifndef Q_OS_WIN
        QSKIP("Safe image publication is currently implemented on Windows");
#else
        QTemporaryDir dir, outside; QVERIFY(dir.isValid() && outside.isValid());
        const auto documentPath = dir.filePath("doc.md"), imagePath = outside.filePath("late.png");
        QVERIFY(fileFixtures::write(documentPath, "old")); QImage image(2, 2, QImage::Format_ARGB32); image.fill(Qt::red);
        QByteArray png; QBuffer buffer(&png); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&buffer, "PNG")); QVERIFY(fileFixtures::write(imagePath, png));
        FileApplication desktop; auto *workspace = desktop.workspaceWindow(dir.path()); desktop.openInWorkspace(documentPath, workspace);
        auto *pane = waitPane(workspace, documentPath); QVERIFY(pane); pane->setEditing(true); auto *session = pane->document()->session();
        QSignalSpy finished(pane, &SingleFileWindow::imageImportFinished); QSignalSpy cancelled(pane, &SingleFileWindow::closeCancelled);
        pane->importImageFile(imagePath); pane->close(); QCOMPARE(cancelled.size(), 1); QVERIFY(pane->document());
        QCOMPARE(session->replace(0, 0, "changed ", session->revision()), EditResult::Applied);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000); QVERIFY(!finished.first().first().toBool());
        QCOMPARE(session->snapshot().utf8, QByteArray("changed old")); QCOMPARE(disk(dir.filePath("assets/images/late.png")), png);
        QVERIFY(finished.first().at(1).toString().contains("late.png")); QCOMPARE(disk(documentPath), QByteArray("old"));
        finished.clear(); pane->editor()->send(SCI_SETSEL, 0, 0); pane->importImageFile(imagePath);
        pane->editor()->send(SCI_SETSEL, 1, 1);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000); QVERIFY(!finished.first().first().toBool());
        QCOMPARE(session->snapshot().utf8, QByteArray("changed old")); QVERIFY(finished.first().at(1).toString().contains("late.png"));
#endif
    }
    void imageDropAndClipboardUseTheImportService() {
#ifndef Q_OS_WIN
        QSKIP("Safe image publication is currently implemented on Windows");
#else
        // Avoid replacing the user's real clipboard during local native runs.
        if (!qEnvironmentVariableIsSet("CI") && QGuiApplication::platformName() != "offscreen") QSKIP("Clipboard fixture requires CI or the offscreen platform");
        QTemporaryDir dir, outside; QVERIFY(dir.isValid() && outside.isValid());
        const auto documentPath = dir.filePath("doc.md"), imagePath = outside.filePath("dropped.png");
        QVERIFY(fileFixtures::write(documentPath, "")); QImage image(3, 2, QImage::Format_ARGB32); image.fill(Qt::green);
        QByteArray png; QBuffer buffer(&png); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&buffer, "PNG")); QVERIFY(fileFixtures::write(imagePath, png));
        FileApplication desktop; auto *workspace = desktop.workspaceWindow(dir.path()); desktop.openInWorkspace(documentPath, workspace);
        auto *pane = waitPane(workspace, documentPath); QVERIFY(pane); pane->setEditing(true); auto *editor = pane->editor(); auto *session = pane->document()->session();
        QSignalSpy finished(pane, &SingleFileWindow::imageImportFinished);
        QMimeData dropMime; dropMime.setUrls({QUrl::fromLocalFile(imagePath)});
        QDragEnterEvent enter(QPoint(4, 4), Qt::CopyAction, &dropMime, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(editor, &enter); QVERIFY(enter.isAccepted());
        QDropEvent drop(QPointF(4, 4), Qt::CopyAction, &dropMime, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(editor, &drop); QVERIFY(drop.isAccepted());
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000); QVERIFY2(finished.first().first().toBool(), qPrintable(finished.first().at(1).toString()));
        QCOMPARE(disk(dir.filePath("assets/images/dropped.png")), png); QCOMPARE(session->undo(), EditResult::Applied);
        auto *textAndImage = new QMimeData; textAndImage->setText(QStringLiteral("https://example.invalid/image.png")); textAndImage->setImageData(image);
        QApplication::clipboard()->setMimeData(textAndImage); finished.clear(); QTest::keySequence(editor, QKeySequence::Paste);
        QTRY_COMPARE(session->snapshot().utf8, QByteArray("https://example.invalid/image.png")); QVERIFY(finished.isEmpty());
        QCOMPARE(session->undo(), EditResult::Applied);
        auto *bitmap = new QMimeData; bitmap->setImageData(image); QApplication::clipboard()->setMimeData(bitmap);
        QTest::keySequence(editor, QKeySequence::Paste); QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000);
        QVERIFY2(finished.first().first().toBool(), qPrintable(finished.first().at(1).toString()));
        QVERIFY(session->snapshot().utf8.startsWith("![")); QCOMPARE(QDir(dir.filePath("assets/images")).entryList(QDir::Files).size(), 2);
#endif
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
        auto *target = desktop.workspaceWindow(dir.path()); QInputMethodEvent preedit(QStringLiteral("pin"), {}); QApplication::sendEvent(pane->editor(), &preedit); QVERIFY(pane->document()->session()->isComposing()); QVERIFY(!desktop.transferToWorkspace(pane, target)); QVERIFY(pane->isWindow()); QVERIFY(target->panes().isEmpty());
        qInfo() << "transfer-fixture: preedit deferred";
        QInputMethodEvent commit; commit.setCommitString(QStringLiteral("中")); QApplication::sendEvent(pane->editor(), &commit); QTRY_VERIFY(pane->embedded()); QCOMPARE(pane->window(), static_cast<QWidget *>(target)); QVERIFY(pane->document()->session()->snapshot().utf8.contains(QStringLiteral("中").toUtf8()));
        qInfo() << "transfer-fixture: transferred";
        const auto otherPath = dir.filePath("preserve.md"); QVERIFY(fileFixtures::write(otherPath, "preserved")); desktop.open(otherPath); auto *other = waitStandalone(otherPath); QVERIFY(other);
        QTemporaryDir otherRoot; auto *closing = desktop.workspaceWindow(otherRoot.path()); qInfo() << "transfer-fixture: closing destination"; closing->close(); qInfo() << "transfer-fixture: destination closed"; QVERIFY(closing->closingWorkspace()); QVERIFY(!desktop.transferToWorkspace(other, closing)); QVERIFY(other->isWindow()); QCOMPARE(other->document()->session()->snapshot().utf8, QByteArray("preserved"));
        qInfo() << "transfer-fixture: leaving scope";
    }
    void readingPositionSurvivesNativeHostTransfer() {
        QTemporaryDir dir; const auto path = dir.filePath("reader.md"); QByteArray text; for (int i = 0; i < 180; ++i) text += "paragraph\n\n"; QVERIFY(fileFixtures::write(path, text)); FileApplication desktop; desktop.open(path); auto *pane = waitStandalone(path); QVERIFY(pane);
        QTRY_VERIFY(pane->preview()->view()->contentsSize().height() > 1000);
        pane->preview()->restoreReadingPosition({0, 500}); QTRY_VERIFY(pane->preview()->view()->readingPosition().y() >= 490);
        const auto position = pane->preview()->readingPosition(); auto *view = pane->preview()->view(); auto *workspace = desktop.workspaceWindow(dir.path()); QVERIFY(desktop.transferToWorkspace(pane, workspace)); QCOMPARE(pane->preview()->view(), view); QTest::qWait(200); QTRY_VERIFY(qAbs(view->readingPosition().y() - position.y()) <= 2); capture(pane->window(), "transferred-reader"); QCOMPARE(disk(path), text);
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
        QTRY_COMPARE(workspace->tree()->currentIndex().data().toString(), QStringLiteral("写作，从这里开始.md")); QTRY_VERIFY2_WITH_TIMEOUT(pane->preview()->view()->ready(), qPrintable(QString("active=%1 visible=%2 content=%3 pending=%4 running=%5 revision=%6 expected=%7 error=%8")
            .arg(workspace->activePane() == pane).arg(pane->preview()->view()->isVisible()).arg(pane->preview()->view()->content() != nullptr)
            .arg(pane->preview()->view()->statistics().layoutPending).arg(pane->preview()->view()->statistics().layoutRunning)
            .arg(pane->preview()->displayedRevision()).arg(session->revision()).arg(pane->preview()->view()->error())), 15000);
        QVERIFY(pane->preview()->view()->contentsSize().height() > 100); QTest::qWait(250);
        const auto treeImage = workspace->tree()->viewport()->grab().toImage(); const auto treeRatio = treeImage.devicePixelRatio(); const auto selectedRow = workspace->tree()->visualRect(workspace->tree()->currentIndex()); QCOMPARE(treeImage.pixelColor(qRound((workspace->tree()->viewport()->width() - 16) * treeRatio), qRound(selectedRow.center().y() * treeRatio)), pane->appearance()->accentColor("soft"));
        const auto divider = handle->grab().toImage(); const auto ratio = divider.devicePixelRatio(); QCOMPARE(divider.pixelColor(int((handle->width() / 2.0) * ratio), qRound(60 * ratio)), pane->appearance()->color("border")); QCOMPARE(divider.pixelColor(0, qRound(60 * ratio)), pane->appearance()->color("background"));
        const auto original = splitter->sizes(); QTest::mousePress(handle, Qt::LeftButton); QTest::mouseMove(handle, handle->rect().center() + QPoint(40, 0), 30); QTest::mouseRelease(handle, Qt::LeftButton); QVERIFY(splitter->sizes().first() != original.first()); splitter->setSizes(original);
        auto *tabs = workspace->findChild<QTabBar *>("workspaceTabs"); const auto tabImage = tabs->grab().toImage(); const auto active = tabs->tabRect(tabs->currentIndex()), inactive = tabs->tabRect(1 - tabs->currentIndex()); const auto border = pane->appearance()->color("border"), background = pane->appearance()->color("background");
        QVERIFY(hasColorNear(tabImage, QPointF(active.center().x(), 6), border)); QVERIFY(hasColorNear(tabImage, QPointF(active.left() + .5, 24), border)); QVERIFY(hasColorNear(tabImage, QPointF(active.right() - .5, 24), border)); QVERIFY(hasColorNear(tabImage, QPointF(active.center().x(), tabs->height() - 1), background)); QVERIFY(!hasColorNear(tabImage, QPointF(active.center().x(), tabs->height() - 1), border));
        QVERIFY(hasColorNear(tabImage, QPointF(inactive.center().x(), tabs->height() - 1), border)); auto *header = workspace->findChild<QWidget *>("documentHeader"); auto *tabStrip = workspace->findChild<QWidget *>("workspaceTabStrip"); const auto headerImage = header->grab().toImage(); QVERIFY(hasColorNear(headerImage, QPointF(tabStrip->geometry().right() - 12, header->height() - 1), border));
        auto *edge = workspace->findChild<QWidget *>("workspaceSidebarBorder"); QVERIFY(edge); QCOMPARE(edge->height(), workspace->findChild<QWidget *>("workspaceSidebar")->height()); const auto edgeImage = edge->grab().toImage(); for (const int y : {2, edge->height() / 2, edge->height() - 3}) QVERIFY(hasColorNear(edgeImage, QPointF(0, y), border));
        capture(workspace, theme + "-split");
        workspace->findChild<QAction *>("favoriteDocument")->trigger(); QTest::mouseClick(workspace->findChild<SharedButton *>("workspaceRecent"), Qt::LeftButton); capture(workspace, theme + "-recent"); QTest::mouseClick(workspace->findChild<SharedButton *>("workspaceFavorites"), Qt::LeftButton); capture(workspace, theme + "-favorites");
        auto *search = workspace->findChild<QLineEdit *>("workspaceSearch"); search->setText(QStringLiteral("不存在的文件名")); QTRY_COMPARE(workspace->findChild<QLabel *>("navigationEmpty")->text(), QStringLiteral("没有找到匹配的文档")); capture(workspace, theme + "-search-empty"); search->clear(); QVERIFY(!workspace->activePane()); workspace->activatePane(pane->paneId()); QVERIFY(pane->isVisible()); QCOMPARE(pane->document()->session(), session);
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
