#include "AdmissionWindow.h"
#include "DocumentSession.h"
#include "MarkdownPreview.h"
#include "MarkdownReferences.h"
#include "MarkdownHeadings.h"
#include "WorkspacePath.h"
#include "WorkspaceResourceReader.h"
#include "WorkspaceImagePreview.h"
#include "ImageImport.h"
#include "SourceEditor.h"
#include "VisualTokens.h"
#include "GuiRuntime.h"
#include "FileService.h"
#include "SessionRegistry.h"
#include "file_fixtures.h"

#include <QAccessible>
#include <QApplication>
#include <QCryptographicHash>
#include <QBuffer>
#include <QElapsedTimer>
#include <QInputMethodEvent>
#include <QProcess>
#include <QFontDatabase>
#include <QGlyphRun>
#include <QTextLayout>
#include <QTemporaryDir>
#include <QSemaphore>
#include <QTcpServer>
#include <QHostAddress>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWidget>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QSignalBlocker>
#include <QTest>
#include <QWebEnginePage>
#include <QWebEngineScript>
#include <QWebEngineSettings>
#include <QWebEngineView>
#include <future>
#include <atomic>

using namespace xdnote;

namespace {
QVariant evaluatePreview(QWebEnginePage *page, const QString &script) {
    struct Result { QVariant value; bool ready = false; };
    const auto result = std::make_shared<Result>();
    page->runJavaScript(script, QWebEngineScript::ApplicationWorld, [result](const QVariant &value) { result->value = value; result->ready = true; });
    QElapsedTimer timer; timer.start();
    while (!result->ready && timer.elapsed() < 10000) QTest::qWait(10);
    return result->value;
}
}

class DesktopTests final : public QObject {
    Q_OBJECT
private slots:
    void resourcePreviewAcceptsMaximumDocumentWithOptionalBom_data() {
        QTest::addColumn<bool>("bom"); QTest::newRow("without-bom") << false; QTest::newRow("with-bom") << true;
    }
    void resourcePreviewAcceptsMaximumDocumentWithOptionalBom() {
#if !defined(Q_OS_WIN) && !defined(Q_OS_LINUX)
        QSKIP("Confined resource reads are unavailable on this platform");
#else
        QFETCH(bool, bom); QTemporaryDir dir; QVERIFY(dir.isValid());
        QByteArray source("![x](a.png)\n"); source.append(QByteArray(FileService::SourceLimit - source.size(), 'x'));
        const auto disk = (bom ? QByteArray::fromHex("efbbbf") : QByteArray()) + source;
        QVERIFY(fileFixtures::write(dir.filePath("doc.md"), disk));
        QImage image(2, 2, QImage::Format_ARGB32); image.fill(Qt::red); QByteArray png; QBuffer buffer(&png);
        QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&buffer, "PNG")); QVERIFY(fileFixtures::write(dir.filePath("a.png"), png));
        const auto file = FileService::probe(dir.filePath("doc.md")); QVERIFY(file.file);
        const auto baseline = FileService::read({file.file}); QVERIFY2(baseline.baseline, qPrintable(baseline.problem.description()));
        QCOMPARE(baseline.baseline->utf8(), source); QCOMPARE(baseline.baseline->hasUtf8Bom(), bom);
        const auto root = WorkspaceResourceReader::open(dir.path()); QVERIFY(root.reader);
#ifdef Q_OS_LINUX
        if (root.reader->read("a.png", 1024).error == WorkspaceReadError::Unsupported) QSKIP("Kernel does not offer confined openat2");
#endif
        DocumentSession session(source); const auto prepared = preparePreviewImages(session.snapshot(), root.reader, "doc.md", file.file->identity());
        QVERIFY(prepared.analyzed); QCOMPARE(prepared.states.size(), 1); QCOMPARE(int(prepared.states[0]), 1); QVERIFY(prepared.png.contains(0));
#endif
    }
    void resourcePublicationAcceptsMaximumDocumentWithOptionalBom_data() {
        QTest::addColumn<bool>("bom"); QTest::newRow("without-bom") << false; QTest::newRow("with-bom") << true;
    }
    void resourcePublicationAcceptsMaximumDocumentWithOptionalBom() {
#ifndef Q_OS_WIN
        QSKIP("Image publication is currently implemented on Windows");
#else
        QFETCH(bool, bom); QTemporaryDir dir; QVERIFY(dir.isValid());
        const QByteArray source(FileService::SourceLimit, 'x');
        QVERIFY(fileFixtures::write(dir.filePath("doc.md"), (bom ? QByteArray::fromHex("efbbbf") : QByteArray()) + source));
        const auto file = FileService::probe(dir.filePath("doc.md")); QVERIFY(file.file);
        const auto baseline = FileService::read({file.file}); QVERIFY2(baseline.baseline, qPrintable(baseline.problem.description()));
        const auto root = WorkspaceResourceReader::open(dir.path()); QVERIFY(root.reader);
        auto written = root.reader->publishImage("doc.md", file.file->identity(), "image bytes", "image.png", "png");
        QVERIFY2(written.status == WorkspaceWriteStatus::Committed, qPrintable(QStringLiteral("error=%1").arg(int(written.error))));
#endif
    }
    void fileReadStillRejectsContentAboveLimit() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        for (const bool bom : {false, true}) {
            const auto path = dir.filePath(bom ? "bom.md" : "plain.md");
            const auto disk = (bom ? QByteArray::fromHex("efbbbf") : QByteArray()) + QByteArray(FileService::SourceLimit + 1, 'x');
            QVERIFY(fileFixtures::write(path, disk)); const auto file = FileService::probe(path); QVERIFY(file.file);
            const auto loaded = FileService::read({file.file}); QVERIFY(!loaded.baseline); QCOMPARE(loaded.problem.code, FileError::TooLarge);
        }
    }
    void quickHostAccessibilityNavigationKeepsParentsAndActions() {
        DocumentSession session("# fixture"); AdmissionWindow window(&session); window.show(); QTRY_VERIFY(window.isVisible());
        auto *quick = window.findChild<QQuickWidget *>("sharedChrome"); QVERIFY(quick); QCOMPARE(quick->status(), QQuickWidget::Ready);
        auto *host = QAccessible::queryAccessibleInterface(quick); QVERIFY(host); QCOMPARE(host->childCount(), 1);
        auto *group = host->child(0); QVERIFY(group); QVERIFY(group->isValid()); QCOMPARE(group->parent(), host); QCOMPARE(host->indexOfChild(group), 0);
        QCOMPARE(group->role(), QAccessible::Grouping); QCOMPARE(group->childCount(), 3);
        const QStringList names{QStringLiteral("撤销"), QStringLiteral("重做"), QStringLiteral("设为只读")};
        for (int index = 0; index < names.size(); ++index) {
            auto *button = group->child(index); QVERIFY(button); QCOMPARE(button->text(QAccessible::Name), names[index]);
            QCOMPARE(button->parent(), group); QCOMPARE(group->indexOfChild(button), index); QVERIFY(button->actionInterface());
        }
        auto *toggle = group->child(2)->actionInterface(); QVERIFY(toggle->actionNames().contains(QAccessibleActionInterface::pressAction()));
        toggle->doAction(QAccessibleActionInterface::pressAction()); QTRY_VERIFY(session.isReadOnly());
        toggle->doAction(QAccessibleActionInterface::pressAction()); QTRY_VERIFY(!session.isReadOnly());
    }
    void quickHostAccessibilityLifetimeIsBoundToItsWindow() {
        DocumentSession first("first"), second("second");
        auto *one = new AdmissionWindow(&first); auto *two = new AdmissionWindow(&second); one->show(); two->show();
        auto *firstHost = one->findChild<QQuickWidget *>("sharedChrome"), *secondHost = two->findChild<QQuickWidget *>("sharedChrome");
        QVERIFY(firstHost && secondHost); auto *firstInterface = QAccessible::queryAccessibleInterface(firstHost); auto *secondInterface = QAccessible::queryAccessibleInterface(secondHost);
        QVERIFY(firstInterface && secondInterface); auto *firstRoot = firstInterface->child(0), *secondRoot = secondInterface->child(0); QVERIFY(firstRoot && secondRoot);
        QVERIFY(firstRoot != secondRoot); QCOMPARE(firstRoot->parent(), firstInterface); QCOMPARE(secondRoot->parent(), secondInterface);
        const auto firstId = QAccessible::uniqueId(firstRoot), secondId = QAccessible::uniqueId(secondRoot); QVERIFY(firstId != secondId);
        delete one; QVERIFY(!QAccessible::accessibleInterface(firstId)); QVERIFY(QAccessible::accessibleInterface(secondId));
        QCOMPARE(secondRoot->parent(), secondInterface); QCOMPARE(secondRoot->childCount(), 3);
        delete two; QVERIFY(!QAccessible::accessibleInterface(secondId));
    }
    void quickHostExposesWindowsUiaTreeAndCommands() {
#ifndef Q_OS_WIN
        QSKIP("This test exercises the Windows UI Automation bridge");
#else
        DocumentSession session("# fixture"); AdmissionWindow window(&session); window.show(); QTRY_VERIFY(window.isVisible());
        auto *quick = window.findChild<QQuickWidget *>("sharedChrome"); QVERIFY(quick); QTRY_COMPARE(quick->status(), QQuickWidget::Ready);
        const auto script = QCoreApplication::applicationDirPath() + "/windows_uia_client.ps1"; QVERIFY(QFileInfo::exists(script));
        QProcess client; client.setProcessChannelMode(QProcess::MergedChannels);
        client.start(QStringLiteral("powershell.exe"), {QStringLiteral("-NoProfile"), QStringLiteral("-STA"), QStringLiteral("-NonInteractive"),
            QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"), QStringLiteral("-File"), script, QString::number(quintptr(window.winId()))});
        QVERIFY(client.waitForStarted(5000));
        QElapsedTimer deadline; deadline.start();
        while (client.state() != QProcess::NotRunning && deadline.elapsed() < 50000) QTest::qWait(50);
        if (client.state() != QProcess::NotRunning) { client.kill(); client.waitForFinished(5000); QFAIL("UI Automation client did not finish"); }
        const auto output = client.readAll(); qInfo().noquote() << output; QVERIFY2(client.exitStatus() == QProcess::NormalExit && client.exitCode() == 0, output.constData());
        QCOMPARE(session.snapshot().utf8, QByteArray("UIA fixture updated")); QVERIFY(!session.isReadOnly());
#endif
    }
    void headingMapKeepsSnapshotIdentityAndUtf8Ranges() {
        const auto source = QStringLiteral("# 中文 **加粗**\n# 中文 加粗\n#\n").toUtf8();
        DocumentSession session(source); const auto snapshot = session.snapshot();
        const auto result = analyzeMarkdownHeadings(snapshot);
        QVERIFY2(result.success, qPrintable(result.reason)); QCOMPARE(result.documentId, snapshot.documentId); QCOMPARE(result.revision, snapshot.revision);
        QCOMPARE(result.headings.size(), 3); QCOMPARE(result.headings[0].title, QStringLiteral("中文 加粗"));
        QCOMPARE(result.headings[0].id, QStringLiteral("中文-加粗")); QCOMPARE(result.headings[1].id, QStringLiteral("中文-加粗-1"));
        QCOMPARE(result.headings[2].id, QStringLiteral("section"));
        for (const auto &heading : result.headings) { QCOMPARE(heading.level, 1); QVERIFY(source.mid(heading.range.start, heading.range.end - heading.range.start).startsWith('#')); }
        DocumentSnapshot invalid; invalid.result = EditResult::Composing;
        QVERIFY(!analyzeMarkdownHeadings(invalid).success);
        MarkdownPreview preview(&session); QSignalSpy ready(&preview, &MarkdownPreview::previewReady);
        preview.resize(600, 400); preview.show(); QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 30000);
        QString html; bool received = false;
        preview.view()->page()->toHtml([&](const QString &value) { html = value; received = true; });
        QTRY_VERIFY_WITH_TIMEOUT(received, 10000);
        for (const auto &heading : result.headings) QVERIFY2(html.contains(QStringLiteral("id=\"%1\"").arg(heading.id)), qPrintable(heading.id));
    }
    void previewFragmentLinksNavigateOnlyTheDisplayedRevision() {
        const auto source = (QStringLiteral("[中文](#%E4%B8%AD%E6%96%87) ![图](missing.png) [重复](#重复-1) [缺失](#missing) [坏](#%GG) [外部](https://bad.invalid) [引用][r]\n\n[r]: #中文\n\n")
            + QStringLiteral("A paragraph for scrolling.\n\n").repeated(45) + QStringLiteral("# 中文\n\n")
            + QStringLiteral("More content.\n\n").repeated(35) + QStringLiteral("# 重复\n\n")
            + QStringLiteral("More content.\n\n").repeated(35) + QStringLiteral("# 重复\n")).toUtf8();
        DocumentSession session(source); MarkdownPreview preview(&session); preview.resize(640, 320);
        QSignalSpy ready(&preview, &MarkdownPreview::previewReady), moved(&preview, &MarkdownPreview::fragmentNavigated);
        QSignalSpy messages(&preview, &MarkdownPreview::fragmentNavigationMessage);
        preview.show(); QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 30000);
        auto *page = preview.view()->page();
        QCOMPARE(evaluatePreview(page, QStringLiteral("document.querySelectorAll('a.link-placeholder').length")).toInt(), 5);
        QCOMPARE(evaluatePreview(page, QStringLiteral("document.querySelectorAll('span.link-placeholder').length")).toInt(), 1);
        QString html; bool htmlReady = false; page->toHtml([&](const QString &value) { html = value; htmlReady = true; });
        QTRY_VERIFY_WITH_TIMEOUT(htmlReady, 10000); QVERIFY(!html.contains("https://bad.invalid"));
        const auto pageUrl = page->url();
        const auto actionUrl = evaluatePreview(page, QStringLiteral("document.querySelector('a.link-placeholder').href")).toString();
        QVERIFY(actionUrl.startsWith(QStringLiteral("xdnote-preview:")));
        page->load(QUrl(actionUrl)); QTest::qWait(300);
        QCOMPARE(page->url(), pageUrl); QVERIFY(moved.isEmpty());
        const auto click = [&](int index) {
            const auto point = evaluatePreview(page, QStringLiteral("(()=>{const a=document.querySelectorAll('a.link-placeholder')[%1];if(!a)return null;const r=a.getBoundingClientRect();return {x:r.x+r.width/2,y:r.y+r.height/2}})()").arg(index)).toMap();
            if (point.isEmpty()) return false;
            QTest::mouseClick(preview.view()->focusProxy() ? preview.view()->focusProxy() : preview.view(), Qt::LeftButton, Qt::NoModifier,
                QPoint(qRound(point.value("x").toDouble()), qRound(point.value("y").toDouble())));
            return true;
        };
        QVERIFY(click(0)); QTRY_COMPARE_WITH_TIMEOUT(moved.size(), 1, 10000); QCOMPARE(moved.first().first().toString(), QStringLiteral("中文"));
        QVERIFY(!evaluatePreview(page, QStringLiteral("document.activeElement.classList.contains('xdnote-keyboard-focus')")).toBool());
        QTRY_VERIFY_WITH_TIMEOUT(evaluatePreview(page, QStringLiteral("window.scrollY")).toDouble() > 0, 10000);
        preview.restoreReadingPosition(QPointF()); QTRY_COMPARE_WITH_TIMEOUT(evaluatePreview(page, QStringLiteral("window.scrollY")).toInt(), 0, 10000);
        QVERIFY(click(1)); QTRY_COMPARE_WITH_TIMEOUT(moved.size(), 2, 10000); QCOMPARE(moved.last().first().toString(), QStringLiteral("重复-1"));
        preview.restoreReadingPosition(QPointF()); QTRY_COMPARE_WITH_TIMEOUT(evaluatePreview(page, QStringLiteral("window.scrollY")).toInt(), 0, 10000);
        messages.clear(); QVERIFY(click(2)); QTRY_VERIFY_WITH_TIMEOUT(!messages.isEmpty(), 10000);
        QVERIFY(messages.last().first().toString().contains(QStringLiteral("未找到标题"))); QCOMPARE(moved.size(), 2);
        messages.clear(); QVERIFY(click(3)); QTRY_VERIFY_WITH_TIMEOUT(!messages.isEmpty(), 10000);
        QVERIFY(messages.last().first().toString().contains(QStringLiteral("无效"))); QCOMPARE(moved.size(), 2);
        QVERIFY(click(4)); QTRY_COMPARE_WITH_TIMEOUT(moved.size(), 3, 10000); QCOMPARE(moved.last().first().toString(), QStringLiteral("中文"));
        preview.restoreReadingPosition(QPointF()); QTRY_COMPARE_WITH_TIMEOUT(evaluatePreview(page, QStringLiteral("window.scrollY")).toInt(), 0, 10000);
        QCOMPARE(session.snapshot().utf8, source); QVERIFY(!session.isDirty());
        {
            const QSignalBlocker block(&session);
            QCOMPARE(session.replace(0, 0, QStringLiteral("changed "), session.revision()), EditResult::Applied);
        }
        QVERIFY(click(0)); QTest::qWait(300); QCOMPARE(moved.size(), 3);
        QCOMPARE(preview.displayedRevision() + 1, session.revision());
    }
    void previewDoesNotActivateLinksWhenReferenceAnalysisMayBeIncomplete() {
        const auto source = QByteArray("[r] ").repeated(99) + "\n\n[r]: #" + QByteArray(999, 'a') + "\n\n# heading\n";
        DocumentSession session(source); const auto analysis = analyzeMarkdownReferences(session.snapshot());
        QVERIFY(analysis.success); QVERIFY(!analysis.complete);
        MarkdownPreview preview(&session); QSignalSpy ready(&preview, &MarkdownPreview::previewReady);
        preview.resize(640, 320); preview.show(); QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 30000);
        QCOMPARE(evaluatePreview(preview.view()->page(), QStringLiteral("document.querySelectorAll('a.link-placeholder').length")).toInt(), 0);
    }
    void previewFragmentLinksSupportKeyboardFocusAndActivation() {
        const auto source = (QStringLiteral("[跳转](#target)\n\n") + QStringLiteral("Text for scrolling.\n\n").repeated(50) + QStringLiteral("# target\n")).toUtf8();
        DocumentSession session(source); MarkdownPreview preview(&session); preview.resize(640, 320);
        QSignalSpy ready(&preview, &MarkdownPreview::previewReady), moved(&preview, &MarkdownPreview::fragmentNavigated);
        preview.show(); QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 30000);
        auto *page = preview.view()->page(); auto *surface = preview.view()->focusProxy() ? preview.view()->focusProxy() : preview.view();
        surface->setFocus(Qt::OtherFocusReason); QTest::keyClick(surface, Qt::Key_Tab);
        QTRY_VERIFY_WITH_TIMEOUT(evaluatePreview(page, QStringLiteral("document.activeElement === document.querySelector('a.link-placeholder')")).toBool(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(evaluatePreview(page, QStringLiteral("document.activeElement.classList.contains('xdnote-keyboard-focus')")).toBool(), 10000);
        QTest::keyClick(surface, Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(moved.size(), 1, 10000); QCOMPARE(moved.first().first().toString(), QStringLiteral("target"));
        QVERIFY(!evaluatePreview(page, QStringLiteral("document.activeElement.classList.contains('xdnote-keyboard-focus')")).toBool());
        QCOMPARE(session.snapshot().utf8, source); QVERIFY(!session.isDirty());
    }
    void markdownReferencesKeepSnapshotIdentityAndIncompleteStatus() {
        const auto source = QStringLiteral("中文😀 ![图](../assets/a%23b.png) [甲][ref] [乙][ref]\n\n[ref]: notes/说明.md\n\n`[伪](ignored.md)`\n").toUtf8();
        DocumentSession session(source); const auto snapshot = session.snapshot();
        const auto result = analyzeMarkdownReferences(snapshot); QVERIFY2(result.success, qPrintable(result.reason)); QVERIFY(result.complete);
        QCOMPARE(result.documentId, snapshot.documentId); QCOMPARE(result.revision, snapshot.revision); QCOMPARE(result.references.size(), 3);
        QCOMPARE(result.references[0].kind, MarkdownReference::Kind::Image); QCOMPARE(result.references[0].target, QStringLiteral("../assets/a%23b.png"));
        QCOMPARE(source.mid(result.references[0].usage.start, result.references[0].usage.end - result.references[0].usage.start), QByteArray("![图](../assets/a%23b.png)"));
        QVERIFY(result.references[1].referenceStyle && result.references[2].referenceStyle); QVERIFY(result.references[1].definition.valid());
        QCOMPARE(result.references[1].definition.start, result.references[2].definition.start); QCOMPARE(result.references[1].target, QStringLiteral("notes/说明.md"));
        QCOMPARE(session.replace(0, 0, QStringLiteral("修改"), snapshot.revision), EditResult::Applied); QVERIFY(session.revision() != result.revision);
        DocumentSnapshot bad{EditResult::Applied, QByteArray::fromHex("ff"), 42, QUuid::createUuid()}; const auto invalid = analyzeMarkdownReferences(bad); QVERIFY(!invalid.success); QVERIFY(invalid.references.isEmpty());
        const auto longSource = QByteArray("[x][r] ").repeated(6000) + "\n\n[r]: " + QByteArray(4096, 'a') + '\n';
        DocumentSnapshot large{EditResult::Applied, longSource, 7, QUuid::createUuid()}; const auto partial = analyzeMarkdownReferences(large);
        QVERIFY2(partial.success, qPrintable(partial.reason)); QVERIFY(!partial.complete); QVERIFY(!partial.references.isEmpty());
    }

    void workspacePathsRoundTripWithoutGrantingFileAccess() {
        const auto url = relativeWorkspaceUrl(QStringLiteral("notes/子目录/方案.md"), QStringLiteral("assets/images/猫 #1%.png"));
        QVERIFY(url.success); QVERIFY(url.path.startsWith("../../assets/images/")); QVERIFY(!url.path.contains('#')); QVERIFY(!url.path.contains(' '));
        const auto parsed = resolveWorkspaceTarget(QStringLiteral("notes/子目录/方案.md"), url.path + "#%E6%A0%87%E9%A2%98");
        QVERIFY(parsed.success); QCOMPARE(parsed.path, QStringLiteral("assets/images/猫 #1%.png")); QVERIFY(parsed.hasFragment); QCOMPARE(parsed.fragment, QStringLiteral("标题"));
        const auto escaped = resolveWorkspaceTarget(QStringLiteral("notes/a.md"), QStringLiteral("%2e%2e/%2e%2e/secret"));
        QVERIFY(!escaped.success); QCOMPARE(escaped.error, QStringLiteral("escapes-workspace"));
        const auto doubleEncoded = resolveWorkspaceTarget(QStringLiteral("notes/a.md"), QStringLiteral("x%252Fy.png"));
        QVERIFY(doubleEncoded.success); QCOMPARE(doubleEncoded.path, QStringLiteral("notes/x%2Fy.png"));
        const auto external = resolveWorkspaceTarget(QStringLiteral("notes/a.md"), QStringLiteral("file:///private/data")); QVERIFY(!external.success);
    }

    void workspaceResourcesRequireTheSelectedRootAndRejectLinks() {
        QTemporaryDir temp; QVERIFY(temp.isValid());
        const auto root = temp.filePath("root"), outside = temp.filePath("outside");
        QVERIFY(QDir().mkpath(root + "/assets")); QVERIFY(QDir().mkpath(outside));
        QVERIFY(fileFixtures::write(root + QStringLiteral("/assets/图片.png"), "image-bytes"));
        QVERIFY(fileFixtures::write(outside + "/private.png", "secret"));
        auto opened = WorkspaceResourceReader::open(root);
#if !defined(Q_OS_WIN) && !defined(Q_OS_LINUX)
        QCOMPARE(opened.error, WorkspaceReadError::Unsupported); QVERIFY(!opened.reader); return;
#else
        QVERIFY(opened.reader); QCOMPARE(opened.error, WorkspaceReadError::None);
        const auto image = opened.reader->read(QStringLiteral("assets/图片.png"), 64);
#ifdef Q_OS_LINUX
        if (image.error == WorkspaceReadError::Unsupported) QSKIP("Kernel does not offer confined openat2");
#endif
        QVERIFY(image.ok()); QCOMPARE(image.bytes, QByteArray("image-bytes"));
        {
            const auto fileProbe = FileService::probe(root + QStringLiteral("/assets/图片.png"));
            QVERIFY(fileProbe.file); QCOMPARE(image.identity, fileProbe.file->identity());
        }
        QCOMPARE(opened.reader->read(QStringLiteral("assets/图片.png"), 2).error, WorkspaceReadError::TooLarge);
        QCOMPARE(opened.reader->read("../outside/private.png", 64).error, WorkspaceReadError::InvalidPath);
        QCOMPARE(opened.reader->read("assets/../../outside/private.png", 64).error, WorkspaceReadError::InvalidPath);
        QCOMPARE(opened.reader->read("/outside/private.png", 64).error, WorkspaceReadError::InvalidPath);
        QCOMPARE(opened.reader->read("assets\\private.png", 64).error, WorkspaceReadError::InvalidPath);
        QCOMPARE(opened.reader->read("missing.png", 64).error, WorkspaceReadError::Missing);
        {
#ifdef Q_OS_WIN
        QCOMPARE(opened.reader->read("assets/图片.png:stream", 64).error, WorkspaceReadError::InvalidPath);
        fileFixtures::Junction junction;
        QVERIFY(junction.create(root + "/escape", outside));
#else
        QVERIFY(::symlink(QFile::encodeName(outside).constData(), QFile::encodeName(root + "/escape").constData()) == 0);
        QVERIFY(::symlink(QFile::encodeName(outside + "/private.png").constData(), QFile::encodeName(root + "/assets/link.png").constData()) == 0);
        QVERIFY(!opened.reader->read("assets/link.png", 64).ok());
        QVERIFY(::mkfifo(QFile::encodeName(root + "/assets/pipe").constData(), 0600) == 0);
        QVERIFY(!opened.reader->read("assets/pipe", 64).ok());
#endif
        const auto escaped = opened.reader->read("escape/private.png", 64);
        QVERIFY(!escaped.ok()); QVERIFY(escaped.bytes.isEmpty());
        }
        // A different directory at the same path cannot inherit the prior
        // authorization, even if it contains a similarly named file.
        QVERIFY(QDir().rename(root, root + "-old")); QVERIFY(QDir().mkpath(root + "/assets"));
        QVERIFY(fileFixtures::write(root + QStringLiteral("/assets/图片.png"), "replacement"));
        QCOMPARE(opened.reader->read(QStringLiteral("assets/图片.png"), 64).error, WorkspaceReadError::RootChanged);
        opened.reader->revoke();
        QCOMPARE(opened.reader->read(QStringLiteral("assets/图片.png"), 64).error, WorkspaceReadError::Revoked);
#endif
    }

    void workspaceImagePublicationNeverOverwritesOrReusesDifferentBytes() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto documentPath = dir.filePath("doc.md"); QVERIFY(fileFixtures::write(documentPath, "document"));
        const auto file = FileService::probe(documentPath); QVERIFY(file.file);
        auto root = WorkspaceResourceReader::open(dir.path());
#if !defined(Q_OS_WIN)
#ifdef Q_OS_LINUX
        QVERIFY(root.reader);
        QCOMPARE(root.reader->publishImage("doc.md", file.file->identity(), "bytes", "a.png", "png").error, WorkspaceReadError::Unsupported);
#else
        QCOMPARE(root.error, WorkspaceReadError::Unsupported);
#endif
        QVERIFY(!QFileInfo::exists(dir.filePath("assets"))); return;
#else
        QVERIFY(root.reader);
        const QByteArray firstBytes("original-image-bytes"), secondBytes("different-image-bytes");
        const auto invalid = root.reader->publishImage("doc.md", "wrong-id", firstBytes, "图.png", "png");
        QCOMPARE(invalid.status, WorkspaceWriteStatus::Failed); QVERIFY(!QFileInfo::exists(dir.filePath("assets")));
        auto first = root.reader->publishImage("doc.md", file.file->identity(), firstBytes, QStringLiteral("图.png"), "png");
        QVERIFY2(first.status == WorkspaceWriteStatus::Committed, qPrintable(QStringLiteral("error=%1 native=%2 recovery=%3").arg(int(first.error)).arg(first.nativeCode).arg(first.recoveryPath)));
        QCOMPARE(first.status, WorkspaceWriteStatus::Committed); QCOMPARE(first.path, QStringLiteral("assets/images/图.png")); QVERIFY(first.lease);
        QFile held(dir.filePath(first.path)); QVERIFY(!held.open(QIODevice::ReadOnly));
        QFile heldDocument(documentPath); QVERIFY(!heldDocument.open(QIODevice::WriteOnly));
        QVERIFY(!QDir().rename(dir.filePath("assets"), dir.filePath("assets-moved")));
        first.lease.reset(); QVERIFY(held.open(QIODevice::ReadOnly)); QCOMPARE(held.readAll(), firstBytes); held.close();
        auto reused = root.reader->publishImage("doc.md", file.file->identity(), firstBytes, QStringLiteral("图.png"), "png");
        QCOMPARE(reused.status, WorkspaceWriteStatus::Reused); QCOMPARE(reused.path, first.path); QVERIFY(reused.lease);
        QFile overwrite(dir.filePath(reused.path)); QVERIFY(!overwrite.open(QIODevice::WriteOnly)); reused.lease.reset();
        const auto digest = QString::fromLatin1(QCryptographicHash::hash(secondBytes, QCryptographicHash::Sha256).toHex().left(12));
        const auto collision = dir.filePath(QStringLiteral("assets/images/图-%1.png").arg(digest));
        QVERIFY(fileFixtures::write(collision, "occupied"));
        auto second = root.reader->publishImage("doc.md", file.file->identity(), secondBytes, QStringLiteral("图.png"), "png");
        QCOMPARE(second.status, WorkspaceWriteStatus::Committed); QCOMPARE(second.path, QStringLiteral("assets/images/图-%1-2.png").arg(digest));
        second.lease.reset(); QFile saved(dir.filePath(second.path)); QVERIFY(saved.open(QIODevice::ReadOnly)); QCOMPARE(saved.readAll(), secondBytes);
        QFile original(dir.filePath(first.path)); QVERIFY(original.open(QIODevice::ReadOnly)); QCOMPARE(original.readAll(), firstBytes);
        QFile occupied(collision); QVERIFY(occupied.open(QIODevice::ReadOnly)); QCOMPARE(occupied.readAll(), QByteArray("occupied"));
        const QDir imageDir(dir.filePath("assets/images")); QVERIFY(imageDir.entryList({".xdnote-*.tmp"}, QDir::Files | QDir::Hidden).isEmpty());
        const auto tooLarge = root.reader->publishImage("doc.md", file.file->identity(), QByteArray(8 * 1024 * 1024 + 1, 'x'), "large.png", "png");
        QCOMPARE(tooLarge.error, WorkspaceReadError::TooLarge);
        root.reader->revoke(); QCOMPARE(root.reader->publishImage("doc.md", file.file->identity(), firstBytes, "x.png", "png").error, WorkspaceReadError::Revoked);
#endif
    }

    void selectedBinarySourcePreservesBytesAndLimits() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path = dir.filePath(QStringLiteral("源图片.png"));
        const auto bytes = QByteArray::fromHex("89504e47000d0aff0102"); QVERIFY(fileFixtures::write(path, bytes));
        const auto source = FileService::readSelectedBinary(path, 1024); QVERIFY(source.success); QCOMPARE(source.bytes, bytes);
        const auto limited = FileService::readSelectedBinary(path, 4); QVERIFY(!limited.success); QCOMPARE(limited.problem.code, FileError::TooLarge);
        const auto missing = FileService::readSelectedBinary(dir.filePath("missing.png"), 1024); QVERIFY(!missing.success); QCOMPARE(missing.problem.code, FileError::Missing);
        const auto badLimit = FileService::readSelectedBinary(path, 8 * 1024 * 1024 + 1); QVERIFY(!badLimit.success); QCOMPARE(badLimit.problem.code, FileError::InvalidPath);
    }

    void imageImportRejectsInvalidSourcesBeforeWriting() {
#if !defined(Q_OS_WIN) && !defined(Q_OS_LINUX)
        QSKIP("Confined workspace resources are not available on this platform");
#else
        QTemporaryDir dir, outside; QVERIFY(dir.isValid() && outside.isValid());
        QVERIFY(fileFixtures::write(dir.filePath("doc.md"), "doc"));
        const auto svg = outside.filePath("active.svg");
        QVERIFY(fileFixtures::write(svg, "<svg xmlns='http://www.w3.org/2000/svg' width='1' height='1'/>"));
        const auto doc = FileService::probe(dir.filePath("doc.md")); QVERIFY(doc.file);
        const auto root = WorkspaceResourceReader::open(dir.path()); QVERIFY(root.reader);
        const auto invalid = importWorkspaceImage(root.reader, "doc.md", doc.file->identity(), svg, {});
        QCOMPARE(invalid.publication.status, WorkspaceWriteStatus::Failed); QVERIFY(!invalid.message.isEmpty());
        QVERIFY(!QFileInfo::exists(dir.filePath("assets")));
        QImage oversized(8193, 1, QImage::Format_ARGB32); oversized.fill(Qt::red);
        const auto bitmap = importWorkspaceImage(root.reader, "doc.md", doc.file->identity(), {}, oversized);
        QCOMPARE(bitmap.publication.status, WorkspaceWriteStatus::Failed); QVERIFY(!bitmap.message.isEmpty());
        QVERIFY(!QFileInfo::exists(dir.filePath("assets")));
#endif
    }

    void workspaceImagePublicationRejectsBlockedResourceDirectories() {
#ifndef Q_OS_WIN
        QSKIP("Publication is currently implemented only for Windows");
#else
        QTemporaryDir rootDir, outside; QVERIFY(rootDir.isValid() && outside.isValid());
        QVERIFY(fileFixtures::write(rootDir.filePath("doc.md"), "doc"));
        const auto doc = FileService::probe(rootDir.filePath("doc.md")); QVERIFY(doc.file);
        const auto root = WorkspaceResourceReader::open(rootDir.path()); QVERIFY(root.reader);
        QVERIFY(fileFixtures::write(rootDir.filePath("assets"), "occupied"));
        const auto blocked = root.reader->publishImage("doc.md", doc.file->identity(), "bytes", "a.png", "png");
        QCOMPARE(blocked.status, WorkspaceWriteStatus::Failed);
        QFile occupied(rootDir.filePath("assets")); QVERIFY(occupied.open(QIODevice::ReadOnly)); QCOMPARE(occupied.readAll(), QByteArray("occupied")); occupied.close();
        QVERIFY(QFile::remove(rootDir.filePath("assets")));
        fileFixtures::Junction junction; QVERIFY(junction.create(rootDir.filePath("assets"), outside.path()));
        const auto escaped = root.reader->publishImage("doc.md", doc.file->identity(), "bytes", "a.png", "png");
        QCOMPARE(escaped.status, WorkspaceWriteStatus::Failed); QVERIFY(!QFileInfo::exists(outside.filePath("images")));
#endif
    }

    void workspaceImagePublicationHonorsRevocationAroundCommit() {
#ifndef Q_OS_WIN
        QSKIP("Publication is currently implemented only for Windows");
#else
        for (const auto stage : {WorkspaceWriteStage::BeforeCommit, WorkspaceWriteStage::AfterCommit}) {
            QTemporaryDir dir; QVERIFY(dir.isValid()); QVERIFY(fileFixtures::write(dir.filePath("doc.md"), "doc"));
            const auto doc = FileService::probe(dir.filePath("doc.md")); QVERIFY(doc.file);
            const auto root = WorkspaceResourceReader::open(dir.path()); QVERIFY(root.reader);
            QSemaphore reached, resume;
            const auto identity = doc.file->identity();
            auto future = std::async(std::launch::async, [reader = root.reader, identity, stage, &reached, &resume] {
                return reader->publishImage("doc.md", identity, "image-snapshot", "a.png", "png", [&](WorkspaceWriteStage point) {
                    if (point == stage) { reached.release(); resume.acquire(); }
                });
            });
            if (!reached.tryAcquire(1, 10000)) { resume.release(); future.wait(); QFAIL("publication did not reach the requested stage"); }
            root.reader->revoke(); resume.release(); auto result = future.get();
            if (stage == WorkspaceWriteStage::BeforeCommit) {
                QCOMPARE(result.status, WorkspaceWriteStatus::Failed); QCOMPARE(result.error, WorkspaceReadError::Revoked);
                QVERIFY(!QFileInfo::exists(dir.filePath("assets/images/a.png")));
            } else {
                QCOMPARE(result.status, WorkspaceWriteStatus::PublishedButUnverified); QCOMPARE(result.path, QStringLiteral("assets/images/a.png"));
                QFile published(dir.filePath(result.path)); QVERIFY(published.open(QIODevice::ReadOnly)); QCOMPARE(published.readAll(), QByteArray("image-snapshot"));
            }
            const QDir images(dir.filePath("assets/images")); QVERIFY(images.entryList({".xdnote-*.tmp"}, QDir::Files | QDir::Hidden).isEmpty());
        }
#endif
    }

    void workspaceImagePublicationSerializesCollisions() {
#ifndef Q_OS_WIN
        QSKIP("Publication is currently implemented only for Windows");
#else
        QTemporaryDir dir; QVERIFY(dir.isValid()); QVERIFY(fileFixtures::write(dir.filePath("doc.md"), "doc"));
        const auto doc = FileService::probe(dir.filePath("doc.md")); QVERIFY(doc.file);
        const auto root = WorkspaceResourceReader::open(dir.path()); QVERIFY(root.reader);
        const auto id = doc.file->identity();
        auto first = std::async(std::launch::async, [reader = root.reader, id] { return reader->publishImage("doc.md", id, "first", "same.png", "png"); });
        auto second = std::async(std::launch::async, [reader = root.reader, id] { return reader->publishImage("doc.md", id, "second", "same.png", "png"); });
        auto a = first.get(), b = second.get();
        QCOMPARE(a.status, WorkspaceWriteStatus::Committed); QCOMPARE(b.status, WorkspaceWriteStatus::Committed); QVERIFY(a.path != b.path);
        a.lease.reset(); b.lease.reset();
        QFile fileA(dir.filePath(a.path)), fileB(dir.filePath(b.path)); QVERIFY(fileA.open(QIODevice::ReadOnly)); QVERIFY(fileB.open(QIODevice::ReadOnly));
        QCOMPARE(fileA.readAll(), QByteArray("first")); QCOMPARE(fileB.readAll(), QByteArray("second"));
#endif
    }

    void workspaceImagesUseAuthorizedSnapshotsAndStaticDecoding() {
        QTemporaryDir temp; QVERIFY(temp.isValid());
        QVERIFY(QDir().mkpath(temp.filePath("assets")));
        const auto source = QByteArray("![ok](assets/a.png) ![remote](https://example.invalid/a.png) "
            "![svg](assets/a.svg) ![escape](../private.png) ![missing](assets/missing.png)");
        QVERIFY(fileFixtures::write(temp.filePath("doc.md"), source));
        QImage image(3, 2, QImage::Format_ARGB32); image.fill(QColor("#8b42aa"));
        QByteArray bytes; QBuffer buffer(&bytes); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&buffer, "PNG"));
        QVERIFY(fileFixtures::write(temp.filePath("assets/a.png"), bytes));
        QVERIFY(fileFixtures::write(temp.filePath("assets/a.svg"), "<svg xmlns='http://www.w3.org/2000/svg' width='1' height='1'/>"));
        const auto file = FileService::probe(temp.filePath("doc.md")); QVERIFY(file.file);
        const auto root = WorkspaceResourceReader::open(temp.path());
#if !defined(Q_OS_WIN) && !defined(Q_OS_LINUX)
        QCOMPARE(root.error, WorkspaceReadError::Unsupported); return;
#else
        QVERIFY(root.reader);
        DocumentSession session(source); const auto snapshot = session.snapshot();
        const auto prepared = preparePreviewImages(snapshot, root.reader, "doc.md", file.file->identity());
#ifdef Q_OS_LINUX
        if (prepared.png.isEmpty() && prepared.states.size() == 5 && prepared.states[0] == 5) QSKIP("Kernel does not offer confined openat2");
#endif
        QVERIFY(prepared.analyzed); QCOMPARE(prepared.states.size(), 5);
        QCOMPARE(int(prepared.states[0]), 1); QCOMPARE(int(prepared.states[1]), 2);
        QCOMPARE(int(prepared.states[2]), 4); QCOMPARE(int(prepared.states[3]), 2); QCOMPARE(int(prepared.states[4]), 5);
        QCOMPARE(prepared.png.size(), 1); QVERIFY(prepared.png.contains(0));
        const auto decoded = QImage::fromData(prepared.png.value(0), "PNG");
        QCOMPARE(decoded.size(), QSize(3, 2)); QCOMPARE(decoded.pixelColor(0, 0), QColor("#8b42aa"));
        const auto wrongDocument = preparePreviewImages(snapshot, root.reader, "doc.md", "another-identity");
        QVERIFY(wrongDocument.png.isEmpty()); QCOMPARE(wrongDocument.states, QByteArray(5, char(5)));
        root.reader->revoke();
        const auto revoked = preparePreviewImages(snapshot, root.reader, "doc.md", file.file->identity());
        QVERIFY(revoked.png.isEmpty()); QCOMPARE(revoked.states, QByteArray(5, char(5)));
#endif
    }

    void previewLoadsOnlyVerifiedMemoryImages() {
#if !defined(Q_OS_WIN) && !defined(Q_OS_LINUX)
        QSKIP("Confined resource reads are not available on this platform");
#else
        QTemporaryDir temp; QVERIFY(temp.isValid());
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost, 0)); QSignalSpy incoming(&server, &QTcpServer::newConnection);
        const auto remote = QStringLiteral("http://127.0.0.1:%1/a.png").arg(server.serverPort());
        const auto source = QStringLiteral("![local](a.png) ![remote](%1) <img src='file:///private'>").arg(remote).toUtf8();
        QVERIFY(fileFixtures::write(temp.filePath("doc.md"), source));
        QImage image(3, 2, QImage::Format_ARGB32); image.fill(Qt::red);
        QByteArray bytes; QBuffer buffer(&bytes); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&buffer, "PNG"));
        QVERIFY(fileFixtures::write(temp.filePath("a.png"), bytes));
        const auto file = FileService::probe(temp.filePath("doc.md")); QVERIFY(file.file);
        const auto root = WorkspaceResourceReader::open(temp.path()); QVERIFY(root.reader);
        DocumentSession session(source); MarkdownPreview preview(&session);
        QSignalSpy ready(&preview, &MarkdownPreview::previewReady), errors(&preview, &MarkdownPreview::previewError);
        preview.setResourceContext(root.reader, "doc.md", file.file->identity());
        preview.resize(600, 400); preview.show();
        QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty() || !errors.isEmpty(), 30000);
        QVERIFY2(errors.isEmpty(), errors.isEmpty() ? "" : qPrintable(errors.first().first().toString()));
        QString html; bool received = false;
        preview.view()->page()->toHtml([&](const QString &value) { html = value; received = true; });
        QTRY_VERIFY_WITH_TIMEOUT(received, 10000);
        QVERIFY(!html.contains(remote)); QVERIFY(!html.contains("<img src=\"file:///private"));
        QVERIFY(html.contains("src=\"xdnote-preview:"));
        QVariant size; bool checked = false;
        preview.view()->page()->runJavaScript(QStringLiteral("document.images.length ? document.images[0].naturalWidth : -1"), QWebEngineScript::ApplicationWorld,
            [&](const QVariant &value) { size = value; checked = true; });
        QTRY_VERIFY_WITH_TIMEOUT(checked, 10000); QCOMPARE(size.toInt(), 3);
        QTest::qWait(200); QCOMPARE(incoming.count(), 0);
        root.reader->revoke(); ready.clear(); preview.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 30000);
        received = false;
        preview.view()->page()->toHtml([&](const QString &value) { html = value; received = true; });
        QTRY_VERIFY_WITH_TIMEOUT(received, 10000); QVERIFY(!html.contains("<img src=\"xdnote-preview:"));
#endif
    }
    void fileBaselinePreservesUtf8BomAndMixedNewlines() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath(QStringLiteral("中文😀.md"));
        const auto body = QStringLiteral("第一行😀\r\n第二行\n第三行\r末尾\u2028\u2029").toUtf8();
        for (bool bom : {false, true}) {
            const auto disk = (bom ? QByteArray::fromHex("efbbbf") : QByteArray()) + body;
            QVERIFY(fileFixtures::write(path, disk));
            auto probe = std::async(std::launch::async, [path] { return FileService::probe(path); }).get();
            QVERIFY(probe.file);
            SessionRegistry registry;
            const auto request = registry.beginOpen(probe.file).read;
            auto loaded = std::async(std::launch::async, [request] { return FileService::read(request); }).get();
            QVERIFY2(loaded.baseline, qPrintable(loaded.problem.description()));
            QCOMPARE(loaded.baseline->diskBytes(), disk);
            QCOMPARE(loaded.baseline->utf8(), body);
            QCOMPARE(loaded.baseline->hasUtf8Bom(), bom);
            QCOMPARE(loaded.baseline->sha256().size(), 32);
            auto adopted = registry.complete(request, loaded.baseline);
            QCOMPARE(adopted.result, RegistryResult::Created);
            QCOMPARE(adopted.session->snapshot().utf8, body);
            QVERIFY(!adopted.session->isDirty());
            QVERIFY(!adopted.session->documentId().isNull());
            QCOMPARE(adopted.session->snapshot().documentId, adopted.session->documentId());
            // A later disk write cannot mutate the captured baseline.
            QVERIFY(fileFixtures::write(path, "external"));
            QCOMPARE(loaded.baseline->diskBytes(), disk);
        }
    }

    void fileAliasesReuseDirtySessionAndUndoWithoutReadingAgain() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("original.md"), alias = directory.filePath("alias.md");
        QVERIFY(fileFixtures::write(path, "original"));
        QVERIFY(fileFixtures::hardLink(path, alias));
        auto first = FileService::probe(path);
        auto second = FileService::probe(alias);
        QVERIFY(first.file && second.file);
        QCOMPARE(first.file->identity(), second.file->identity());
        SessionRegistry registry;
        const auto opening = registry.beginOpen(first.file);
        QVERIFY(opening.startRead);
        const auto request = opening.read;
        const auto pending = registry.beginOpen(second.file);
        QCOMPARE(pending.read.requestId, request.requestId);
        QVERIFY(!pending.startRead);
        auto loaded = FileService::read(request);
        QVERIFY(loaded.baseline);
        auto original = registry.complete(request, loaded.baseline);
        QVERIFY(original.session);
        const auto id = original.session->documentId();
        QCOMPARE(original.session->replace(0, 0, "unsaved ", original.session->revision()), EditResult::Applied);
        QVERIFY(original.session->isDirty());
        const auto edited = original.session->snapshot();
        // Disk now has invalid UTF-8. A duplicate open must still activate the
        // existing session, before attempting a text read or decoder.
        QVERIFY(fileFixtures::write(path, QByteArray::fromHex("ffff")));
        auto repeated = FileService::probe(directory.path() + "/./alias.md");
        QVERIFY(repeated.file);
        auto reused = registry.lookup(repeated.file);
        QCOMPARE(reused.result, RegistryResult::Reused);
        QCOMPARE(reused.session, original.session);
        QCOMPARE(reused.session->documentId(), id);
        QCOMPARE(reused.session->snapshot().utf8, edited.utf8);
        QCOMPARE(FileService::read(FileReadRequest{second.file}).problem.code, FileError::InvalidUtf8);
        // A late successful read from an earlier request also cannot reset it.
        QCOMPARE(registry.complete(request, loaded.baseline).session, original.session);
        QCOMPARE(registry.beginOpen(repeated.file).session, original.session);
        QCOMPARE(original.session->undo(), EditResult::Applied);
        QCOMPARE(original.session->snapshot().utf8, QByteArray("original"));
        QVERIFY(!original.session->isDirty());
        QCOMPARE(original.session->redo(), EditResult::Applied);
        QVERIFY(original.session->isDirty());
    }

    void fileBomDoesNotConsumeLeadingContentCharacterOrOffsets() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("double-bom.md");
        const auto body = QStringLiteral("\ufeff中文😀").toUtf8();
        const auto disk = QByteArray::fromHex("efbbbf") + body;
        QVERIFY(fileFixtures::write(path, disk));
        auto probe = FileService::probe(path);
        QVERIFY(probe.file);
        SessionRegistry registry;
        const auto request = registry.beginOpen(probe.file).read;
        auto loaded = FileService::read(request);
        QVERIFY2(loaded.baseline, qPrintable(loaded.problem.description()));
        QCOMPARE(loaded.baseline->utf8(), body);
        QCOMPARE(loaded.baseline->diskBytes(), disk);
        auto opened = registry.complete(request, loaded.baseline);
        QVERIFY(opened.session);
        SourceEditor editor(opened.session);
        auto *accessible = QAccessible::queryAccessibleInterface(&editor);
        QVERIFY(accessible && accessible->textInterface());
        QCOMPARE(accessible->textInterface()->text(0, -1), QStringLiteral("\ufeff中文😀"));
        QCOMPARE(opened.session->replace(1, 1, "+", opened.session->revision()), EditResult::Applied);
        QCOMPARE(opened.session->snapshot().utf8, QStringLiteral("\ufeff+中文😀").toUtf8());
        QCOMPARE(opened.session->undo(), EditResult::Applied);
        QCOMPARE(opened.session->snapshot().utf8, body);
    }

    void fileReplacementHasNewIdentityAndCannotSatisfyOldRead() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("document.md"), moved = directory.filePath("moved.md");
        QVERIFY(fileFixtures::write(path, "before"));
        auto original = FileService::probe(path);
        QVERIFY(original.file);
        SessionRegistry registry;
        const auto request = registry.beginOpen(original.file).read;
        auto originalText = FileService::read(request);
        QVERIFY(originalText.baseline);
        auto previous = registry.complete(request, originalText.baseline);
        QVERIFY(QFile::rename(path, moved));
        QVERIFY(fileFixtures::write(path, "after"));
        auto replacement = FileService::probe(path);
        QVERIFY(replacement.file);
        QVERIFY(original.file->identity() != replacement.file->identity());
        QCOMPARE(FileService::read(FileReadRequest{original.file}).problem.code, FileError::Changed);
        auto movedProbe = FileService::probe(moved);
        QVERIFY(movedProbe.file);
        QCOMPARE(movedProbe.file->identity(), original.file->identity());
        QCOMPARE(registry.lookup(movedProbe.file).session, previous.session);
        const auto replacementRequest = registry.beginOpen(replacement.file).read;
        auto replacementText = FileService::read(replacementRequest);
        QVERIFY(replacementText.baseline);
        auto next = registry.complete(replacementRequest, replacementText.baseline);
        QVERIFY(next.session != previous.session);
        QVERIFY(next.session->documentId() != previous.session->documentId());
        QCOMPARE(previous.session->snapshot().utf8, QByteArray("before"));
        QCOMPARE(next.session->snapshot().utf8, QByteArray("after"));
    }

    void fileFailuresAreTypedAndDoNotModifyBytes() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("invalid.md");
        QCOMPARE(FileService::probe("relative.md").problem.code, FileError::InvalidPath);
        QCOMPARE(FileService::probe(directory.path() + '/' + QChar(0xd800)).problem.code, FileError::InvalidPath);
        QCOMPARE(FileService::probe(path).problem.code, FileError::Missing);
        QCOMPARE(FileService::probe(directory.path()).problem.code, FileError::UnsupportedFile);
        const QList<QByteArray> rejected{QByteArray::fromHex("fffe6100"), QByteArray::fromHex("c0af"),
            QByteArray::fromHex("eda080"), QByteArray::fromHex("e4b8"), QByteArray("a\0b", 3)};
        for (const auto &bytes : rejected) {
            QVERIFY(fileFixtures::write(path, bytes));
            auto probe = FileService::probe(path);
            QVERIFY(probe.file);
            auto read = FileService::read(FileReadRequest{probe.file});
            QVERIFY(!read.baseline);
            QCOMPARE(read.problem.code, bytes.contains('\0') && QString::fromUtf8(bytes).toUtf8() == bytes
                ? FileError::UnsupportedContent : FileError::InvalidUtf8);
            QVERIFY(!read.problem.description().isEmpty());
            QFile actual(path);
            QVERIFY(actual.open(QIODevice::ReadOnly));
            QCOMPARE(actual.readAll(), bytes);
        }
        QVERIFY(fileFixtures::write(path, {}));
        auto empty = FileService::read(FileReadRequest{FileService::probe(path).file});
        QVERIFY(empty.baseline);
        QVERIFY(empty.baseline->utf8().isEmpty());
        SessionRegistry registry;
        QCOMPARE(registry.complete({}, {}).result, RegistryResult::InvalidBaseline);
    }

    void fileSourceQuotaIncludesBomWithoutTruncation() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("quota.md");
        const QByteArray body(FileService::SourceLimit, 'a');
        QVERIFY(fileFixtures::write(path, QByteArray::fromHex("efbbbf") + body));
        auto accepted = FileService::read(FileReadRequest{FileService::probe(path).file});
        QVERIFY(accepted.baseline);
        QCOMPARE(accepted.baseline->utf8(), body);
        QVERIFY(fileFixtures::write(path, body + "x"));
        auto rejected = FileService::read(FileReadRequest{FileService::probe(path).file});
        QVERIFY(!rejected.baseline);
        QCOMPARE(rejected.problem.code, FileError::TooLarge);
    }

    void fileFinalLinksAndNonRegularObjectsAreRejected() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("regular.md"), alias = directory.filePath("link");
        QVERIFY(fileFixtures::write(path, "text"));
#ifdef Q_OS_WIN
        fileFixtures::Junction junction;
        QVERIFY(junction.create(alias, directory.path()));
        QCOMPARE(FileService::probe(alias).problem.code, FileError::UnsupportedFile);
        QCOMPARE(FileService::probe(QStringLiteral("//./pipe/xdnote-should-not-open")).problem.code, FileError::InvalidPath);
        QCOMPARE(FileService::probe(path + ":alternate").problem.code, FileError::InvalidPath);
#else
        QVERIFY(::symlink(path.toUtf8().constData(), alias.toUtf8().constData()) == 0);
        QCOMPARE(FileService::probe(alias).problem.code, FileError::UnsupportedFile);
        const auto fifo = directory.filePath("fifo").toUtf8();
        QVERIFY(::mkfifo(fifo.constData(), 0600) == 0);
        QCOMPARE(FileService::probe(QString::fromUtf8(fifo)).problem.code, FileError::UnsupportedFile);
#endif
    }

    void fileReadDenialKeepsExistingSessionUsable() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("document.md");
        QVERIFY(fileFixtures::write(path, "retained"));
        auto probe = FileService::probe(path);
        SessionRegistry registry;
        const auto request = registry.beginOpen(probe.file).read;
        auto loaded = FileService::read(request);
        QVERIFY(loaded.baseline);
        auto original = registry.complete(request, loaded.baseline);
        QVERIFY(original.session);
#ifdef Q_OS_WIN
        fileFixtures::ReadBlock block(path);
        QVERIFY(block.valid());
        auto failed = FileService::read(FileReadRequest{probe.file});
        QCOMPARE(failed.problem.code, FileError::InUse);
        QVERIFY(failed.problem.nativeCode != 0);
        QVERIFY(!failed.problem.description().isEmpty());
#else
        QVERIFY(::chmod(path.toUtf8().constData(), 0000) == 0);
        auto failed = FileService::read(FileReadRequest{probe.file});
        // Root bypasses discretionary read permissions; CI uses an ordinary
        // account. Both cases retain the registry without rewriting anything.
        if (::geteuid() != 0) QCOMPARE(failed.problem.code, FileError::Denied);
        QVERIFY(::chmod(path.toUtf8().constData(), 0600) == 0);
#endif
        auto same = FileService::probe(path);
        QVERIFY(same.file);
        QCOMPARE(registry.lookup(same.file).session, original.session);
        QCOMPARE(original.session->snapshot().utf8, QByteArray("retained"));
    }

    void fileRegistryRejectsWorkerAccessAndRebindDuringClose() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("document.md");
        QVERIFY(fileFixtures::write(path, "text"));
        auto probe = FileService::probe(path);
        SessionRegistry registry;
        const auto request = registry.beginOpen(probe.file).read;
        auto loaded = FileService::read(request);
        QVERIFY(loaded.baseline);
        auto original = registry.complete(request, loaded.baseline);
        QVERIFY(original.session);
        const auto id = original.session->documentId();
        auto wrong = std::async(std::launch::async, [&] { return registry.complete(request, loaded.baseline).result; }).get();
        QCOMPARE(wrong, RegistryResult::WrongThread);
        OpenRegistration pendingReopen;
        bool oldAliveWhenReserved = false;
        // This queued observer runs after registry removal but before the old
        // session's deleteLater event. Its destroyed callback must not erase
        // the new request which now occupies the same file identity.
        connect(original.session, &DocumentSession::closed, &registry, [&] {
            oldAliveWhenReserved = !original.session.isNull();
            pendingReopen = registry.beginOpen(probe.file);
        }, Qt::QueuedConnection);
        QCOMPARE(original.session->requestClose(), EditResult::Applied);
        QCOMPARE(registry.complete(request, loaded.baseline).result, RegistryResult::Closing);
        QTRY_VERIFY(pendingReopen.startRead);
        QVERIFY(oldAliveWhenReserved);
        QTRY_VERIFY(original.session.isNull());
        QCOMPARE(registry.lookup(probe.file).result, RegistryResult::Pending);
        QCOMPARE(registry.complete(request, loaded.baseline).result, RegistryResult::StaleRequest);
        const auto nextRequest = pendingReopen.read;
        QCOMPARE(registry.complete(request, loaded.baseline).result, RegistryResult::StaleRequest);
        QCOMPARE(registry.complete(nextRequest, loaded.baseline).result, RegistryResult::InvalidBaseline);
        auto newRead = FileService::read(nextRequest);
        auto reopened = registry.complete(nextRequest, newRead.baseline);
        QCOMPARE(reopened.result, RegistryResult::Created);
        QVERIFY(reopened.session->documentId() != id);
        QCOMPARE(registry.cancel(nextRequest), RegistryResult::Reused);
        QCOMPARE(reopened.session->snapshot().utf8, QByteArray("text"));
    }

    void cancelledFileReadCannotCreateOrOverwriteSession() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("document.md");
        QVERIFY(fileFixtures::write(path, "before"));
        auto probe = FileService::probe(path);
        QVERIFY(probe.file);
        SessionRegistry registry;
        const auto first = registry.beginOpen(probe.file).read;
        auto delayed = FileService::read(first);
        QVERIFY(delayed.baseline);
        QCOMPARE(registry.cancel(first), RegistryResult::Cancelled);
        QCOMPARE(registry.complete(first, delayed.baseline).result, RegistryResult::StaleRequest);
        QVERIFY(fileFixtures::write(path, "after"));
        const auto second = registry.beginOpen(probe.file).read;
        QVERIFY(first.requestId != second.requestId);
        QCOMPARE(registry.complete(first, delayed.baseline).result, RegistryResult::StaleRequest);
        auto current = FileService::read(second);
        QVERIFY(current.baseline);
        auto opened = registry.complete(second, current.baseline);
        QCOMPARE(opened.result, RegistryResult::Created);
        QCOMPARE(opened.session->snapshot().utf8, QByteArray("after"));
        QCOMPARE(registry.complete(first, delayed.baseline).result, RegistryResult::StaleRequest);
    }

    void systemFontsCanShapeChinese() {
        for (auto family : {QFontDatabase::GeneralFont, QFontDatabase::FixedFont}) {
            QTextLayout layout(QStringLiteral("中文界面"), QFontDatabase::systemFont(family));
            layout.beginLayout();
            auto line = layout.createLine();
            line.setLineWidth(300);
            layout.endLayout();
            const auto runs = layout.glyphRuns();
            QVERIFY(!runs.isEmpty());
            for (const auto &run : runs)
                for (auto glyph : run.glyphIndexes()) QVERIFY2(glyph != 0, "System font fallback has missing Chinese glyphs");
        }
    }

    void literalTextAndAtomicUndo() {
        QByteArray original = QStringLiteral("中文😀\u00a0\u2028\u2029\r\n末尾").toUtf8();
        original.append('\0'); original.append("after NUL\n");
        DocumentSession session(original);
        SourceEditor first(&session), second(&session);
        QCOMPARE(session.snapshot().utf8, original);
        QCOMPARE(first.send(SCI_GETDOCPOINTER), second.send(SCI_GETDOCPOINTER));
        auto rev = session.revision();
        QCOMPARE(session.replace(3, 4, "x", rev), EditResult::InvalidRange); // middle of emoji
        QCOMPARE(session.replace(0, 0, QString(QChar(0xd800)), rev), EditResult::InvalidText);
        QCOMPARE(session.snapshot().utf8, original);
        QCOMPARE(session.replace(2, 4, QStringLiteral("你好"), rev), EditResult::Applied);
        QVERIFY(session.revision() > rev);
        QVERIFY(session.snapshot().utf8.startsWith(QStringLiteral("中文你好").toUtf8()));
        QCOMPARE(session.undo(), EditResult::Applied);
        QCOMPARE(session.snapshot().utf8, original);
        QCOMPARE(session.redo(), EditResult::Applied);
        QCOMPARE(session.replace(0, 0, "stale", rev), EditResult::StaleRevision);
    }

    void viewLifetimeAndReadOnly() {
        auto session = std::make_unique<DocumentSession>("initial");
        auto view = std::make_unique<SourceEditor>(session.get());
        QCOMPARE(session->replace(0, 7, "edited", 0), EditResult::Applied);
        view.reset();
        SourceEditor replacement(session.get());
        QCOMPARE(session->snapshot().utf8, QByteArray("edited"));
        QCOMPARE(session->setReadOnly(true), EditResult::Applied);
        QCOMPARE(session->replace(0, 0, "blocked", session->revision()), EditResult::ReadOnly);
        QCOMPARE(session->undo(), EditResult::ReadOnly);
        QVERIFY(!session->rejection().isEmpty());
        QCOMPARE(session->snapshot().utf8, QByteArray("edited"));
        QCOMPARE(session->setReadOnly(false), EditResult::Applied);
        QCOMPARE(session->undo(), EditResult::Applied);
        QCOMPARE(session->snapshot().utf8, QByteArray("initial"));
        session.reset();
        QVERIFY(replacement.session() == nullptr);
        QVERIFY(replacement.send(SCI_GETREADONLY));
    }

    void compositionAndCommit() {
        DocumentSession session;
        SourceEditor view(&session);
        QInputMethodEvent preedit(QStringLiteral("ni"), {});
        QApplication::sendEvent(&view, &preedit);
        QVERIFY(view.compositionPending());
        QCOMPARE(session.snapshot().result, EditResult::Composing);
        QCOMPARE(session.replace(0, 0, "bad", 0), EditResult::Composing);
        QCOMPARE(session.requestClose(), EditResult::Composing);
        QInputMethodEvent mixed(QStringLiteral("hao"), {});
        mixed.setCommitString(QStringLiteral("你"));
        QApplication::sendEvent(&view, &mixed);
        QCOMPARE(session.snapshot().result, EditResult::Composing);
        QInputMethodEvent commit;
        commit.setCommitString(QStringLiteral("好"));
        QApplication::sendEvent(&view, &commit);
        QVERIFY(!view.compositionPending());
        QCOMPARE(session.snapshot().utf8, QStringLiteral("你好").toUtf8());
        QCOMPARE(session.undo(), EditResult::Applied);
        QVERIFY(!session.snapshot().utf8.contains("hao"));
    }

    void reentrancyClosingAndWrongThread() {
        DocumentSession session("a");
        EditResult reentrant = EditResult::Applied;
        QSignalSpy closed(&session, &DocumentSession::closed);
        connect(&session, &DocumentSession::changed, &session, [&](quint64 rev) {
            reentrant = session.replace(0, 0, "bad", rev);
            QCOMPARE(session.snapshot().result, EditResult::Busy);
            QCOMPARE(session.requestClose(), EditResult::Applied);
        });
        QCOMPARE(session.replace(0, 1, "b", 0), EditResult::Applied);
        QCOMPARE(reentrant, EditResult::Busy);
        QCOMPARE(session.snapshot().result, EditResult::Closing);
        QCOMPARE(session.replace(0, 0, "bad", 1), EditResult::Closing);
        QTRY_COMPARE(closed.count(), 1);
        auto wrongThread = std::async(std::launch::async, [&] { return session.snapshot().result; });
        QCOMPARE(wrongThread.get(), EditResult::WrongThread);
    }

    void accessibleCommandsAndInvalidation() {
        auto session = std::make_unique<DocumentSession>(QStringLiteral("甲😀乙").toUtf8());
        SourceEditor view(session.get());
        auto *accessible = QAccessible::queryAccessibleInterface(&view);
        QVERIFY(accessible && accessible->isValid());
        auto *text = accessible->textInterface();
        auto *edit = accessible->editableTextInterface();
        QVERIFY(text && edit);
        QCOMPARE(text->characterCount(), 4);
        edit->replaceText(1, 2, "bad");
        QCOMPARE(session->snapshot().utf8, QStringLiteral("甲😀乙").toUtf8());
        edit->replaceText(1, 3, QStringLiteral("中文"));
        QCOMPARE(session->snapshot().utf8, QStringLiteral("甲中文乙").toUtf8());
        session->setReadOnly(true);
        accessible->setText(QAccessible::Value, "blocked");
        edit->insertText(0, "blocked");
        QCOMPARE(session->snapshot().utf8, QStringLiteral("甲中文乙").toUtf8());
        QVERIFY(accessible->state().readOnly);
        QVERIFY(!accessible->text(QAccessible::Description).isEmpty());
        session->setReadOnly(false);
        QCOMPARE(session->undo(), EditResult::Applied);
        QCOMPARE(text->text(0, -1), QStringLiteral("甲😀乙"));
        session.reset();
        QVERIFY(!accessible->isValid());
        QCOMPARE(text->characterCount(), 0);
        edit->insertText(0, "ignored");
    }

    void accessibleWritesDoNotFlushDocumentOnAnotherThread() {
        for (const bool wholeValue : {false, true}) {
            DocumentSession session("a");
            SourceEditor view(&session);
            auto *accessible = QAccessible::queryAccessibleInterface(&view);
            QVERIFY(accessible && accessible->editableTextInterface());
            std::atomic<bool> wrongThread{false};
            connect(&session, &DocumentSession::changed, &session, [&](quint64) {
                if (QThread::currentThread() != session.thread()) wrongThread.store(true);
            }, Qt::DirectConnection);
            QTest::keyClick(&view, Qt::Key_X); // leaves a queued document flush
            auto call = std::async(std::launch::async, [accessible, wholeValue] {
                if (wholeValue) accessible->setText(QAccessible::Value, "forbidden");
                else accessible->editableTextInterface()->insertText(0, "forbidden");
            });
            call.get();
            QVERIFY(!wrongThread.load());
            const auto snapshot = session.snapshot();
            QCOMPARE(snapshot.result, EditResult::Applied);
            QVERIFY(snapshot.utf8.contains('x'));
            QVERIFY(!snapshot.utf8.contains("forbidden"));
            QCOMPARE(session.undo(), EditResult::Applied);
            QCOMPARE(session.snapshot().utf8, QByteArray("a"));
        }
    }

    void accessibleWritesRejectUncommittedComposition() {
        DocumentSession session("a");
        SourceEditor view(&session);
        auto *accessible = QAccessible::queryAccessibleInterface(&view);
        QVERIFY(accessible && accessible->editableTextInterface());
        QCOMPARE(session.replace(1, 1, "b", session.revision()), EditResult::Applied);
        QInputMethodEvent preedit(QStringLiteral("ni"), {});
        QApplication::sendEvent(&view, &preedit);
        QCOMPARE(session.snapshot().result, EditResult::Composing);
        accessible->setText(QAccessible::Value, "forbidden");
        accessible->editableTextInterface()->insertText(0, "forbidden");
        QCOMPARE(session.rejection(), resultDescription(EditResult::Composing));
        QInputMethodEvent cancel;
        QApplication::sendEvent(&view, &cancel);
        QCOMPARE(session.snapshot().utf8, QByteArray("ab"));
        QCOMPARE(session.undo(), EditResult::Applied);
        QCOMPARE(session.snapshot().utf8, QByteArray("a"));
    }

    void nativeInputNotificationRejectsReentrantWrite() {
        DocumentSession session;
        SourceEditor editor(&session);
        EditResult result = EditResult::Applied;
        bool observed = false;
        connect(&editor, &ScintillaEditBase::notifyChange, &session, [&] {
            observed = true;
            result = session.replace(0, 0, "reentrant", 0);
            QCOMPARE(session.snapshot().result, EditResult::Busy);
        });
        QTest::keyClick(&editor, Qt::Key_A);
        QVERIFY(observed);
        QCOMPARE(result, EditResult::Busy);
        QCOMPARE(session.snapshot().utf8, QByteArray("a"));
        QCOMPARE(session.undo(), EditResult::Applied);
        QVERIFY(session.snapshot().utf8.isEmpty());
    }

    void previewIsolationAndLatestRevision() {
        DocumentSession session("# safe\n<script>alert(1)</script>\n![x](file:///private)\n[x](https://example.invalid)");
        MarkdownPreview preview(&session);
        QSignalSpy ready(&preview, &MarkdownPreview::previewReady);
        QSignalSpy errors(&preview, &MarkdownPreview::previewError);
        int staleReady = 0;
        connect(&preview, &MarkdownPreview::previewReady, &preview, [&](quint64 revision) {
            if (revision != session.revision()) ++staleReady;
        });
        preview.resize(700, 500); preview.show();
        QTRY_VERIFY_WITH_TIMEOUT(ready.count() > 0 || errors.count() > 0, 30000);
        QVERIFY2(ready.count() > 0, errors.isEmpty() ? "preview did not complete" : qPrintable(errors.first().first().toString()));
        QVERIFY(!preview.view()->settings()->testAttribute(QWebEngineSettings::JavascriptEnabled));
        QVERIFY(!preview.view()->settings()->testAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls));
        QString html;
        bool received = false;
        preview.view()->page()->toHtml([&](const QString &value) { html = value; received = true; });
        QTRY_VERIFY_WITH_TIMEOUT(received, 10000);
        QVERIFY(html.contains("&lt;script&gt;"));
        QVERIFY(!html.contains("<script>"));
        QVERIFY(!html.contains("file:///private"));
        QVERIFY(!html.contains("href=\"https://"));
        QVERIFY(html.contains("default-src 'none'"));
        preview.refresh(); // an older snapshot may complete before queued refreshes
        for (int i = 0; i < 8; ++i) {
            const auto snap = session.snapshot();
            QCOMPARE(session.replace(0, QString::fromUtf8(snap.utf8).size(), QStringLiteral("# latest %1").arg(i), snap.revision), EditResult::Applied);
        }
        const auto latest = session.revision();
        QTRY_COMPARE_WITH_TIMEOUT(preview.displayedRevision(), latest, 30000);
        QCOMPARE(errors.count(), 0);
        QCOMPARE(staleReady, 0);
        QSignalSpy navigation(preview.view()->page(), &QWebEnginePage::loadFinished);
        for (const auto &url : {QUrl(QStringLiteral("xdnote-preview:/foreign-document")), QUrl(QStringLiteral("data:text/html,<h1>untrusted</h1>"))}) {
            navigation.clear(); preview.view()->page()->load(url);
            QTRY_VERIFY_WITH_TIMEOUT(!navigation.isEmpty(), 10000);
            QVERIFY(!navigation.last().first().toBool());
        }
        ready.clear();
        session.requestClose();
        QTRY_VERIFY(session.isClosing());
        QTest::qWait(100);
        QCOMPARE(ready.count(), 0);
    }

    void mixedHostAndSharedChrome() {
        DocumentSession session(QStringLiteral("# 集成验证\n\n中文正文与共享控件。\n").toUtf8());
        AdmissionWindow window(&session);
        auto *quick = window.findChild<QQuickWidget *>("sharedChrome");
        QVERIFY(quick);
        QCOMPARE(quick->status(), QQuickWidget::Ready);
        QVERIFY(quick->errors().isEmpty());
        const auto geometry = visualTokens()["windows"].toObject()["main"].toObject();
        QCOMPARE(quick->height(), geometry["toolbarHeight"].toInt());
        window.show();
        QTRY_VERIFY(window.isVisible());
        QSignalSpy previewReady(window.preview(), &MarkdownPreview::previewReady);
        QTRY_VERIFY_WITH_TIMEOUT(previewReady.count() > 0, 30000);
        const QImage chrome = quick->grabFramebuffer();
        QVERIFY(!chrome.isNull());
        bool hasContent = false;
        for (int y = 0; y < chrome.height() && !hasContent; ++y)
            for (int x = 0; x < chrome.width() && !hasContent; ++x)
                hasContent = chrome.pixel(x, y) != chrome.pixel(0, 0);
        QVERIFY2(hasContent, "QML operation area rendered blank");
        QVERIFY(window.preview()->width() > 100);
        auto *button = quick->rootObject()->findChild<QQuickItem *>("readOnlyButton");
        QVERIFY(button);
        const auto point = button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint();
        QTest::mouseClick(quick, Qt::LeftButton, Qt::NoModifier, point);
        QVERIFY(session.isReadOnly());
        QVERIFY(!button->property("keyboardOutline").toBool());
        quick->setFocus();
        button->forceActiveFocus(Qt::OtherFocusReason);
        QTest::keyClick(quick, Qt::Key_Tab, Qt::ShiftModifier);
        QTest::keyClick(quick, Qt::Key_Tab);
        QTRY_VERIFY(button->property("keyboardOutline").toBool());
        QTest::mouseClick(quick, Qt::LeftButton, Qt::NoModifier, QPoint(2, 2));
        QVERIFY(!button->property("keyboardOutline").toBool());
        QTest::keyClick(quick, Qt::Key_Tab, Qt::ShiftModifier);
        QTest::keyClick(quick, Qt::Key_Tab);
        QTRY_VERIFY(button->property("keyboardOutline").toBool());
        QTest::keyClick(quick, Qt::Key_Space);
        QVERIFY(!session.isReadOnly());
        QVERIFY(!button->property("keyboardOutline").toBool());
        QVERIFY(button->hasActiveFocus());
        const auto evidence = qEnvironmentVariable("XDNOTE_TEST_EVIDENCE");
        if (!evidence.isEmpty()) QVERIFY(window.grab().save(evidence + "/desktop-admission.png"));
    }

    void largePreviewAndCloseInvalidateWork() {
        DocumentSession large(QString(300000, QChar(0x7532)).toUtf8());
        auto window = std::make_unique<AdmissionWindow>(&large);
        auto *preview = window->preview();
        QSignalSpy errors(preview, &MarkdownPreview::previewError);
        QSignalSpy ready(preview, &MarkdownPreview::previewReady);
        QTRY_VERIFY_WITH_TIMEOUT(ready.count() > 0 || errors.count() > 0, 30000);
        QVERIFY2(!ready.isEmpty(), errors.isEmpty() ? "large preview did not finish" : qUtf8Printable(errors.first().first().toString()));
        QCOMPARE(errors.count(), 0);
        QCOMPARE(preview->view()->url().scheme(), QStringLiteral("xdnote-preview"));
        QString text; bool received = false;
        preview->view()->page()->toPlainText([&](const QString &value) { text = value; received = true; });
        QTRY_VERIFY_WITH_TIMEOUT(received, 10000);
        QCOMPARE(text.count(QChar(0x7532)), 300000);
        preview->refresh();
        window.reset(); // a running Rust task owns no widget or session pointer
        QTest::qWait(100);
    }
};

int main(int argc, char **argv) {
    initializeGuiRuntime();
    QApplication app(argc, argv);
    configureGuiApplication();
    SourceEditor::installAccessibility();
    DesktopTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "desktop_tests.moc"
