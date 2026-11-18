// Owned loopback transport diagnostic; no candidate authentication or installation.
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTimer>
#include <cstdio>
#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static QString networkLibrary() {
#ifdef Q_OS_WIN
    HMODULE module = nullptr; wchar_t buffer[32768];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&QNetworkAccessManager::staticMetaObject), &module)) return {};
    const auto count = GetModuleFileNameW(module, buffer, 32768);
    return count && count < 32768 ? QString::fromWCharArray(buffer, count) : QString();
#else
    Dl_info info{};
    return dladdr(&QNetworkAccessManager::staticMetaObject, &info) && info.dli_fname ? QString::fromUtf8(info.dli_fname) : QString();
#endif
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv); QCommandLineParser parser;
    for (const auto &name : {"url", "proxy", "ca", "bytes", "sha256", "pause-ms", "timeout-ms"})
        parser.addOption(QCommandLineOption(name, name, "value"));
    parser.process(app);
    const QUrl url(parser.value("url"), QUrl::StrictMode), proxy(parser.value("proxy"), QUrl::StrictMode);
    bool number = false; const qint64 expected = parser.value("bytes").toLongLong(&number);
    const int pause = parser.value("pause-ms").toInt();
    const int timeout = parser.isSet("timeout-ms") ? parser.value("timeout-ms").toInt() : 12000;
    if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https") || url.host() != "updates.fixture.invalid"
        || url.port() < 1 || url.path() != "/archive.app.zip" || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()
        || proxy.host() != "127.0.0.1" || proxy.port() < 1 || !proxy.userInfo().isEmpty() || !proxy.path().isEmpty()
        || proxy.hasQuery() || proxy.hasFragment() || (proxy.scheme() != "http" && proxy.scheme() != "socks5")
        || !number || expected < 1 || expected > qint64(512) * 1024 * 1024 || parser.value("sha256").size() != 64
        || pause < 0 || pause > 1000 || timeout < 100 || timeout > 30000) return 4;
    QNetworkAccessManager manager;
    manager.setProxy(QNetworkProxy(proxy.scheme() == "http" ? QNetworkProxy::HttpProxy : QNetworkProxy::Socks5Proxy,
                                  proxy.host(), quint16(proxy.port())));
    QNetworkRequest request(url); request.setTransferTimeout(timeout);
    request.setRawHeader("Accept-Encoding", "identity");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    if (url.scheme() == "https") {
        auto tls = QSslConfiguration::defaultConfiguration();
        const auto certificates = QSslCertificate::fromPath(parser.value("ca"));
        if (certificates.isEmpty()) return 4;
        tls.setCaCertificates(certificates); tls.setPeerVerifyMode(QSslSocket::VerifyPeer); tls.setProtocol(QSsl::TlsV1_2OrLater);
        request.setSslConfiguration(tls);
    }
    auto *reply = manager.get(request); reply->setReadBufferSize(65536);
    QElapsedTimer elapsed; elapsed.start(); QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 received = 0, peak = 0; bool tlsError = false, paused = pause > 0, pauseStarted = false, overflow = false;
    const auto drain = [&] {
        peak = qMax(peak, reply->bytesAvailable());
        while (reply->bytesAvailable() > 0) {
            const auto bytes = reply->read(qMin(qint64(65536), expected - received + 1));
            if (bytes.isEmpty()) break;
            received += bytes.size(); hash.addData(bytes);
            if (received > expected) { overflow = true; reply->abort(); break; }
        }
    };
    QObject::connect(reply, &QNetworkReply::sslErrors, &app, [&] { tlsError = true; });
    QObject::connect(reply, &QIODevice::readyRead, &app, [&] {
        peak = qMax(peak, reply->bytesAvailable());
        if (paused) {
            if (!pauseStarted) { pauseStarted = true; QTimer::singleShot(pause, &app, [&] { paused = false; drain(); }); }
            return;
        }
        drain();
    });
    QObject::connect(reply, &QNetworkReply::finished, &app, [&] {
        drain(); const auto actual = QString::fromLatin1(hash.result().toHex());
        const bool ok = reply->error() == QNetworkReply::NoError && !tlsError && !overflow
            && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200
            && received == expected && actual == parser.value("sha256");
        const QJsonObject result{{"ok", ok}, {"bytes", double(received)}, {"expectedBytes", double(expected)},
            {"sha256", actual}, {"networkError", int(reply->error())}, {"tlsError", tlsError}, {"overflow", overflow},
            {"peakReplyBytes", double(peak)}, {"readBufferBytes", double(reply->readBufferSize())},
            {"elapsedMs", double(elapsed.elapsed())}, {"networkLibrary", networkLibrary()},
            {"qtVersion", QString::fromLatin1(qVersion())}, {"tlsBackend", QSslSocket::activeBackend()},
            {"scope", "owned loopback bytes/TLS only; no manifest authentication, install or minimum-OS acceptance"}};
        std::puts(QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
        QTimer::singleShot(0, &app, [&app, ok] { app.exit(ok ? 0 : 2); });
    });
    QTimer::singleShot(timeout + 3000, reply, [reply] { reply->abort(); });
    return app.exec();
}
