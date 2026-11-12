#include "PreviewAnalysis.h"
#include "MarkdownHeadings.h"
#include "MarkdownPreview.h"
#include "Appearance.h"
#include "GuiRuntime.h"
#include "ImageAdmission.h"
#include "FileService.h"
#include "FileApplication.h"
#include "WorkspaceWindow.h"
#include "SingleFileWindow.h"
#include "SourceEditor.h"
#include "file_fixtures.h"
#include <QApplication>
#include <QBuffer>
#include <QCryptographicHash>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>
#include <QtConcurrentRun>

using namespace xdnote;
namespace {
DocumentSnapshot snapshot(QByteArray source) { return {EditResult::Applied, std::move(source), 7, QUuid::createUuid()}; }
QByteArray image(QColor color) {
    QImage value(12, 8, QImage::Format_ARGB32); value.fill(color);
    QByteArray bytes; QBuffer out(&bytes); out.open(QIODevice::WriteOnly); value.save(&out, "PNG"); return bytes;
}
bool ready(MarkdownPreview &preview, DocumentSession &session) {
    QElapsedTimer elapsed; elapsed.start();
    while (elapsed.elapsed() < 20000) {
        QTest::qWait(5);
        if (preview.view()->ready() && preview.displayedRevision() == session.revision()
            && !preview.workStatistics()["running"].toBool() && !preview.workStatistics()["pending"].toBool()) return true;
    }
    return false;
}
}
class PreviewPipelineTests : public QObject {
    Q_OBJECT
private slots:
    void combinedAnalysisPreservesReferencesAndSourceRanges() {
        auto input = snapshot(QStringLiteral("# 中文 😀\r\n\r\n[a &amp; b][共享]\n![图][共享]\n`[伪](no.md)`\n\n[共享]: <assets/a\\(b\\).png> \"标题\"\n\n# 中文 😀\n").toUtf8());
        const auto classic = analyzeMarkdownReferences(input); QVERIFY(classic.success); QCOMPARE(classic.references.size(), 2);
        const auto headings = analyzeMarkdownHeadings(input); QVERIFY(headings.success);
        QJsonObject metrics; QString error;
        auto result = PreviewAnalysis::get(input, {}, metrics, &error); QVERIFY2(result, qPrintable(error));
        const auto cachedHeadings = analyzeMarkdownHeadings(input); QVERIFY(cachedHeadings.success); QCOMPARE(cachedHeadings.headings.size(), headings.headings.size());
        for (int i = 0; i < headings.headings.size(); ++i) {
            QCOMPARE(cachedHeadings.headings[i].title, headings.headings[i].title); QCOMPARE(cachedHeadings.headings[i].id, headings.headings[i].id);
            QCOMPARE(cachedHeadings.headings[i].range.start, headings.headings[i].range.start); QCOMPARE(cachedHeadings.headings[i].range.end, headings.headings[i].range.end);
        }
        QCOMPARE(result->references.references.size(), classic.references.size());
        for (int i = 0; i < classic.references.size(); ++i) {
            const auto a = result->references.references[i], b = classic.references[i];
            QCOMPARE(a.kind, b.kind); QCOMPARE(a.target, b.target); QCOMPARE(a.title, b.title);
            QCOMPARE(a.usage.start, b.usage.start); QCOMPARE(a.usage.end, b.usage.end);
            QCOMPARE(a.definition.start, b.definition.start); QCOMPARE(a.destination.start, b.destination.start); QCOMPARE(a.destination.end, b.destination.end);
        }
        auto again = PreviewAnalysis::get(input, {}, metrics, &error); QCOMPARE(again.get(), result.get()); QVERIFY(metrics["analysisCacheHit"].toBool());
        auto changed = input; changed.utf8 += "\nchanged";
        auto different = PreviewAnalysis::get(changed, {}, metrics, &error); QVERIFY(different); QVERIFY(different.get() != result.get()); QVERIFY(!metrics["analysisCacheHit"].toBool());
        PreviewAnalysis::forget(input.documentId);
    }
    void concurrentRequestsShareOneResultAndReleaseAfterForget() {
        auto input = snapshot(QByteArray("# Shared\n\nparagraph **text**\n\n").repeated(2000));
        auto run = [input] { QJsonObject m; QString e; return PreviewAnalysis::get(input, {}, m, &e); };
        auto first = QtConcurrent::run(run), second = QtConcurrent::run(run);
        first.waitForFinished(); second.waitForFinished(); auto a = first.result(), b = second.result();
        QVERIFY(a); QCOMPARE(a.get(), b.get());
        std::weak_ptr<const PreviewAnalysis> weak = a;
        PreviewAnalysis::forget(input.documentId); a.reset(); b.reset(); first = {}; second = {};
        // Qt reports the future finished before the auto-deleted runnable drops
        // its own result storage. Wait for actual destruction, not that flag.
        QTRY_VERIFY_WITH_TIMEOUT(weak.expired(), 10000); QCOMPARE(PreviewAnalysis::cacheStatistics()["inFlight"].toInt(), 0);
    }
    void cacheIsBoundedAndCancelledRequestsDoNotPopulateIt() {
        QList<QUuid> documents;
        for (int i = 0; i < 38; ++i) {
            auto input = snapshot("# bounded " + QByteArray::number(i)); documents.append(input.documentId);
            QJsonObject m; QString e; QVERIFY(PreviewAnalysis::get(input, {}, m, &e));
            const auto stats = PreviewAnalysis::cacheStatistics(); QVERIFY(stats["entries"].toInt() <= 32); QVERIFY(stats["estimatedBytes"].toInt() <= stats["limitBytes"].toInt());
        }
        auto input = snapshot("cancelled"); QJsonObject m; QString e; auto token = std::make_shared<std::atomic_bool>(true);
        QVERIFY(!PreviewAnalysis::get(input, token, m, &e));
        auto huge = snapshot(QByteArray("a\n\n").repeated(600000));
        auto result = PreviewAnalysis::get(huge, {}, m, &e); QVERIFY2(result, qPrintable(e));
        QVERIFY(result->estimatedBytes > PreviewAnalysis::cacheStatistics()["limitBytes"].toInt());
        QVERIFY(PreviewAnalysis::cacheStatistics()["estimatedBytes"].toInt() <= PreviewAnalysis::cacheStatistics()["limitBytes"].toInt());
        for (const auto &document : documents) PreviewAnalysis::forget(document);
        PreviewAnalysis::forget(huge.documentId);
    }
    void changedImageOnlyDecodesItsOwnBytesAndRevocationDeniesCache() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QByteArray source("![one](one.png) ![two](two.png)");
        QVERIFY(fileFixtures::write(dir.filePath("doc.md"), source));
        QVERIFY(fileFixtures::write(dir.filePath("one.png"), image(Qt::red))); QVERIFY(fileFixtures::write(dir.filePath("two.png"), image(Qt::blue)));
        const auto probe = FileService::probe(dir.filePath("doc.md")); QVERIFY(probe.file);
        const auto root = WorkspaceResourceReader::open(dir.path()); QVERIFY(root.reader);
        auto input = snapshot(source); QJsonObject m; QString e;
        const auto parsed = PreviewAnalysis::get(input, {}, m, &e); QVERIFY(parsed);
        auto cache = std::make_shared<PreviewImageCache>();
        auto prepare = [&] { return preparePreviewImages(input, parsed->references, root.reader, "doc.md", probe.file->identity(), {}, {}, cache); };
        const auto first = prepare(); QCOMPARE(first.states, QByteArray(2, char(1))); QCOMPARE(first.decodedImages, 2);
        const auto second = prepare(); QCOMPARE(second.reusedImages, 2); QCOMPARE(second.decodedImages, 0);
        QVERIFY(fileFixtures::write(dir.filePath("one.png"), image(Qt::green)));
        const auto changed = prepare(); QCOMPARE(changed.decodedImages, 1); QCOMPARE(changed.reusedImages, 1);
        QCOMPARE(QImage::fromData(changed.png[0]).pixelColor(0, 0), QColor(Qt::green));
        auto stale = input; stale.utf8 += "different";
        QVERIFY(!preparePreviewImages(stale, parsed->references, root.reader, "doc.md", probe.file->identity(), {}, {}, cache).analyzed);
        qint64 pixels = MaxBatchImagePixels; QByteArray png; bool reused = false;
        QCOMPARE(cache->decode(image(Qt::blue), pixels, 1024 * 1024, png, reused), char(3));
        QVERIFY(cache->retainedBytes() <= 8 * 1024 * 1024);
        root.reader->revoke(); const auto denied = prepare(); QVERIFY(denied.png.isEmpty()); QCOMPARE(denied.states, QByteArray(2, char(5))); QCOMPARE(denied.reusedImages, 0);
        PreviewAnalysis::forget(input.documentId);
    }
    void resourceRefreshReusesSyntaxAndProjectionWithCurrentGenerations() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); Appearance appearance(nullptr, dir.filePath("appearance.json"));
        DocumentSession session("# Title\n\nbody\n"); MarkdownPreview preview(&session, nullptr, 24, &appearance); preview.resize(700, 500); preview.show();
        QVERIFY(ready(preview, session)); const auto original = session.snapshot().utf8; const auto before = preview.requestedGeneration();
        preview.refresh(); QVERIFY(ready(preview, session));
        QVERIFY(preview.requestedGeneration() > before); QVERIFY(preview.workStatistics()["analysisCacheHit"].toBool()); QVERIFY(preview.workStatistics()["projectionCacheHit"].toBool());
        const auto root = WorkspaceResourceReader::open(dir.path()); QVERIFY(root.reader);
        preview.setResourceContext(root.reader, "not-on-disk.md", "unverified"); QVERIFY(ready(preview, session));
        QVERIFY(preview.workStatistics()["analysisCacheHit"].toBool()); QCOMPARE(session.snapshot().utf8, original);
        const auto stats = preview.workStatistics();
        for (const char *stage : {"semanticMs", "referencesMs", "decodeMs", "resourcesMs", "resourceReadMs", "imageDecodeMs", "projectionMs", "layoutMs", "readyMs", "queueMs", "workerMs"}) QVERIFY2(stats.contains(stage) && stats[stage].toDouble(-1) >= 0, stage);
    }
    void burstEditingMultipleDocumentsAndCloseDiscardOldWork() {
        QTemporaryDir dir; Appearance appearance(nullptr, dir.filePath("appearance.json"));
        DocumentSession one(QByteArray("# one\n\nparagraph **body**\n\n").repeated(2000)), two("# second\n\nuntouched\n");
        auto first = std::make_unique<MarkdownPreview>(&one, nullptr, 24, &appearance);
        MarkdownPreview second(&two, nullptr, 24, &appearance); first->resize(700, 500); second.resize(700, 500); first->show(); second.show();
        for (int i = 0; i < 12; ++i) { QCOMPARE(one.replace(0, 0, QString::number(i) + "\n", one.revision()), EditResult::Applied); QApplication::processEvents(); first->setVisible(i % 2 == 0); second.setVisible(i % 2 != 0); }
        first->show(); second.show(); QVERIFY(ready(*first, one)); QVERIFY(ready(second, two));
        QCOMPARE(first->view()->content()->model.sourceDigest(), QCryptographicHash::hash(one.snapshot().utf8, QCryptographicHash::Sha256));
        QCOMPARE(two.snapshot().utf8, QByteArray("# second\n\nuntouched\n"));
        const auto id = one.documentId(); first->refresh(); first.reset();
        QTRY_COMPARE_WITH_TIMEOUT(PreviewAnalysis::cacheStatistics()["inFlight"].toInt(), 0, 20000);
        QJsonObject metrics; QString error; auto value = PreviewAnalysis::get(one.snapshot(), {}, metrics, &error); QVERIFY(value); QVERIFY(!metrics["analysisCacheHit"].toBool());
        PreviewAnalysis::forget(id);
    }
    void workspaceTabSwitchesKeepLatestUnsavedSession() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto firstPath = dir.filePath("one.md"), secondPath = dir.filePath("two.md");
        QVERIFY(fileFixtures::write(firstPath, "# First\n\nfirst\n")); QVERIFY(fileFixtures::write(secondPath, "# Second\n\nsecond\n"));
        FileApplication app(nullptr, dir.filePath("settings.json")); auto *workspace = app.workspaceWindow(dir.path()); QVERIFY(workspace);
        app.openInWorkspace(firstPath, workspace); QTRY_VERIFY(workspace->activePane()); auto *one = workspace->activePane();
        app.openInWorkspace(secondPath, workspace); QTRY_VERIFY(workspace->activePane() != one); auto *two = workspace->activePane(); QVERIFY(two);
        auto *session = one->document()->session();
        for (int i = 0; i < 10; ++i) {
            workspace->activatePane(one->paneId()); QCOMPARE(workspace->activePane(), one);
            QCOMPARE(session->replace(0, 0, QString::number(i) + "\n", session->revision()), EditResult::Applied);
            QApplication::processEvents(); workspace->activatePane(two->paneId()); QCOMPARE(workspace->activePane(), two);
        }
        workspace->activatePane(one->paneId()); one->setViewMode(SingleFileWindow::ViewMode::Split);
        QVERIFY(ready(*one->preview(), *session));
        const auto source = session->snapshot().utf8;
        QCOMPARE(one->preview()->view()->content()->model.sourceDigest(), QCryptographicHash::hash(source, QCryptographicHash::Sha256));
        QCOMPARE(two->document()->session()->snapshot().utf8, QByteArray("# Second\n\nsecond\n"));
        QCOMPARE(session->undo(), EditResult::Applied); QVERIFY(ready(*one->preview(), *session)); QVERIFY(session->snapshot().utf8 != source);
    }
};
int main(int argc, char **argv) { initializeGuiRuntime(); QApplication app(argc, argv); configureGuiApplication(); app.setQuitOnLastWindowClosed(false); SourceEditor::installAccessibility(); PreviewPipelineTests tests; return QTest::qExec(&tests, argc, argv); }
#include "preview_pipeline_tests.moc"
