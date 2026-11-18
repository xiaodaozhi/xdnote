// Private Qt API is restricted to this pinned-SDK diagnostic, never the product.
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QtNetwork/private/qabstractsocket_p.h>
#include <QtNetwork/private/qsocks5socketengine_p.h>
#include <cstdio>
// Exact layout from the pinned 6.8.2 translation unit; no changed members.
struct QSocks5Data { QTcpSocket *controlSocket; QSocks5Authenticator *authenticator; };

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv); QCommandLineParser parser;
    for (const auto &name : {"proxy", "port", "bytes", "sha256", "pause-ms"}) parser.addOption(QCommandLineOption(name, name, "value"));
    parser.addOption(QCommandLineOption("blocking")); parser.addOption(QCommandLineOption("remote-closes"));
    parser.addOption(QCommandLineOption("notify-close")); parser.process(app);
    const QUrl proxy(parser.value("proxy"), QUrl::StrictMode);
    bool number = false; const qint64 expected = parser.value("bytes").toLongLong(&number);
    const int port = parser.value("port").toInt(), pause = parser.value("pause-ms").toInt();
    if (proxy.scheme() != "socks5" || proxy.host() != "127.0.0.1" || proxy.port() < 1 || proxy.port() > 65535
        || !proxy.userInfo().isEmpty() || !proxy.path().isEmpty() || proxy.hasQuery() || proxy.hasFragment()
        || port < 1 || port > 65535 || !number || expected < 1 || expected > qint64(512)*1024*1024
        || parser.value("sha256").size() != 64 || pause < 0 || pause > 1000) return 4;
    QTcpSocket socket; socket.setProxy(QNetworkProxy(QNetworkProxy::Socks5Proxy, proxy.host(), quint16(proxy.port())));
    socket.setReadBufferSize(4096); QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 received = 0, peakEngine = 0, peakControl = 0, peakSocket = 0;
    bool paused = pause > 0, pauseStarted = false, finished = false, closeNotified = false, closeScheduled = false;
    const auto sample = [&] {
        peakSocket = qMax(peakSocket, socket.bytesAvailable());
        const auto *d = static_cast<QAbstractSocketPrivate *>(QObjectPrivate::get(&socket));
        auto *engine = d->socketEngine;
        if (!engine || QByteArray(engine->metaObject()->className()) != "QSocks5SocketEngine") return;
        peakEngine = qMax(peakEngine, engine->bytesAvailable());
        const auto *s = static_cast<QSocks5SocketEnginePrivate *>(QObjectPrivate::get(engine));
        if (s->data) {
            auto *control = s->data->controlSocket;
            peakControl = qMax(peakControl, control->bytesAvailable());
            // Replay the Windows close notifier in an owned diagnostic, even
            // on another host, while both bounded layers are under backpressure.
            if (parser.isSet("notify-close") && !closeScheduled && control->bytesAvailable() >= 65536) {
                const auto *inner = static_cast<QAbstractSocketPrivate *>(QObjectPrivate::get(control));
                if (inner->socketEngine) {
                    closeScheduled = true;
                    auto *nativeEngine = inner->socketEngine;
                    QTimer::singleShot(50, nativeEngine, [&, nativeEngine] {
                        closeNotified = QMetaObject::invokeMethod(nativeEngine, "closeNotification", Qt::QueuedConnection);
                    });
                }
            }
        }
    };
    const auto drain = [&] {
        sample();
        while (socket.bytesAvailable() > 0 && received <= expected) {
            const auto bytes = socket.read(qMin(qint64(65536), expected - received + 1));
            if (bytes.isEmpty()) break;
            hash.addData(bytes); received += bytes.size(); sample();
        }
    };
    const auto report = [&] {
        if (finished) return;
        finished = true; sample(); QObject::disconnect(&socket, nullptr, &app, nullptr);
        const auto actual = QString::fromLatin1(hash.result().toHex());
        const bool eof = socket.state() == QAbstractSocket::UnconnectedState;
        const bool ok = received == expected && actual == parser.value("sha256") && peakEngine <= 65536
            && peakControl <= 65536 && peakSocket <= 4096 && (!parser.isSet("remote-closes") || eof)
            && (!parser.isSet("notify-close") || closeNotified);
        std::puts(QJsonDocument(QJsonObject{{"ok", ok}, {"bytes", double(received)}, {"sha256", actual},
            {"peakEngineBytes", double(peakEngine)}, {"peakControlBytes", double(peakControl)}, {"peakSocketBytes", double(peakSocket)},
            {"eofObserved", eof}, {"closeNotificationReplayed", closeNotified}, {"blocking", parser.isSet("blocking")},
            {"scope", "pinned private socket bound/EOF diagnostic only"}}).toJson(QJsonDocument::Compact).constData());
        app.exit(ok ? 0 : 2);
    };
    if (parser.isSet("blocking")) {
        socket.connectToHost("updates.fixture.invalid", quint16(port));
        if (!socket.waitForConnected(10000)) { report(); return 2; }
        socket.write("PING"); socket.waitForBytesWritten(10000); QThread::msleep(pause);
        QElapsedTimer deadline; deadline.start();
        while (received < expected && deadline.elapsed() < 12000) {
            if (!socket.bytesAvailable() && !socket.waitForReadyRead(1000) && socket.state() == QAbstractSocket::UnconnectedState) break;
            drain();
        }
        if (parser.isSet("remote-closes") && socket.state() != QAbstractSocket::UnconnectedState) socket.waitForDisconnected(3000);
        report(); return received == expected && hash.result().toHex() == parser.value("sha256").toLatin1()
            && peakEngine <= 65536 && peakControl <= 65536 && peakSocket <= 4096
            && (!parser.isSet("remote-closes") || socket.state() == QAbstractSocket::UnconnectedState)
            && (!parser.isSet("notify-close") || closeNotified) ? 0 : 2;
    }
    QObject::connect(&socket, &QAbstractSocket::connected, &app, [&] { socket.write("PING"); });
    const auto maybeDone = [&] { if (socket.state() == QAbstractSocket::UnconnectedState
        || (received >= expected && !parser.isSet("remote-closes"))) report(); };
    QObject::connect(&socket, &QIODevice::readyRead, &app, [&] {
        sample();
        if (paused) {
            if (!pauseStarted) { pauseStarted = true; QTimer::singleShot(pause, &app, [&] { paused = false; drain(); maybeDone(); }); }
            return;
        }
        drain(); maybeDone();
    });
    QObject::connect(&socket, &QAbstractSocket::disconnected, &app, [&] { if (!paused) { drain(); report(); } });
    QTimer observation; observation.setInterval(1); QObject::connect(&observation, &QTimer::timeout, &app, sample); observation.start();
    QTimer::singleShot(15000, &app, report);
    socket.connectToHost("updates.fixture.invalid", quint16(port)); return app.exec();
}
