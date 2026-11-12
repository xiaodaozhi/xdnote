// Diagnostic-only worker: generated inputs, no product windows or user files.
#include "Appearance.h"
#include "DocumentSession.h"
#include "FileService.h"
#include "GuiRuntime.h"
#include "ImageAdmission.h"
#include "MarkdownBridge.h"
#include "MarkdownPreview.h"
#include "WorkspaceReferenceScan.h"
#include <QApplication>
#include <QBuffer>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFontMetrics>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <cstdio>
#include <functional>
#include <stdexcept>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

using namespace xdnote;
namespace {
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
void write(const QString &path, const QByteArray &bytes) {
    QFile file(path); require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "fixture write failed");
}
QByteArray sample(int bytes, bool dense, bool references = false) {
    const QByteArray block = dense ? QByteArray("[label][ref] [label][ref]\n\n[ref]: target.md \"title\"\n\n")
        : (QStringLiteral("# 标题 heading\n\n中文、emoji 😀，**粗体**。\n\n- [x] item\n\n|列|值|\n|--|--|\n|甲|乙|\n\n```cpp\nint x = 1;\n```\n\n")
           + QStringLiteral("这是用于测量长文处理的自有正文样本，保留文字、空格与换行。 ").repeated(8)
           + (references ? QStringLiteral("\n\n[段落链接][shared] [行内链接](docs/target.md#标题) ![占位](missing.png)\n\n[shared]: docs/target.md \"标题\"\n\n") : QStringLiteral("\n\n"))).toUtf8();
    QByteArray result; result.reserve(bytes);
    while (result.size() + block.size() <= bytes) result += block;
    result += QByteArray(bytes - result.size(), 'x');
    return result;
}
double peakMiB() {
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS info{}; info.cb = sizeof(info);
    require(GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info)), "cannot measure process memory");
    return double(info.PeakWorkingSetSize) / (1024 * 1024);
#else
    rusage info{}; require(getrusage(RUSAGE_SELF, &info) == 0, "cannot measure process memory");
#ifdef Q_OS_MACOS
    return double(info.ru_maxrss) / (1024 * 1024);
#else
    return double(info.ru_maxrss) / 1024;
#endif
#endif
}
QJsonObject measure(const std::function<void()> &operation) {
    const bool progress = qEnvironmentVariableIsSet("XDNOTE_PROBE_PROGRESS");
    QElapsedTimer warm; warm.start();
    if (progress) { std::fprintf(stderr, "measurement warmup started\n"); std::fflush(stderr); }
    operation(); // Warm-up is checked, but excluded from timing quantiles.
    if (progress) { std::fprintf(stderr, "measurement warmup completed: %.3f ms\n", warm.nsecsElapsed() / 1e6); std::fflush(stderr); }
    QList<double> values;
    for (int i = 0; i < 7; ++i) {
        if (progress) { std::fprintf(stderr, "measurement sample %d started\n", i + 1); std::fflush(stderr); }
        QElapsedTimer timer; timer.start(); operation(); values.append(timer.nsecsElapsed() / 1e6);
        if (progress) { std::fprintf(stderr, "measurement sample %d completed: %.3f ms\n", i + 1, values.last()); std::fflush(stderr); }
    }
    auto sorted = values; std::sort(sorted.begin(), sorted.end()); QJsonArray runs;
    for (double value : values) runs.append(value);
    return {{"samplesMs", runs}, {"p50Ms", sorted[3]}, {"p95Ms", sorted[6]}}; // nearest rank, n=7
}
QJsonObject pipeline(const QByteArray &source) {
    QTemporaryDir dir; require(dir.isValid(), "pipeline temporary directory missing");
    Appearance appearance(nullptr, dir.filePath("appearance.json"));
    DocumentSession one(source), two("# Other document\n\nunchanged\n");
    auto first = std::make_unique<MarkdownPreview>(&one, nullptr, 24, &appearance);
    auto other = std::make_unique<MarkdownPreview>(&two, nullptr, 24, &appearance);
    first->resize(800, 600); other->resize(800, 600); first->show(); other->show();
    auto await = [](MarkdownPreview &view, DocumentSession &session) {
        QElapsedTimer elapsed; elapsed.start();
        while (elapsed.elapsed() < 20000) {
            QTest::qWait(5);
            if (view.view()->ready() && view.displayedRevision() == session.revision()
                && !view.workStatistics()["running"].toBool() && !view.workStatistics()["pending"].toBool()) return;
        }
        throw std::runtime_error("pipeline did not publish latest revision");
    };
    await(*first, one); await(*other, two); const auto cold = first->workStatistics();
    first->refresh(); await(*first, one); const auto reused = first->workStatistics();
    require(reused["analysisCacheHit"].toBool() && reused["projectionCacheHit"].toBool(), "unchanged preview recomputed syntax/projection");
    // Prefix replacement leaves even a capacity-limit source within its budget.
    for (int i = 0; i < 8; ++i) {
        require(one.replace(0, 1, QString::number(i), one.revision()) == EditResult::Applied, "burst edit failed");
        QApplication::processEvents(); first->setVisible(i % 2 == 0); other->setVisible(i % 2 != 0);
    }
    first->show(); other->show(); await(*first, one); await(*other, two);
    const auto burst = first->workStatistics();
    require(first->view()->content()->model.sourceDigest() == QCryptographicHash::hash(one.snapshot().utf8, QCryptographicHash::Sha256), "obsolete preview published");
    require(two.snapshot().utf8 == "# Other document\n\nunchanged\n", "document crossover");
    first->refresh(); first.reset(); other.reset();
    QElapsedTimer close; close.start();
    while (PreviewAnalysis::cacheStatistics()["inFlight"].toInt() && close.elapsed() < 20000) QTest::qWait(5);
    const auto retained = PreviewAnalysis::cacheStatistics();
    require(retained["inFlight"].toInt() == 0 && retained["entries"].toInt() == 0, "closed pipeline retained work or analysis");
    return {{"cold", cold}, {"reused", reused}, {"burst", burst}, {"afterClose", retained}, {"closeDrainMs", close.nsecsElapsed() / 1e6}};
}
QJsonObject preview(const QString &theme, const QString &evidence) {
    QTemporaryDir temporary; require(temporary.isValid(), "fixture directory failed");
    Appearance appearance(nullptr, temporary.filePath("appearance.json"));
    require(appearance.setPreferences(theme, "graphite"), "cannot configure fixture appearance");
    const auto text = QStringLiteral("# 排版 Typography\n\n中文与 Latin 😀，**粗体**、*斜体*、~~删除~~。\n\n- [x] 已完成\n- [ ] 待办\n\n> 引用内容\n\n```js\nconst text = '<script>中文😀</script>';\n%1\n```\n\n|列|内容|\n|--|--|\n|甲|%2|\n\n[外链](https://example.invalid) ![网络图片](https://example.invalid/a.png)\n<script>window.documentInjected=true</script>\n").arg(QString(240, 'x'), QString(240, 'y')).toUtf8();
    DocumentSession session(text); MarkdownPreview widget(&session, nullptr, 24, &appearance); widget.resize(800, 600);
    QSignalSpy ready(&widget, &MarkdownPreview::previewReady), errors(&widget, &MarkdownPreview::previewError);
    QElapsedTimer timer; timer.start(); widget.show();
    while (errors.isEmpty() && timer.elapsed() < 20000) {
        QTest::qWait(10);
        if (ready.isEmpty() || !widget.view()->ready()) continue;
        widget.grab(); QApplication::processEvents();
        if (widget.view()->ready()) break;
    }
    require(!ready.isEmpty() && widget.view()->ready() && errors.isEmpty(), "offscreen preview did not become ready");
    const double readyMs = timer.nsecsElapsed() / 1e6;
    const auto *content = widget.view()->content();
    require(content && widget.view()->ready(), "native content is not ready");
    require(content->text.contains(QStringLiteral("排版 Typography")), "heading missing");
    require(content->text.contains(QStringLiteral("<script>window.documentInjected=true</script>")), "HTML literal lost");
    int tasks = 0, code = 0, tables = 0;
    for (quint32 i = 0; i < content->model.nodeCount(); ++i) {
        const auto node = content->model.node(i);
        if (node.kind == NativePreviewModel::Kind::TaskMarker) ++tasks;
        if (node.kind == NativePreviewModel::Kind::Table) ++tables;
        if (node.kind == NativePreviewModel::Kind::CodeBlock) {
            ++code;
        }
    }
    require(tasks == 2 && code == 1 && tables == 1, "native structure mismatch");
    for (const auto &owner : content->owners) if (owner.code)
        require(content->text.mid(owner.start, owner.length) == QStringLiteral("const text = '<script>中文😀</script>';\n") + QString(240, 'x') + '\n', "code literal changed");
    QString copied;
    QObject::connect(widget.view(), &NativePreviewView::copyTextRequested, &widget, [&](const QString &value) { copied = value; });
    widget.view()->selectAll(); widget.view()->copy();
    require(copied == content->text, "native selection/copy mismatch");
    const auto stats = widget.view()->statistics();
    require(stats.peakCachedDocuments <= 48 && stats.peakCachedEstimatedBytes <= 8 * 1024 * 1024, "native layout cache limit");
    QJsonObject result{{"tasks", tasks}, {"codeBlocks", code}, {"tables", tables}, {"copyExact", true}, {"htmlLiteral", true},
                       {"sourceSha256", QString::fromLatin1(QCryptographicHash::hash(text, QCryptographicHash::Sha256).toHex())},
                       {"sourceBytes", text.size()}, {"peakCachedDocuments", stats.peakCachedDocuments}, {"peakCachedEstimatedBytes", stats.peakCachedEstimatedBytes}};
    const QFontMetrics metrics(widget.font()); require(metrics.inFontUcs4(0x5199) && metrics.inFontUcs4('M'), "missing Chinese or Latin font");
    require(session.snapshot().utf8 == text && !session.isDirty(), "preview changed source");
    require(!evidence.isEmpty() && widget.grab().save(evidence), "cannot save offscreen preview evidence");
    result.insert("readyMs", readyMs); result.insert("theme", theme); return result;
}
}
int main(int argc, char **argv) {
    initializeGuiRuntime(); QApplication app(argc, argv); configureGuiApplication();
    app.setApplicationName("xdnote-background-probe"); app.setQuitOnLastWindowClosed(false);
    QJsonObject result;
    try {
        const auto args = app.arguments(); require(args.size() >= 4, "expected operation, byte count and variant");
        const auto operation = args[1]; bool valid = false; const int bytes = args[2].toInt(&valid);
        require(valid && bytes >= 0 && bytes <= 10 * 1024 * 1024, "invalid bounded input size");
        const bool dense = args[3] == "dense"; const auto source = sample(bytes, dense, operation == "pipeline");
        result = {{"operation", operation}, {"bytes", bytes}, {"variant", args[3]}, {"sourceSha256", QString::fromLatin1(QCryptographicHash::hash(source, QCryptographicHash::Sha256).toHex())}};
        if (operation == "pipeline") {
            QJsonArray stages; int iteration = -1;
            result.insert("expected", "success");
            result.insert("timing", measure([&] {
                auto record = pipeline(source); if (iteration++ >= 0) stages.append(record);
            }));
            result.insert("stages", stages);
        } else if (operation == "preview") {
            require(args.size() == 5, "preview requires evidence path"); const auto record = preview(args[3], args[4]);
            result.insert("sourceSha256", record.value("sourceSha256")); result.insert("preview", record);
        } else if (operation == "model") {
            const bool rejected = bytes > FileService::SourceLimit;
            result.insert("expected", rejected ? "source-limit" : "success");
            result.insert("timing", measure([&] {
                QString error; auto model = NativePreviewModel::parse(source, &error);
                if (rejected) { require(!model && !error.isEmpty(), "oversize native model admitted"); return; }
                require(bool(model), "native model parse failed");
                auto content = NativePreviewContent::build(std::move(*model), {}, &error);
                require(bool(content), "native projection failed");
            }));
        } else if (operation == "render" || operation == "references" || operation == "headings") {
            auto function = operation == "render" ? xdnote_markdown_render : operation == "references" ? xdnote_markdown_references : xdnote_markdown_headings;
            const bool sourceLimit = bytes > FileService::SourceLimit;
            const bool complexityLimit = dense && operation == "references";
            const bool rejected = sourceLimit || complexityLimit;
            result.insert("expected", sourceLimit ? "source-limit" : complexityLimit ? "bounded-complexity-rejection" : "success");
            result.insert("timing", measure([&] {
                auto buffer = function(reinterpret_cast<const unsigned char *>(source.constData()), size_t(source.size()));
                const auto status = buffer.status; const auto size = buffer.length; xdnote_markdown_free(&buffer);
                require(rejected ? status == 2 : status == 0 && size > 0, "unexpected parser result");
            }));
        } else if (operation == "session") {
            require(bytes <= FileService::SourceLimit, "session input exceeds admission"); DocumentSession session(source);
            if (bytes == FileService::SourceLimit)
                require(session.replace(0, 0, "x", session.revision()) == EditResult::InvalidText && session.snapshot().utf8 == source,
                        "oversize edit did not preserve the source");
            result.insert("timing", measure([&] {
                require(session.replace(0, 1, "X", session.revision()) == EditResult::Applied, "edit failed");
                require(session.undo() == EditResult::Applied && session.snapshot().utf8 == source, "undo/source mismatch");
            }));
        } else if (operation == "file") {
            QTemporaryDir dir; require(dir.isValid(), "fixture directory failed"); const auto path = dir.filePath("generated.md"); write(path, source);
            const bool rejected = bytes > FileService::SourceLimit; result.insert("expected", rejected ? "source-limit" : "success");
            result.insert("timing", measure([&] {
                const auto probe = FileService::probe(path); require(bool(probe.file), "file probe failed");
                const auto read = FileService::read({probe.file});
                if (rejected) { require(read.problem.code == FileError::TooLarge && !read.baseline, "oversize file admitted"); return; }
                require(read.baseline && read.baseline->utf8() == source, "read/source mismatch");
                FileSaveRequest request; request.baseline = read.baseline; request.path = path; request.utf8 = source;
                const auto saved = FileService::save(request); require(saved.status == SaveStatus::Committed && saved.baseline && saved.baseline->utf8() == source, "atomic save failed");
            }));
        } else if (operation == "image") {
            QImage image(3000, 2000, QImage::Format_RGB32); image.fill(Qt::blue); QByteArray encoded; QBuffer buffer(&encoded);
            require(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"), "image fixture failed"); image = {};
            result.insert("sourceSha256", QString::fromLatin1(QCryptographicHash::hash(encoded, QCryptographicHash::Sha256).toHex()));
            result.insert("hashScope", "generated encoded PNG");
            result.insert("pixels", 6000000); result.insert("timing", measure([&] {
                qint64 pixels = 0; QByteArray png; require(decodeImage(encoded, pixels, MaxImageSourceBytes, png) == 1 && pixels == 6000000 && !png.isEmpty(), "image decode failed");
            }));
        } else if (operation == "scan" || operation == "cancel") {
            QTemporaryDir dir; require(dir.isValid(), "fixture directory failed");
            QCryptographicHash inputs(QCryptographicHash::Sha256);
            for (int i = 0; i < 128; ++i) {
                const auto name = QString("doc%1.md").arg(i); const auto bytes = "[next](doc0.md)\n" + sample(10240, false);
                write(dir.filePath(name), bytes); inputs.addData(name.toUtf8()); inputs.addData(QByteArray(1, '\0')); inputs.addData(bytes);
            }
            result.insert("sourceSha256", QString::fromLatin1(inputs.result().toHex()));
            result.insert("hashScope", "128 ordered relative paths, NUL delimiters and UTF-8 file contents");
            const auto root = WorkspaceResourceReader::open(dir.path()); require(bool(root.reader), "root open failed");
            const auto cancelled = std::make_shared<std::atomic_bool>(operation == "cancel");
            ReferenceIndexInput input; input.rootId = QUuid::createUuid(); input.rootGeneration = 1; input.scanGeneration = 1;
            result.insert("documents", 128); result.insert("timing", measure([&] {
                const auto scanned = scanWorkspaceReferences(root.reader, input, {}, cancelled);
                if (operation == "cancel") require(scanned.cancelled && scanned.index.sources.isEmpty(), "cancel did not discard sources");
                else require(scanned.index.complete && scanned.readDocuments == 128 && scanned.index.references.size() == 128, "incomplete workspace scan");
            }));
        } else throw std::runtime_error("unknown operation");
        result.insert("peakProcessMiB", peakMiB()); result.insert("ok", true);
    } catch (const std::exception &error) { result.insert("ok", false); result.insert("error", error.what()); }
    const auto output = QJsonDocument(result).toJson(QJsonDocument::Compact); std::puts(output.constData());
    return result.value("ok").toBool() ? 0 : 1;
}
