// Explicit public-network diagnostic. No document, preference or install I/O.
#include "UpdateController.h"
#include "BuildInfo.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QTimer>
#include <QSysInfo>
#include "UpdateNetwork.h"
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QSslConfiguration>
#include <QSslSocket>
#include <cstdio>
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
#ifdef Q_OS_WIN
    const QString platform = "windows";
#elif defined(Q_OS_MACOS)
    const QString platform = "macos";
#else
    const QString platform = "linux";
#endif
    xdnote::UpdateController updates(nullptr, xdnote::BuildInfo::version(), platform, QSysInfo::buildCpuArchitecture(), xdnote::BuildInfo::packaged());
    const auto args = app.arguments();
    if (args.size() != 1 && ((args.size() != 3 && args.size() != 4) || !updates.configureNetwork(args[1], args[2]))) return 4;
    if (args.size() == 4) {
        if (args[3] != "--public-page") return 4;
        QNetworkAccessManager network; network.setProxyFactory(new xdnote::UpdateNetwork::ProxyFactory(args[1], args[2]));
        QNetworkRequest request(QUrl("https://github.com/xiaodaozhi/xdnote/releases"));
        auto tls = QSslConfiguration::defaultConfiguration(); tls.setPeerVerifyMode(QSslSocket::VerifyPeer); tls.setProtocol(QSsl::TlsV1_2OrLater); request.setSslConfiguration(tls);
        request.setTransferTimeout(15000); request.setMaximumRedirectsAllowed(5); request.setRawHeader("User-Agent", "xdnote-network-diagnostic");
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual); request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual); request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
        auto *reply = network.get(request); reply->setReadBufferSize(65536); qint64 received = 0; bool tlsError = false;
        QObject::connect(reply, &QNetworkReply::redirected, reply, [reply](const QUrl &url) { if (xdnote::UpdateController::allowedUrl(reply->url().resolved(url))) reply->redirectAllowed(); else reply->abort(); });
        QObject::connect(reply, &QNetworkReply::sslErrors, reply, [&](const QList<QSslError> &) { tlsError = true; reply->abort(); });
        const auto drain = [&] { while (reply->bytesAvailable()) { received += reply->read(65536).size(); if (received > 8 * 1024 * 1024) { reply->abort(); break; } } };
        QObject::connect(reply, &QIODevice::readyRead, reply, drain);
        QObject::connect(reply, &QNetworkReply::finished, &app, [&] {
            drain(); const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const bool ok = status == 200 && reply->error() == QNetworkReply::NoError && !tlsError;
            const auto output = QJsonDocument(QJsonObject{{"scope", "fixed public release-page HTTPS only; no candidate/install acceptance"}, {"ok", ok}, {"status", status}, {"networkError", int(reply->error())}, {"tlsError", tlsError}, {"bytes", received}}).toJson(QJsonDocument::Compact);
            std::puts(output.constData()); app.exit(ok ? 0 : 2);
        });
        QTimer::singleShot(30000, reply, [reply] { reply->abort(); });
        return app.exec();
    }
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
