#include "spike_document.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickWindow>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTextCursor>

#include <memory>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace {
std::unique_ptr<QTemporaryDir> fixtureDirectory()
{
    const QString executableDir = QDir(QCoreApplication::applicationDirPath()).canonicalPath();
    if (executableDir.isEmpty())
        return {};
    const QString requestedRoot = executableDir + QStringLiteral("/benchmark-data");
    if (QFileInfo(requestedRoot).isSymLink() || !QDir().mkpath(requestedRoot))
        return {};
    const QString root = QDir(requestedRoot).canonicalPath();
    if (!root.startsWith(executableDir + QLatin1Char('/')))
        return {};
    auto temporary = std::make_unique<QTemporaryDir>(root + QStringLiteral("/fixture-XXXXXX"));
    temporary->setAutoRemove(false);
    if (!temporary->isValid())
        return {};
    if (!QDir(temporary->path()).canonicalPath().startsWith(root + QStringLiteral("/fixture-")))
        return {};
    temporary->setAutoRemove(true);
    return temporary;
}

QByteArray makeSample(qsizetype size)
{
    QByteArray chunk = QStringLiteral("# 标题\n\n- [x] 笔记\n- [ ] 示例\n\n```text\n").toUtf8();
    chunk += QByteArray(2048, 'x');
    chunk += "\n```\n\n";
    QByteArray sample;
    sample.reserve(size);
    while (sample.size() + chunk.size() <= size)
        sample += chunk;
    sample += QByteArray(size - sample.size(), 'x');
    return sample;
}

QJsonObject processMemory()
{
    QJsonObject result;
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS_EX counters = {};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                             sizeof(counters))) {
        result.insert(QStringLiteral("workingSetBytes"), double(counters.WorkingSetSize));
        result.insert(QStringLiteral("peakWorkingSetBytes"), double(counters.PeakWorkingSetSize));
        result.insert(QStringLiteral("privateBytes"), double(counters.PrivateUsage));
    }
#endif
    return result;
}

bool validReportPath(const QString &requested)
{
    const QString executableDir = QDir(QCoreApplication::applicationDirPath()).canonicalPath();
    const QString runRoot = QDir(executableDir + QStringLiteral("/runs")).canonicalPath();
    const QFileInfo report(requested);
    const QString parent = QDir(report.absolutePath()).canonicalPath();
    return !executableDir.isEmpty() && !runRoot.isEmpty()
        && runRoot.startsWith(executableDir + QLatin1Char('/'))
        && parent.startsWith(runRoot + QLatin1Char('/'))
        && !report.exists() && !report.isSymLink();
}

double elapsedMilliseconds(const QElapsedTimer &timer)
{
    return double(timer.nsecsElapsed()) / 1000000.0;
}
} // namespace

int main(int argc, char **argv)
{
    QElapsedTimer timer;
    timer.start();
    QGuiApplication app(argc, argv);
    const double appInitMs = elapsedMilliseconds(timer);

    QCommandLineParser parser;
    parser.addOption(QCommandLineOption(QStringLiteral("size"), QStringLiteral("Sample bytes"), QStringLiteral("bytes")));
    parser.addOption(QCommandLineOption(QStringLiteral("output"), QStringLiteral("New report file inside build runs"), QStringLiteral("path")));
    parser.process(app);
    bool sizeOk = false;
    const qsizetype size = parser.value(QStringLiteral("size")).toLongLong(&sizeOk);
    const QString report = parser.value(QStringLiteral("output"));
    if (!sizeOk || (size != 10240 && size != 1048576 && size != 10485760)
        || !validReportPath(report)) {
        qCritical("Unsupported sample size or report path");
        return 2;
    }

    auto temporary = fixtureDirectory();
    if (!temporary) {
        qCritical("Could not create a verified test-owned fixture directory");
        return 3;
    }
    const QByteArray sample = makeSample(size);
    const QString path = temporary->filePath(QStringLiteral("benchmark.md"));
    QFile fixture(path);
    if (!fixture.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        || fixture.write(sample) != sample.size() || !fixture.flush()) {
        qCritical("Could not create benchmark fixture");
        return 4;
    }
    fixture.close();

    timer.restart();
    QQuickWindow window;
    window.create();
    if (!window.handle()) {
        qCritical("Could not create a native window handle");
        return 5;
    }
    const double hiddenWindowMs = elapsedMilliseconds(timer);

    SpikeSession session;
    QString error;
    timer.restart();
    if (!session.open(path, &error)) {
        qCritical().noquote() << "Fixture load failed:" << error;
        return 6;
    }
    const double loadMs = elapsedMilliseconds(timer);

    QTextCursor cursor(session.document());
    cursor.movePosition(QTextCursor::End);
    timer.restart();
    cursor.insertText(QStringLiteral("x"));
    const double documentEditMs = elapsedMilliseconds(timer);
    if (!session.isDirty()) {
        qCritical("Benchmark edit was not observed");
        return 7;
    }

    const QString source = QString::fromUtf8(sample) + QStringLiteral("x");
    RestrictedPreview preview;
    timer.restart();
    preview.render(source);
    const double parseMs = elapsedMilliseconds(timer);
    timer.restart();
    preview.setTextWidth(720);
    const QSizeF previewSize = preview.size();
    const double layoutMs = elapsedMilliseconds(timer);
    if (previewSize.isEmpty()) {
        qCritical("Benchmark preview layout is empty");
        return 8;
    }

    QJsonObject result{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("scope"), QStringLiteral("single-synthetic-sample-not-product-acceptance")},
        {QStringLiteral("sampleBytes"), double(size)},
        {QStringLiteral("qtVersion"), QString::fromLatin1(qVersion())},
        {QStringLiteral("platformPlugin"), QGuiApplication::platformName()},
        {QStringLiteral("os"), QSysInfo::prettyProductName()},
        {QStringLiteral("cpuArchitecture"), QSysInfo::currentCpuArchitecture()},
        {QStringLiteral("timingMs"), QJsonObject{
             {QStringLiteral("guiApplicationInit"), appInitMs},
             {QStringLiteral("hiddenWindowCreate"), hiddenWindowMs},
             {QStringLiteral("fileOpenAndDocumentLoad"), loadMs},
             {QStringLiteral("documentCursorInsert"), documentEditMs},
             {QStringLiteral("markdownParse"), parseMs},
             {QStringLiteral("previewLayout"), layoutMs}}},
        {QStringLiteral("processMemory"), processMemory()},
        {QStringLiteral("note"), QStringLiteral("Hidden window; cursor insertion is not native keyboard/IME latency. One process and one sample; no performance threshold.")},
    };
    QFile output(report);
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        qCritical("Could not create benchmark report");
        return 9;
    }
    const QByteArray json = QJsonDocument(result).toJson(QJsonDocument::Indented);
    if (output.write(json) != json.size() || !output.flush()) {
        qCritical("Could not write complete benchmark report");
        return 10;
    }
    return 0;
}
