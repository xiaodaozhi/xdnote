#include "FileApplication.h"
#include "GuiRuntime.h"
#include "SourceEditor.h"
#include "SingleFileWindow.h"
#include "WorkspaceWindow.h"
#include "SingleInstance.h"
#include "MarkdownPreview.h"
#include "VisualTokens.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QThread>
#include <functional>
#include <stdexcept>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

using namespace xdnote;
namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
bool waitFor(const std::function<bool()> &predicate, int timeout = 30000) {
    QElapsedTimer clock; clock.start();
    do { qApp->processEvents(); if (predicate()) return true; QThread::msleep(2); } while (clock.elapsed() < timeout);
    return false;
}
QJsonObject memory() {
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS_EX counters{}; counters.cb = sizeof(counters);
    require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters), sizeof(counters)), "process memory query failed");
    return {{"workingSetBytes", static_cast<qint64>(counters.WorkingSetSize)},
            {"privateCommitBytes", static_cast<qint64>(counters.PrivateUsage)},
            {"peakWorkingSetBytes", static_cast<qint64>(counters.PeakWorkingSetSize)}};
#else
    return {{"unsupported", "current process memory collection is implemented on Windows only"}};
#endif
}
void writeJson(const QString &path, const QJsonObject &value) {
    QSaveFile file(path); require(file.open(QIODevice::WriteOnly), "cannot create evidence");
    const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Indented);
    require(file.write(bytes) == bytes.size() && file.commit(), "cannot commit evidence");
}
bool previewReady(SingleFileWindow *pane) {
    if (!pane->document() || pane->document()->loading()) return false;
    if (!pane->isVisible()) return true; // Inactive tabs keep sessions, not visible layouts.
    if (pane->viewMode() == SingleFileWindow::ViewMode::Source) return true;
    if (!pane->preview()->view()->content()) return false;
    pane->grab(); // Controlled Qt paint; no desktop capture or input injection.
    return pane->preview()->view()->ready() && pane->preview()->mappingCurrent();
}
}
int main(int argc, char **argv) {
    QElapsedTimer startup; startup.start();
    initializeGuiRuntime(); QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("xdnote")); app.setOrganizationName(QStringLiteral("xiaodaozhi"));
    configureGuiApplication(); SourceEditor::installAccessibility(); visualTokens();
    app.setWindowIcon(QIcon(QStringLiteral(":/xdnote/design/assets/ui-v2/brand/app-icon.svg")));
    app.setQuitOnLastWindowClosed(false);
    const auto args = app.arguments(); if (args.size() != 5) return 2;
    const QString scenario = args[1], root = args[2], readyFile = args[3], resultFile = args[4];
    QJsonObject result{{"ok", false}, {"scenario", scenario}, {"platformPlugin", QGuiApplication::platformName()},
        {"scope", "actual application services and windows; isolated fixtures; hidden native Qt paint; excludes compositor, user input, IME and external processes"}};
    try {
        require(QStringList{"empty", "reading", "editing", "split", "multitab"}.contains(scenario), "unknown scenario");
        require(QFileInfo(root + "/fixture.json").isFile(), "explicit fixture root required");
        require(QGuiApplication::platformName() == "windows", "Windows native QPA required; offscreen is not a whole-app native sample");
        result["guiInitializationMs"] = startup.elapsed();
        SingleInstance instance;
        require(instance.start(root + "/ipc", {}, [](const QStringList &) {}) == SingleInstance::Result::Primary, "isolated IPC failed");
        FileApplication application(nullptr, root + "/config/settings.json", FileApplication::Presentation::HiddenNative);
        QPointer<WorkspaceWindow> workspace;
        if (scenario == "empty" || scenario == "split" || scenario == "multitab") workspace = application.workspaceWindow(scenario == "empty" ? QString() : root);
        const auto files = QDir(root).entryList({"*.md"}, QDir::Files, QDir::Name);
        const int expected = scenario == "empty" ? 0 : scenario == "multitab" ? 8 : 1;
        require(files.size() == expected, "fixture file count mismatch");
        for (const auto &name : files) {
            if (workspace) application.openInWorkspace(root + '/' + name, workspace);
            else application.open(root + '/' + name);
        }
        QList<SingleFileWindow *> panes;
        require(waitFor([&] {
            panes.clear();
            if (workspace) panes = workspace->panes();
            else for (auto *widget : QApplication::topLevelWidgets())
                if (auto *pane = qobject_cast<SingleFileWindow *>(widget)) if (pane->document()) panes.append(pane);
            if (panes.size() != expected) return false;
            for (auto *pane : panes) if (!pane->document() || pane->document()->loading()) return false;
            return true;
        }), "file/session opening timed out");
        result["documentsOpenedMs"] = startup.elapsed();
        for (auto *pane : panes) pane->setViewMode(scenario == "editing" ? SingleFileWindow::ViewMode::Source : scenario == "split" ? SingleFileWindow::ViewMode::Split : SingleFileWindow::ViewMode::Reading);
        require(waitFor([&] { for (auto *pane : panes) if (!previewReady(pane)) return false; return true; }, 60000), "initial native preview did not settle");
        if (workspace) workspace->grab();
        for (auto *widget : QApplication::topLevelWidgets())
            if (widget->isVisible()) require(widget->testAttribute(Qt::WA_DontShowOnScreen), "a probe window could appear on the desktop");
        result["initialQtPaintMs"] = startup.elapsed(); result["documentCount"] = panes.size();
        result["readyMemory"] = memory();
#ifdef Q_OS_WIN
        wchar_t module[MAX_PATH]{};
        require(GetModuleFileNameW(GetModuleHandleW(L"Qt6Core.dll"), module, MAX_PATH) > 0, "Qt runtime path unavailable");
        result["qtCorePath"] = QString::fromWCharArray(module);
#endif
        writeJson(readyFile, result);
        QJsonArray idle;
        QElapsedTimer clock; clock.start();
        while (clock.elapsed() < 2400) {
            app.processEvents(); QThread::msleep(200); idle.append(memory());
        }
        result["idleMemorySamples"] = idle;
        QJsonArray edits, modes;
        if (!panes.isEmpty()) {
            auto *pane = workspace ? workspace->activePane() : panes.last(); auto *document = pane->document(); auto *session = document->session();
            const auto before = session->snapshot().utf8; pane->setViewMode(SingleFileWindow::ViewMode::Split);
            require(waitFor([&] { return previewReady(pane); }), "split mode did not settle");
            for (int i = 0; i < 12; ++i) {
                const auto position = QString::fromUtf8(session->snapshot().utf8).size();
                clock.restart();
                require(session->replace(position, position, QStringLiteral("\n中文编辑校验 %1\n").arg(i), session->revision()) == EditResult::Applied, "edit rejected");
                require(waitFor([&] { return previewReady(pane); }), "edited preview did not settle");
                edits.append(clock.elapsed());
            }
            for (int i = 0; i < 24 && session->snapshot().utf8 != before; ++i) {
                require(session->undo() == EditResult::Applied, "undo rejected"); app.processEvents();
            }
            require(session->snapshot().utf8 == before, "undo did not restore exact source bytes");
            require(session->replace(0, 0, QStringLiteral("保存完整性验证\n"), session->revision()) == EditResult::Applied, "save fixture edit rejected");
            const auto saved = session->snapshot().utf8;
            require(document->save(), "save did not start");
            require(waitFor([&] { return !document->saving() && !document->dirty(); }), "save did not finish");
            QFile actual(document->path()); require(actual.open(QIODevice::ReadOnly) && actual.readAll() == saved, "saved bytes differ from session");
            for (auto mode : {SingleFileWindow::ViewMode::Source, SingleFileWindow::ViewMode::Reading, SingleFileWindow::ViewMode::Split}) {
                clock.restart(); pane->setViewMode(mode);
                require(waitFor([&] { return previewReady(pane); }), "mode did not settle"); modes.append(clock.elapsed());
            }
            result["editUndoSaveIntegrity"] = true;
        }
        result["editToSettledPreviewMs"] = edits; result["modeToSettledQtPaintMs"] = modes;
        result["finalMemory"] = memory(); result["ok"] = true;
        instance.stop();
    } catch (const std::exception &error) {
        result["error"] = QString::fromUtf8(error.what());
    }
    writeJson(resultFile, result);
    return result["ok"].toBool() ? 0 : 1;
}
