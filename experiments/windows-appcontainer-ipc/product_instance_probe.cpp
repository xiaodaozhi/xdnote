#include "SingleInstance.h"
#include <QCoreApplication>
#include <QFileInfo>
#include <QMetaObject>
#include <QTimer>
#include <cstdio>
#include <thread>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 3) return 2;
    xdnote::SingleInstance instance;
    const QString expected = args[2] + QStringLiteral("/fixture.md");
    if (args[1] == "client")
        return instance.start(args[2], {expected}, [](QStringList){}) == xdnote::SingleInstance::Result::Forwarded ? 0 : 3;
    const bool race = args[1] == "race";
    if (args[1] != "server" && !race) return 2;
    if (race && std::getchar() != 'g') return 5;
    const auto status = instance.start(args[2], race ? QStringList{expected} : QStringList{}, [&](QStringList paths) {
        std::puts(paths == QStringList{expected} ? "accepted" : "wrong_payload");
        std::fflush(stdout);
    });
    if (race && status == xdnote::SingleInstance::Result::Forwarded) {
        std::puts("forwarded"); std::fflush(stdout); return 0;
    }
    if (status != xdnote::SingleInstance::Result::Primary) {
        if (QFileInfo::exists(args[2] + QStringLiteral("/instance.lock"))) {
            std::puts("failed_lock_retained"); std::fflush(stdout); return 6;
        }
        std::puts("primary_failed"); std::fflush(stdout); return 4;
    }
    std::puts("ready"); std::fflush(stdout);
    std::thread([&app] { std::getchar(); QMetaObject::invokeMethod(&app, &QCoreApplication::quit, Qt::QueuedConnection); }).detach();
    QTimer::singleShot(30000, &app, &QCoreApplication::quit);
    const int code = app.exec();
    instance.stop();
    return code;
}
