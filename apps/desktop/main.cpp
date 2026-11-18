#include "FileApplication.h"
#include "GuiRuntime.h"
#include "SourceEditor.h"
#include "SingleInstance.h"
#include "VisualTokens.h"
#include "InstallationGuard.h"
#include "BuildInfo.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QStandardPaths>
#include <QMessageBox>
#include <QIcon>
#include <cstdio>

int main(int argc, char **argv) {
    // Helpers use a bounded, non-GUI identity query even while maintenance owns
    // the installation gate. This mode never opens a document or IPC endpoint.
    if (argc == 2 && QByteArray(argv[1]) == "--version-json") {
        const auto bytes = QJsonDocument(QJsonObject{{"product", "xdnote"}, {"version", xdnote::BuildInfo::version()},
            {"build", xdnote::BuildInfo::buildNumber()}, {"channel", xdnote::BuildInfo::channel()},
            {"revision", xdnote::BuildInfo::revision()}, {"dirty", xdnote::BuildInfo::dirty()}}).toJson(QJsonDocument::Compact);
        std::fwrite(bytes.constData(), 1, std::size_t(bytes.size()), stdout); return 0;
    }
    xdnote::InstallationGuard installation;
    xdnote::initializeGuiRuntime();
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("xdnote")); application.setOrganizationName(QStringLiteral("xiaodaozhi"));
    xdnote::configureGuiApplication(); xdnote::SourceEditor::installAccessibility();
    xdnote::visualTokens();
#ifdef Q_OS_MACOS
    application.setWindowIcon(QIcon(QStringLiteral(":/xdnote/design/assets/ui-v2/brand/png/app-icon-macos-1024.png")));
#else
    application.setWindowIcon(QIcon(QStringLiteral(":/xdnote/design/assets/ui-v2/brand/app-icon.svg")));
#endif
    application.setQuitOnLastWindowClosed(false);
    if (!installation.allowed()) {
        QMessageBox::critical(nullptr, QStringLiteral("xdnote"), QStringLiteral("安装维护正在进行，或无法建立运行保护。请完成安装后重试。"));
        return 1;
    }
    QCommandLineParser parser; parser.setApplicationDescription(QStringLiteral("xdnote Markdown 阅读与编辑")); parser.addHelpOption();
    parser.addPositionalArgument("files", QStringLiteral("要打开的本地 Markdown 文件"), "[files…]"); parser.process(application);
    QStringList paths;
    for (const auto &path : parser.positionalArguments()) paths.append(QDir::isAbsolutePath(path) ? path : QDir::currentPath() + '/' + path);
    xdnote::SingleInstance instance;
    xdnote::FileApplication *windows = nullptr;
    const auto result = instance.start(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation), paths, [&windows](const QStringList &files) {
        if (!windows) return;
        if (files.isEmpty()) windows->activate(); else for (const auto &path : files) windows->openExternal(path);
    });
    if (result == xdnote::SingleInstance::Result::Forwarded) return 0;
    if (result == xdnote::SingleInstance::Result::Error) { std::fprintf(stderr, "%s\n", instance.error().toUtf8().constData()); QMessageBox::critical(nullptr, QStringLiteral("xdnote"), instance.error()); return 1; }
    xdnote::FileApplication desktop(&application, xdnote::BuildInfo::settingsScope(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)) + "/settings.json"); windows = &desktop;
    if (paths.isEmpty()) desktop.workspaceWindow(); else for (const auto &path : paths) desktop.openExternal(path);
    const int code = application.exec(); windows = nullptr; instance.stop(); return code;
}
