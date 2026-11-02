#include "FileManager.h"
#include "SingleFileWindow.h"
#include "SingleInstance.h"
#include "FileApplication.h"
#include "SourceEditor.h"
#include "MarkdownPreview.h"
#include "GuiRuntime.h"
#include "SharedWidgets.h"
#include "file_fixtures.h"
#include <QApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QProcess>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <cstdio>
#include <QSemaphore>
#include <atomic>

using namespace xdnote;
namespace {
QByteArray disk(const QString &path) { QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll(); }
ReadResult load(const QString &path) { const auto file = FileService::probe(path); return file.file ? FileService::read({file.file}) : ReadResult{{}, file.problem}; }
FileSaveRequest saveRequest(const ReadResult &read, const QByteArray &body) {
    FileSaveRequest request; request.baseline = read.baseline; request.path = read.baseline->file()->path();
    request.utf8 = body; request.bom = read.baseline->hasUtf8Bom(); request.documentId = QUuid::createUuid(); request.revision = 1; return request;
}
bool replacePath(const QString &source, const QString &target) {
#ifdef Q_OS_WIN
    // Force a namespace change despite readers holding the old object. Ordinary
    // MoveFileEx replacement can be excluded by the save writer lease.
    HANDLE old = CreateFileW(reinterpret_cast<LPCWSTR>(target.utf16()), DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (old == INVALID_HANDLE_VALUE) return false;
    FILE_DISPOSITION_INFO_EX disposition{FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS};
    const bool removed = SetFileInformationByHandle(old, FileDispositionInfoEx, &disposition, sizeof(disposition));
    CloseHandle(old);
    return removed && MoveFileExW(reinterpret_cast<LPCWSTR>(source.utf16()), reinterpret_cast<LPCWSTR>(target.utf16()), MOVEFILE_WRITE_THROUGH);
#else
    return ::rename(source.toUtf8().constData(), target.toUtf8().constData()) == 0;
#endif
}
void readOnly(const QString &path, bool enabled) {
#ifdef Q_OS_WIN
    const auto name = reinterpret_cast<LPCWSTR>(path.utf16()); const auto flags = GetFileAttributesW(name);
    SetFileAttributesW(name, enabled ? flags | FILE_ATTRIBUTE_READONLY : flags & ~FILE_ATTRIBUTE_READONLY);
#else
    ::chmod(path.toUtf8().constData(), enabled ? 0400 : 0600);
#endif
}
struct RestoreWritable { QString path; ~RestoreWritable() { readOnly(path, false); } };
struct OwnedWindow { QPointer<SingleFileWindow> value; ~OwnedWindow() { if (value) delete value; } };
FileDocument *open(FileManager &manager, const QString &path) {
    QSignalSpy spy(&manager, &FileManager::opened); manager.open(path);
    QElapsedTimer timer; timer.start(); while (spy.isEmpty() && timer.elapsed() < 8000) QTest::qWait(10);
    return spy.isEmpty() ? nullptr : qvariant_cast<FileDocument *>(spy.first().at(1));
}
void capture(QWidget *widget, const QString &name) {
    const auto directory = qEnvironmentVariable("XDNOTE_TEST_EVIDENCE");
    if (!directory.isEmpty()) { QDir().mkpath(directory); widget->grab().save(directory + '/' + name + ".png"); }
}
bool readerHasPainted(MarkdownPreview *preview) {
    // DOM completion can precede Chromium's first composited frame. Check the
    // actual preview pixels before recording visual evidence, not just HTML.
    const QImage pixels = preview->view()->grab().toImage().convertToFormat(QImage::Format_RGB32);
    int ink = 0;
    for (int y = 8; y < pixels.height() - 8; y += 2)
        for (int x = 8; x < pixels.width() - 8; x += 2) {
            const auto pixel = pixels.pixel(x, y);
            if (qRed(pixel) < 100 && qGreen(pixel) < 100 && qBlue(pixel) < 100 && ++ink > 100) return true;
        }
    return false;
}
void chooseDialog(const QString &name, int choice, const QString &screenshot = {}) {
    auto *timer = new QTimer(qApp); timer->setInterval(10);
    auto elapsed = std::make_shared<QElapsedTimer>(); elapsed->start();
    QObject::connect(timer, &QTimer::timeout, timer, [timer, elapsed, name, choice, screenshot] {
        auto *dialog = qobject_cast<QDialog *>(qApp->activeModalWidget());
        if (dialog && dialog->objectName() == name) {
            if (name == "conflictDialog") { QVERIFY(dialog->findChild<QPushButton *>("choice0")->property("primary").toBool()); QVERIFY(!dialog->findChild<QPushButton *>("choice1")->property("primary").toBool()); }
            if (!screenshot.isEmpty()) capture(dialog, screenshot);
            auto *button = dialog->findChild<QPushButton *>(QStringLiteral("choice%1").arg(choice));
            if (button) button->click(); else dialog->reject(); timer->stop(); timer->deleteLater();
        } else if (elapsed->elapsed() > 3000) { if (dialog) dialog->reject(); timer->stop(); timer->deleteLater(); }
    });
    timer->start();
}
}

class FileWorkflowTests final : public QObject {
    Q_OBJECT
private slots:
    void saveRoundTripAndCreateWithoutOverwrite() {
        QTemporaryDir directory; QVERIFY(directory.isValid()); const auto path = directory.filePath(QStringLiteral("真实 文档😀.md"));
        const auto original = QByteArray::fromHex("efbbbf") + QStringLiteral("# 中文😀\r\n正文\n尾\r\u2028\ufeff").toUtf8();
        QVERIFY(fileFixtures::write(path, original)); auto before = load(path); QVERIFY(before.baseline);
        const auto body = before.baseline->utf8() + QStringLiteral("\n新内容").toUtf8(); auto request = saveRequest(before, body);
        auto saved = FileService::save(request);
        QVERIFY2(saved.status == SaveStatus::Committed, qUtf8Printable(QStringLiteral("%1; stage=%2 native=%3 staged=%4")
            .arg(saved.problem.description()).arg(int(saved.stage)).arg(saved.problem.nativeCode).arg(saved.stagedPath)));
        QVERIFY(saved.baseline); QCOMPARE(disk(path), QByteArray::fromHex("efbbbf") + body);
        QCOMPARE(saved.baseline->utf8(), body); QVERIFY(saved.recoveryPath.isEmpty()); QVERIFY(saved.stagedPath.isEmpty());
        QVERIFY(saved.baseline->file()->identity() != before.baseline->file()->identity());
        FileSaveRequest created = request; created.path = directory.filePath("copy.md"); created.baseline.reset(); created.requestId = QUuid::createUuid();
        QCOMPARE(FileService::save(created).status, SaveStatus::Committed);
        QCOMPARE(disk(created.path), disk(path));
        created.utf8 = "must not replace";
        QCOMPARE(FileService::save(created).status, SaveStatus::Conflict); QCOMPARE(disk(created.path), disk(path));
        QCOMPARE(QDir(directory.path()).entryList({".xdnote-*"}, QDir::Files | QDir::Hidden).size(), 0);
    }
    void conflictsFailuresAndHardLinksPreserveOriginal() {
        QTemporaryDir directory; QVERIFY(directory.isValid()); const auto path = directory.filePath("document.md");
        QVERIFY(fileFixtures::write(path, "before")); auto before = load(path); QVERIFY(before.baseline);
        auto request = saveRequest(before, "mine"); QVERIFY(fileFixtures::write(path, "external"));
        QCOMPARE(FileService::save(request).status, SaveStatus::Conflict); QCOMPARE(disk(path), QByteArray("external"));
        QVERIFY(fileFixtures::write(path, "before")); before = load(path); request = saveRequest(before, "mine");
        readOnly(path, true); RestoreWritable restore{path};
        auto denied = FileService::save(request); QCOMPARE(denied.status, SaveStatus::Failed); QCOMPARE(denied.problem.code, FileError::Denied);
        QCOMPARE(disk(path), QByteArray("before")); readOnly(path, false);
        const auto alias = directory.filePath("alias.md"); QVERIFY(fileFixtures::hardLink(path, alias));
        auto linked = FileService::save(request); QCOMPARE(linked.status, SaveStatus::Failed); QCOMPARE(linked.problem.code, FileError::HardLinks);
        QCOMPARE(disk(path), QByteArray("before")); QCOMPARE(disk(alias), QByteArray("before"));
    }
    void lateReplacementRetainsDisplacedVersionAndReportsUncertain() {
        QTemporaryDir directory; QVERIFY(directory.isValid()); const auto path = directory.filePath("document.md"), other = directory.filePath("external.md");
        QVERIFY(fileFixtures::write(path, "baseline")); QVERIFY(fileFixtures::write(other, "external late change"));
        auto before = load(path); QVERIFY(before.baseline); auto request = saveRequest(before, "my changes"); bool replaced = false;
        auto saved = FileService::save(request, [&](SaveStage stage) { if (stage == SaveStage::Commit) replaced = replacePath(other, path); });
        QVERIFY(replaced); QCOMPARE(saved.status, SaveStatus::Uncertain); QVERIFY(!saved.recoveryPath.isEmpty());
        QCOMPARE(disk(saved.recoveryPath), QByteArray("external late change")); QCOMPARE(disk(path), QByteArray("my changes"));
    }
    void changeAfterCommitNeverReportsSaved() {
        QTemporaryDir directory; const auto path = directory.filePath("document.md"); QVERIFY(fileFixtures::write(path, "before"));
        auto before = load(path); QVERIFY(before.baseline); bool written = false;
        auto result = FileService::save(saveRequest(before, "mine"), [&](SaveStage stage) { if (stage == SaveStage::Verify) written = fileFixtures::write(path, "external after commit"); });
        QVERIFY(written); QCOMPARE(result.status, SaveStatus::Uncertain); QCOMPARE(disk(path), QByteArray("external after commit"));
        QCOMPARE(disk(result.recoveryPath), QByteArray("before"));
    }
    void savingWhileEditingAndRepeatedOpenKeepSameSession() {
        QTemporaryDir directory; const auto path = directory.filePath("document.md"); QVERIFY(fileFixtures::write(path, "start"));
        FileManager manager(nullptr, {}, false); auto *document = open(manager, path); QVERIFY(document);
        auto *session = document->session(); const auto id = session->documentId();
        QCOMPARE(session->replace(0, 0, "saved ", session->revision()), EditResult::Applied);
        const auto frozen = session->snapshot(); QSignalSpy saved(document, &FileDocument::saveFinished);
        QVERIFY(document->save()); QVERIFY(document->saving());
        QCOMPARE(session->replace(0, 0, "newer ", session->revision()), EditResult::Applied);
        QSignalSpy reopened(&manager, &FileManager::opened); manager.open(path);
        QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 10000); QCOMPARE(document->lastSave().status, SaveStatus::Committed);
        QCOMPARE(disk(path), frozen.utf8); QVERIFY(document->dirty());
        QTRY_COMPARE_WITH_TIMEOUT(reopened.count(), 1, 8000); QCOMPARE(qvariant_cast<FileDocument *>(reopened.first().at(1)), document);
        QCOMPARE(manager.documentCount(), 1); QCOMPARE(session->documentId(), id);
        QCOMPARE(session->undo(), EditResult::Applied); QVERIFY(!document->dirty());
        const auto copy = directory.filePath("copy.md"); QVERIFY(document->saveAs(copy));
        QTRY_VERIFY_WITH_TIMEOUT(!document->saving(), 8000); QCOMPARE(document->lastSave().status, SaveStatus::Committed);
        QCOMPARE(document->path(), copy); QCOMPARE(session->documentId(), id); QVERIFY(!document->dirty());
        QVERIFY(manager.closeDocument(document)); QTRY_COMPARE(manager.documentCount(), 0);
    }
    void reloadProtectsEditsMadeDuringReadAndRejectsOtherOpenTarget() {
        QTemporaryDir directory; const auto path = directory.filePath("one.md"), target = directory.filePath("two.md");
        QVERIFY(fileFixtures::write(path, "one")); QVERIFY(fileFixtures::write(target, "two"));
        FileManager manager(nullptr, {}, false); auto *document = open(manager, path), *second = open(manager, target); QVERIFY(document && second);
        auto *session = document->session(); QVERIFY(fileFixtures::write(path, "external")); document->checkExternal();
        QTRY_VERIFY(document->conflict()); QVERIFY(document->reload());
        QCOMPARE(session->replace(0, 0, "keep ", session->revision()), EditResult::Applied);
        QTRY_VERIFY(!document->loading()); QVERIFY(document->dirty()); QCOMPARE(session->snapshot().utf8, QByteArray("keep one"));
        QVERIFY(document->saveAs(target)); QTRY_VERIFY(!document->saving()); QVERIFY(document->hasError());
        QCOMPARE(disk(target), QByteArray("two")); QCOMPARE(session->snapshot().utf8, QByteArray("keep one"));
        QVERIFY(document->reload()); QTRY_VERIFY(!document->loading()); QVERIFY(!document->hasError());
        QCOMPARE(session->snapshot().utf8, QByteArray("external")); QVERIFY(!document->dirty());
    }
    void nativeReadOnlyEditorCannotModifySharedDocument() {
        DocumentSession session("unchanged"); SourceEditor view(&session); view.show(); view.setFocus();
        QCOMPARE(session.setReadOnly(true), EditResult::Applied); QTest::keyClicks(&view, "blocked");
        QCOMPARE(session.snapshot().utf8, QByteArray("unchanged"));
        QCOMPARE(session.setReadOnly(false), EditResult::Applied); QTest::keyClicks(&view, "allowed");
        QVERIFY(session.snapshot().utf8 != QByteArray("unchanged"));
    }
    void actualWindowOpenEditSaveAndCloseConfirmation() {
        QTemporaryDir directory; const auto path = directory.filePath(QStringLiteral("写作，从这里开始.md"));
        QVERIFY(fileFixtures::write(path, QStringLiteral("# 写作，从这里开始\n\n中文正文。\n").toUtf8()));
        FileManager manager(nullptr, {}, false); auto *document = open(manager, path); QVERIFY(document);
        OwnedWindow owned{new SingleFileWindow(&manager)}; auto *window = owned.value.data(); window->setDocument(document); window->show();
        QSignalSpy ready(window->preview(), &MarkdownPreview::previewReady); QTRY_VERIFY_WITH_TIMEOUT(ready.count() > 0, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(readerHasPainted(window->preview()), 15000);
        capture(window, "standalone-reading"); QVERIFY(!window->editing());
        auto *edit = window->findChild<QPushButton *>("editMode"); QVERIFY(edit); QTest::mouseClick(edit, Qt::LeftButton); QVERIFY(window->editing());
        window->editor()->setFocus(); window->editor()->send(SCI_GOTOPOS, window->editor()->send(SCI_GETLENGTH)); QTest::keyClicks(window->editor(), "edited");
        QTRY_VERIFY(document->dirty()); QTRY_VERIFY(window->isWindowModified()); capture(window, "standalone-editing"); const auto text = document->session()->snapshot().utf8;
        chooseDialog("unsavedConfirmation", 0, "unsaved-close"); QVERIFY(!window->close()); QVERIFY(owned.value && owned.value->isVisible());
        QCOMPARE(document->session()->snapshot().utf8, text); QVERIFY(document->dirty());
        chooseDialog("unsavedConfirmation", 1); QVERIFY(!window->close());
        QTRY_VERIFY_WITH_TIMEOUT(owned.value.isNull(), 10000); QCOMPARE(disk(path), text); QTRY_COMPARE(manager.documentCount(), 0);
    }
    void discardAndConflictKeepReloadChoices() {
        QTemporaryDir directory; const auto path = directory.filePath("document.md"); QVERIFY(fileFixtures::write(path, "original"));
        FileManager manager(nullptr, {}, false); auto *document = open(manager, path); QVERIFY(document);
        OwnedWindow owned{new SingleFileWindow(&manager)}; auto *window = owned.value.data(); window->setDocument(document); window->show(); window->setEditing(true);
        auto *session = document->session(); QCOMPARE(session->replace(0, 0, "mine ", session->revision()), EditResult::Applied);
        QVERIFY(fileFixtures::write(path, "external")); document->checkExternal(); QTRY_VERIFY(document->conflict());
        chooseDialog("conflictDialog", 0, "external-conflict"); window->resolveConflict(); QCOMPARE(session->snapshot().utf8, QByteArray("mine original"));
        chooseDialog("conflictDialog", 2); chooseDialog("reloadConfirmation", 0); window->resolveConflict();
        QCOMPARE(session->snapshot().utf8, QByteArray("mine original")); QVERIFY(document->dirty());
        chooseDialog("conflictDialog", 2); chooseDialog("reloadConfirmation", 2); window->resolveConflict();
        QTRY_VERIFY(!document->loading()); QCOMPARE(session->snapshot().utf8, QByteArray("external")); QVERIFY(!document->dirty());
        QCOMPARE(session->replace(0, 0, "discard ", session->revision()), EditResult::Applied);
        chooseDialog("unsavedConfirmation", 2); QVERIFY(window->close()); QTRY_VERIFY(owned.value.isNull()); QCOMPARE(disk(path), QByteArray("external"));
    }
    void leadingContentBomAndExplicitRecreateDeletedFile() {
        QTemporaryDir directory; const auto path = directory.filePath("deleted.md"); QVERIFY(fileFixtures::write(path, "original"));
        FileManager manager(nullptr, {}, false); auto *document = open(manager, path); QVERIFY(document);
        auto *session = document->session();
        const auto body = QByteArray::fromHex("efbbbf") + "literal content";
        QCOMPARE(session->replace(0, 8, QString(QChar(0xfeff)) + "literal content", session->revision()), EditResult::Applied);
        QVERIFY(document->save()); QTRY_VERIFY(!document->saving()); QVERIFY(!document->hasError());
        QCOMPARE(disk(path), QByteArray::fromHex("efbbbf") + body); QCOMPARE(load(path).baseline->utf8(), body);
        QVERIFY(QFile::remove(path)); document->checkExternal(); QTRY_VERIFY(document->conflict());
        QVERIFY(!document->save()); QVERIFY(!QFileInfo::exists(path));
        QVERIFY(document->saveAs(path)); QTRY_VERIFY(!document->saving()); QVERIFY(!document->hasError());
        QCOMPARE(load(path).baseline->utf8(), body); QVERIFY(!document->dirty());
    }
    void applicationRoutesDuplicateOpenAndSavesInChildProcess() {
        QTemporaryDir directory; const auto path = directory.filePath("application.md"); QVERIFY(fileFixtures::write(path, "application"));
        QProcess process; process.start(QCoreApplication::applicationFilePath(), {"--window-smoke", path});
        QTRY_VERIFY_WITH_TIMEOUT(process.state() == QProcess::NotRunning, 20000);
        QVERIFY2(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0, qPrintable(QString("exit=%1: %2").arg(process.exitCode()).arg(QString::fromUtf8(process.readAll()))));
        QCOMPARE(disk(path), QByteArray("application[child]"));
    }
    void failedSaveFromCloseRetainsWindowAndCanSaveAs() {
        QTemporaryDir directory; const auto path = directory.filePath("document.md"), copy = directory.filePath("recovered.md");
        QVERIFY(fileFixtures::write(path, "original")); FileManager manager(nullptr, {}, false); auto *document = open(manager, path); QVERIFY(document);
        OwnedWindow owned{new SingleFileWindow(&manager)}; auto *window = owned.value.data(); window->setDocument(document); window->show(); window->setEditing(true);
        auto *session = document->session(); QCOMPARE(session->replace(0, 0, "unsaved ", session->revision()), EditResult::Applied);
        readOnly(path, true); RestoreWritable restore{path}; chooseDialog("unsavedConfirmation", 1); QVERIFY(!window->close());
        QTRY_VERIFY_WITH_TIMEOUT(!document->saving(), 8000); QVERIFY(owned.value && owned.value->isVisible()); QVERIFY(document->dirty()); QVERIFY(document->hasError());
        QCOMPARE(disk(path), QByteArray("original")); capture(window, "save-error");
        QTRY_VERIFY(qApp->activeModalWidget()); auto *error = qobject_cast<QDialog *>(qApp->activeModalWidget()); QVERIFY(error); QCOMPARE(error->objectName(), QString("saveErrorDialog"));
        auto *keep = error->findChild<QPushButton *>("errorKeepEditing"); QVERIFY(keep); QVERIFY(keep->property("primary").toBool()); capture(error, "save-error-dialog"); keep->click(); QTRY_VERIFY(!qApp->activeModalWidget());
        QVERIFY(document->saveAs(copy)); QTRY_VERIFY(!document->saving()); QCOMPARE(document->lastSave().status, SaveStatus::Committed);
        QCOMPARE(disk(copy), QByteArray("unsaved original")); QVERIFY(!document->dirty()); QVERIFY(window->close()); QTRY_VERIFY(owned.value.isNull());
    }
    void savingAnimationAndWaitForCloseRetainDocument() {
        QTemporaryDir directory; const auto path = directory.filePath("busy.md"); QVERIFY(fileFixtures::write(path, "before"));
        QSemaphore gate; std::atomic_bool entered{false};
        FileManager manager(nullptr, [&](SaveStage stage) { if (stage == SaveStage::StageWrite) { entered = true; gate.acquire(); } }, false);
        struct Release { QSemaphore &gate; ~Release() { gate.release(); } } release{gate};
        auto *document = open(manager, path); QVERIFY(document); OwnedWindow owned{new SingleFileWindow(&manager)}; auto *window = owned.value.data(); window->setDocument(document); window->setEditing(true); window->show();
        QCOMPARE(document->session()->replace(0, 0, "changed ", document->session()->revision()), EditResult::Applied); QVERIFY(document->save()); QTRY_VERIFY(entered.load());
        auto *spinner = window->findChild<QWidget *>("documentFooter")->findChild<SharedSpinner *>(); QVERIFY(spinner); QVERIFY(spinner->running()); QVERIFY(spinner->isVisible());
        const auto first = spinner->grab().toImage(); QTest::qWait(120); QVERIFY(first != spinner->grab().toImage()); QVERIFY(!window->findChild<QPushButton *>("saveButton")->isEnabled()); capture(window, "saving-animation");
        bool inspected = false; QTimer timer; timer.setInterval(20); connect(&timer, &QTimer::timeout, this, [&] { auto *dialog = qobject_cast<QDialog *>(qApp->activeModalWidget()); if (!dialog || dialog->objectName() != "operationCloseConfirmation") return; timer.stop(); inspected = true; auto *busy = dialog->findChild<SharedSpinner *>(); QVERIFY(busy && busy->running()); capture(dialog, "waiting-close"); dialog->findChild<QPushButton *>("choice0")->click(); }); timer.start();
        QVERIFY(!window->close()); QVERIFY(inspected); QVERIFY(window->isVisible()); gate.release(); QTRY_VERIFY(!document->saving()); QVERIFY(!document->dirty()); QVERIFY(!spinner->isVisible()); QCOMPARE(disk(path), QByteArray("changed before"));
    }
    void anotherProcessForwardsLiteralOpenIntent() {
        QTemporaryDir directory; QVERIFY(directory.isValid()); SingleInstance primary; QStringList received;
        QCOMPARE(primary.start(directory.path(), {}, [&](const QStringList &paths) { received = paths; }), SingleInstance::Result::Primary);
        const QStringList paths{directory.filePath(QStringLiteral("带空格.md")), directory.filePath("note; literal.md")};
        QProcess process; process.start(QCoreApplication::applicationFilePath(), QStringList{"--instance-client", directory.path()} + paths);
        QTRY_VERIFY_WITH_TIMEOUT(process.state() == QProcess::NotRunning, 10000);
        QCOMPARE(process.exitCode(), 0); QCOMPARE(received, paths);
    }
};

int main(int argc, char **argv) {
    if (argc >= 3 && QByteArray(argv[1]) == "--instance-client") {
        QCoreApplication app(argc, argv); SingleInstance instance; const auto args = app.arguments();
        return instance.start(args[2], args.mid(3), [](QStringList) {}) == SingleInstance::Result::Forwarded ? 0 : 3;
    }
    initializeGuiRuntime(); QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false); configureGuiApplication(); SourceEditor::installAccessibility();
    if (app.arguments().size() == 3 && app.arguments()[1] == "--window-smoke") {
        FileApplication application; const auto path = app.arguments()[2]; application.open(path);
        int phase = 0; QUuid id; QTimer timer;
        QObject::connect(&timer, &QTimer::timeout, &application, [&] {
            QList<SingleFileWindow *> windows;
            for (auto *widget : app.topLevelWidgets()) if (auto *window = qobject_cast<SingleFileWindow *>(widget)) windows.append(window);
            if (windows.size() != 1 || !windows.first()->document()) return;
            auto *window = windows.first(); auto *document = window->document();
            if (phase == 0) { id = document->session()->documentId(); phase = 1; application.open(path); return; }
            if (phase == 1) {
                if (document->session()->documentId() != id) { app.exit(4); return; }
                window->setEditing(true); auto *editor = window->editor(); editor->send(SCI_GOTOPOS, editor->send(SCI_GETLENGTH)); QTest::keyClicks(editor, "[child]");
                phase = 3; return;
            }
            if (phase == 3) {
                auto *save = window->findChild<QPushButton *>("saveButton");
                if (!document->dirty() || !save->isEnabled()) return;
                save->click(); phase = 2; return;
            }
            if (phase == 2 && !document->saving()) {
                if (document->dirty() || document->hasError() || disk(path) != "application[child]") { app.exit(5); return; }
                timer.stop(); window->close();
            }
        });
        timer.start(30); QTimer::singleShot(15000, &app, [&] { app.exit(6); }); return app.exec();
    }
    FileWorkflowTests tests; return QTest::qExec(&tests, argc, argv);
}
#include "file_workflow_tests.moc"
