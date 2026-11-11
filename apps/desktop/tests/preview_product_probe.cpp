#include "FileApplication.h"
#include "SingleFileWindow.h"
#include "MarkdownPreview.h"
#include "GuiRuntime.h"
#include "SourceEditor.h"
#include "preview_fixtures.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <cstdio>

using namespace xdnote;
int main(int argc, char **argv) {
    initializeGuiRuntime(); QApplication app(argc, argv); configureGuiApplication(); SourceEditor::installAccessibility(); app.setQuitOnLastWindowClosed(false);
    const auto args = app.arguments(); if (args.size() != 4) return 2;
    bool valid = false; const int size = args[1].toInt(&valid);
    if (!valid || size < 10240 || size > 2 * 1024 * 1024) return 2;
    QTemporaryDir temporary; if (!temporary.isValid()) return 3;
    const auto source = previewFixtures::fixture(size, args[2]);
    const auto path = temporary.filePath("sample.md"); QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) || file.write(source) != source.size()) return 3;
    file.close(); QElapsedTimer timer; timer.start();
    FileApplication application(nullptr, temporary.filePath("settings/state.ini")); application.open(path);
    QPointer<SingleFileWindow> window; bool ready = false;
    while (timer.elapsed() < 60000) {
        app.processEvents();
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *candidate = qobject_cast<SingleFileWindow *>(widget))
                if (candidate->document() && candidate->document()->path() == path) window = candidate;
        if (window && window->preview() && window->preview()->view()->content()
            && window->preview()->view()->ready()) { ready = true; break; }
        QThread::msleep(1);
    }
    const bool saved = ready && window->grab().save(args[3]);
    if (!saved) {
        const QJsonObject error{{"ok", false}, {"ready", ready}, {"saved", saved}, {"windows", QApplication::topLevelWidgets().size()}, {"matchedWindow", !window.isNull()}, {"elapsedMs", timer.elapsed()}, {"previewError", window && window->preview() ? window->preview()->view()->error() : QString()}};
        std::puts(QJsonDocument(error).toJson(QJsonDocument::Compact).constData()); return 4;
    }
    const auto opened = timer.elapsed();
    const QJsonObject result{{"ok", true}, {"fixture", args[2]}, {"sourceBytes", source.size()}, {"firstObservedFrameMs", opened},
        {"nativeImages", window->preview()->view()->admittedImageCount()}, {"scope", "real standalone window and file/session route; offscreen synthetic fixture; not desktop acceptance"}};
    QTimer::singleShot(300, &app, &QCoreApplication::quit); app.exec();
    std::puts(QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
    return 0;
}
