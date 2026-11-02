#include "FileApplication.h"
#include "GuiRuntime.h"
#include "SourceEditor.h"
#include "SingleInstance.h"
#include "VisualTokens.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QStandardPaths>
#include <QMessageBox>
#include <QIcon>
#include <cstdio>

int main(int argc, char **argv) {
    xdnote::initializeGuiRuntime();
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("xdnote")); application.setOrganizationName(QStringLiteral("xiaodaozhi"));
    xdnote::configureGuiApplication(); xdnote::SourceEditor::installAccessibility();
    xdnote::visualTokens(); application.setWindowIcon(QIcon(QStringLiteral(":/xdnote/design/assets/ui-v2/brand/app-icon.svg")));
    application.setQuitOnLastWindowClosed(false);
    QCommandLineParser parser; parser.setApplicationDescription(QStringLiteral("xdnote Markdown 阅读与编辑")); parser.addHelpOption();
    parser.addPositionalArgument("files", QStringLiteral("要打开的本地 Markdown 文件"), "[files…]"); parser.process(application);
    QStringList paths;
    for (const auto &path : parser.positionalArguments()) paths.append(QDir::isAbsolutePath(path) ? path : QDir::currentPath() + '/' + path);
    xdnote::SingleInstance instance;
    xdnote::FileApplication *windows = nullptr;
    const auto result = instance.start(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation), paths, [&windows](const QStringList &files) {
        if (!windows) return;
        if (files.isEmpty()) windows->activate(); else for (const auto &path : files) windows->open(path);
    });
    if (result == xdnote::SingleInstance::Result::Forwarded) return 0;
    if (result == xdnote::SingleInstance::Result::Error) { std::fprintf(stderr, "%s\n", instance.error().toUtf8().constData()); QMessageBox::critical(nullptr, QStringLiteral("xdnote"), instance.error()); return 1; }
    xdnote::FileApplication desktop(&application); windows = &desktop;
    if (paths.isEmpty()) desktop.emptyWindow(); else for (const auto &path : paths) desktop.open(path);
    const int code = application.exec(); windows = nullptr; instance.stop(); return code;
}
