// Explicit public-network diagnostic. No document, preference or install I/O.
#include "UpdateController.h"
#include "BuildInfo.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QTimer>
#include <cstdio>
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    xdnote::UpdateController updates;
    QObject::connect(&updates, &xdnote::UpdateController::changed, &app, [&] {
        if (updates.busy()) return;
        const auto result = QJsonDocument(QJsonObject{{"scope", "explicit public update check; no installation"},
            {"version", xdnote::BuildInfo::version()}, {"state", updates.state()}, {"error", updates.errorCode()},
            {"trusted", updates.trusted()}, {"store", updates.packaged()}}).toJson(QJsonDocument::Compact);
        std::fwrite(result.constData(),1,std::size_t(result.size()),stdout);std::fputc('\n',stdout);std::fflush(stdout);
        app.exit(updates.state() == "error" ? 2 : 0);
    });
    QTimer::singleShot(0, &updates, &xdnote::UpdateController::check);
    QTimer::singleShot(360000, &app, [&] { updates.cancel(); app.exit(3); });
    return app.exec();
}
