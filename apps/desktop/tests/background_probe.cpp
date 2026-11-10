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
#include <QWebEnginePage>
#include <QWebEngineScript>
#include <QWebEngineView>
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
QByteArray sample(int bytes, bool dense) {
    const QByteArray block = dense ? QByteArray("[label][ref] [label][ref]\n\n[ref]: target.md \"title\"\n\n")
        : (QStringLiteral("# 标题 heading\n\n中文、emoji 😀，**粗体**。\n\n- [x] item\n\n|列|值|\n|--|--|\n|甲|乙|\n\n```cpp\nint x = 1;\n```\n\n")
           + QStringLiteral("这是用于测量长文处理的自有正文样本，保留文字、空格与换行。 ").repeated(8) + "\n\n").toUtf8();
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
    operation(); // Warm-up is checked, but excluded from timing quantiles.
    QList<double> values;
    for (int i = 0; i < 7; ++i) { QElapsedTimer timer; timer.start(); operation(); values.append(timer.nsecsElapsed() / 1e6); }
    auto sorted = values; std::sort(sorted.begin(), sorted.end()); QJsonArray runs;
    for (double value : values) runs.append(value);
    return {{"samplesMs", runs}, {"p50Ms", sorted[3]}, {"p95Ms", sorted[6]}}; // nearest rank, n=7
}
QJsonObject preview(const QString &theme, const QString &evidence) {
    QTemporaryDir temporary; require(temporary.isValid(), "fixture directory failed");
    Appearance appearance(nullptr, temporary.filePath("appearance.json"));
    require(appearance.setPreferences(theme, "graphite"), "cannot configure fixture appearance");
    const auto text = QStringLiteral("# 排版 Typography\n\n中文与 Latin 😀，**粗体**、*斜体*、~~删除~~。\n\n- [x] 已完成\n- [ ] 待办\n\n> 引用内容\n\n```js\nconst text = '<script>中文😀</script>';\n%1\n```\n\n|列|内容|\n|--|--|\n|甲|%2|\n\n[外链](https://example.invalid) ![网络图片](https://example.invalid/a.png)\n<script>window.documentInjected=true</script>\n").arg(QString(240, 'x'), QString(240, 'y')).toUtf8();
    DocumentSession session(text); MarkdownPreview widget(&session, nullptr, 24, &appearance); widget.resize(800, 600);
    QSignalSpy ready(&widget, &MarkdownPreview::previewReady), errors(&widget, &MarkdownPreview::previewError);
    QElapsedTimer timer; timer.start(); widget.show();
    while (ready.isEmpty() && errors.isEmpty() && timer.elapsed() < 20000) QTest::qWait(10);
    require(!ready.isEmpty() && errors.isEmpty(), "offscreen preview did not become ready");
    const double readyMs = timer.nsecsElapsed() / 1e6;
    bool received = false; QVariant details;
    widget.view()->page()->runJavaScript(QStringLiteral(R"JS((()=>{
      const pre=document.querySelector('pre'),table=document.querySelector('.table-scroll');
      return {codeExact:document.querySelector('pre code').textContent===%1,
        codeOverflow:pre.scrollWidth>pre.clientWidth,tableOverflow:table.scrollWidth>table.clientWidth,
        tasks:document.querySelectorAll('input[type=checkbox]').length,
        escaped:document.body.textContent.includes('<script>window.documentInjected=true</script>'),
        noInjectedScript:window.documentInjected===undefined,noRemote:!document.querySelector('img[src^="http"],a[href^="http"]'),
        font:getComputedStyle(document.body).fontFamily,background:getComputedStyle(document.body).backgroundColor,
        hasHeading:document.querySelector('h1').textContent==='排版 Typography'};
    })())JS").arg(QString::fromUtf8(QJsonDocument(QJsonArray{QStringLiteral("const text = '<script>中文😀</script>';\n") + QString(240, 'x') + '\n'}).toJson(QJsonDocument::Compact)).mid(1).chopped(1)),
        QWebEngineScript::ApplicationWorld, [&](const QVariant &value) { details = value; received = true; });
    timer.restart(); while (!received && timer.elapsed() < 10000) QTest::qWait(10);
    require(received, "preview DOM inspection timed out"); auto result = QJsonObject::fromVariantMap(details.toMap());
    for (const auto *name : {"codeExact", "codeOverflow", "tableOverflow", "escaped", "noInjectedScript", "noRemote", "hasHeading"})
        require(result.value(name).toBool(), name);
    require(result.value("tasks").toInt() == 2, "task list mismatch");
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
        const bool dense = args[3] == "dense"; const auto source = sample(bytes, dense);
        result = {{"operation", operation}, {"bytes", bytes}, {"variant", args[3]}, {"sourceSha256", QString::fromLatin1(QCryptographicHash::hash(source, QCryptographicHash::Sha256).toHex())}};
        if (operation == "preview") {
            require(args.size() == 5, "preview requires evidence path"); result.insert("preview", preview(args[3], args[4]));
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
            result.insert("pixels", 6000000); result.insert("timing", measure([&] {
                qint64 pixels = 0; QByteArray png; require(decodeImage(encoded, pixels, MaxImageSourceBytes, png) == 1 && pixels == 6000000 && !png.isEmpty(), "image decode failed");
            }));
        } else if (operation == "scan" || operation == "cancel") {
            QTemporaryDir dir; require(dir.isValid(), "fixture directory failed");
            for (int i = 0; i < 128; ++i) write(dir.filePath(QString("doc%1.md").arg(i)), "[next](doc0.md)\n" + sample(10240, false));
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
