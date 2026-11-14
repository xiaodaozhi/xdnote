#include "FileApplication.h"
#include "SingleFileWindow.h"
#include "WorkspaceWindow.h"
#include "MarkdownPreview.h"
#include "GuiRuntime.h"
#include "SourceEditor.h"
#include "probe_fonts.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <functional>
#include <stdexcept>
#ifdef Q_OS_UNIX
#include <dlfcn.h>
#endif
using namespace xdnote;
namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
bool waitFor(const std::function<bool()> &condition) {
    QElapsedTimer timer; timer.start();
    do { qApp->processEvents(); if (condition()) return true; QThread::msleep(2); } while (timer.elapsed() < 30000);
    return false;
}
}
int main(int argc, char **argv) {
    initializeGuiRuntime(); QApplication app(argc, argv); configureGuiApplication();
    SourceEditor::installAccessibility(); app.setQuitOnLastWindowClosed(false);
    if (!installProbeFont()) return 2;
    const auto args = app.arguments(); if (args.size() != 4) return 2;
    QJsonObject result{{"ok", false}, {"theme", args[1]}, {"scope", "external QtTest-free probe; actual application services; offscreen; no personal data or desktop input"}};
    try {
        require(QGuiApplication::platformName() == "offscreen", "offscreen diagnostic backend required");
        require(args[1] == "light" || args[1] == "dark", "explicit diagnostic theme required");
        QTemporaryDir fixture; require(fixture.isValid(), "fixture directory unavailable");
        QImage image(160, 96, QImage::Format_RGB32); image.fill(Qt::blue);
        require(image.save(fixture.filePath("sample.png")), "image fixture failed");
        const auto path = fixture.filePath("sample.md");
        const QByteArray original = QStringLiteral("# 写作验证\n\n![自建图片](sample.png)\n\n> 少一点干扰，多一点专注。\n\n- [x] 中文与 Latin\n\n```cpp\nint main() {}\n```\n").toUtf8();
        { QFile file(path); require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(original) == original.size(), "document fixture failed"); }
        FileApplication application(nullptr, fixture.filePath("settings.json")); application.open(path);
        QPointer<SingleFileWindow> pane;
        require(waitFor([&] {
            for (auto *widget : QApplication::topLevelWidgets())
                if (auto *candidate = qobject_cast<SingleFileWindow *>(widget))
                    if (candidate->document() && candidate->document()->path() == path) pane = candidate;
            return pane && !pane->document()->loading();
        }), "standalone file did not open");
        require(pane->appearance()->setPreferences(args[1], QStringLiteral("graphite")), "diagnostic theme failed");
        auto ready = [&] { pane->grab(); return pane->preview()->view()->content() && pane->preview()->view()->ready() && pane->preview()->mappingCurrent(); };
        require(waitFor([&] { return ready() && pane->preview()->view()->admittedImageCount() == 1; }), "native preview or installed resource helper failed");
        require(pane->grab().save(args[2]), "standalone screenshot failed");
        auto *document = pane->document(); auto *session = document->session();
        require(session->snapshot().utf8 == original, "opened bytes differ");
        require(session->replace(0, 0, QStringLiteral("编辑\n"), session->revision()) == EditResult::Applied, "edit rejected");
        require(session->undo() == EditResult::Applied && session->snapshot().utf8 == original, "undo changed original bytes");
        require(session->replace(0, 0, QStringLiteral("保存\n"), session->revision()) == EditResult::Applied, "save edit rejected");
        const auto expected = session->snapshot().utf8;
        require(document->save() && waitFor([&] { return !document->saving() && !document->dirty(); }), "save failed");
        { QFile file(path); require(file.open(QIODevice::ReadOnly) && file.readAll() == expected, "saved bytes differ"); }
        auto *workspace = application.workspaceWindow(fixture.path());
        require(application.transferToWorkspace(pane, workspace), "workspace transfer failed");
        require(waitFor([&] { return workspace->activePane() == pane && pane->document() == document; }), "unique session not retained");
        pane->setViewMode(SingleFileWindow::ViewMode::Split);
        require(waitFor(ready) && workspace->grab().save(args[3]), "workspace split preview failed");
        result["editUndoSaveIntegrity"] = true; result["uniqueSessionTransfer"] = true;
        result["nativeImages"] = pane->preview()->view()->admittedImageCount();
        result["resourceHelper"] = QFileInfo(QCoreApplication::applicationDirPath() + "/xdnote-resource-helper").canonicalFilePath();
#ifdef Q_OS_UNIX
        Dl_info info{};
        require(dladdr(reinterpret_cast<void *>(&qVersion), &info) != 0, "actual Qt Core location unavailable");
        result["qtCorePath"] = QFileInfo(QString::fromUtf8(info.dli_fname)).canonicalFilePath();
#endif
        result["ok"] = true;
    } catch (const std::exception &error) { result["error"] = QString::fromUtf8(error.what()); }
    std::puts(QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
    return result["ok"].toBool() ? 0 : 1;
}
