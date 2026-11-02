#include "SingleInstance.h"
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QThread>
#include <QTimer>
#include <QtEndian>

namespace xdnote {
void SingleInstance::stop() {
    m_accepting = false;
    m_server.disconnect(this);
    for (auto *socket : m_server.findChildren<QLocalSocket *>()) { socket->disconnect(this); socket->abort(); }
    m_server.close(); m_lock.reset(); m_connections = 0;
}
SingleInstance::Result SingleInstance::start(const QString &scopeDirectory, const QStringList &paths, std::function<void(QStringList)> accepted) {
    m_error = QStringLiteral("无法与已运行的 xdnote 建立联系，请稍后重试");
    if (paths.size() > 16 || !QDir().mkpath(scopeDirectory)) return Result::Error;
    QJsonArray array;
    for (const auto &path : paths) {
        if (path.size() > 8192 || path.contains(QChar(0)) || !QDir::isAbsolutePath(path)) return Result::Error;
        array.append(path);
    }
    const auto payload = QJsonDocument(QJsonObject{{"v", 1}, {"paths", array}}).toJson(QJsonDocument::Compact);
    if (payload.size() > 65536) return Result::Error;
    const auto name = QStringLiteral("xdnote-v1-") + QString::fromLatin1(QCryptographicHash::hash(scopeDirectory.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
    m_lock = std::make_unique<QLockFile>(scopeDirectory + "/instance.lock"); m_lock->setStaleLockTime(0);
    if (!m_lock->tryLock(0)) {
        QElapsedTimer timer; timer.start();
        while (timer.elapsed() < 5000) {
            QLocalSocket socket; socket.connectToServer(name);
            if (!socket.waitForConnected(200)) { QThread::msleep(25); continue; }
            const quint32 size = qToBigEndian(quint32(payload.size()));
            socket.write(reinterpret_cast<const char *>(&size), 4); socket.write(payload); socket.flush();
            QByteArray reply;
            while (timer.elapsed() < 5000 && !reply.contains('\n')) {
                if (!socket.bytesAvailable() && !socket.waitForReadyRead(200)) { if (socket.state() == QLocalSocket::UnconnectedState) break; continue; }
                reply += socket.readAll(); if (reply.size() > 1024) return Result::Error;
            }
            if (QJsonDocument::fromJson(reply).object().value("accepted").toBool()) return Result::Forwarded;
            return Result::Error;
        }
        return Result::Error;
    }
    m_server.setSocketOptions(QLocalServer::UserAccessOption); m_server.setMaxPendingConnections(8);
    if (!m_server.listen(name)) { QLocalServer::removeServer(name); if (!m_server.listen(name)) return Result::Error; }
    m_accepting = true;
    connect(&m_server, &QLocalServer::newConnection, this, [this, accepted] {
        while (m_server.hasPendingConnections()) {
            auto *socket = m_server.nextPendingConnection();
            socket->setParent(&m_server);
            if (m_connections >= 8) { socket->disconnectFromServer(); socket->deleteLater(); continue; }
            ++m_connections; socket->setReadBufferSize(65540);
            auto data = std::make_shared<QByteArray>();
            auto handled = std::make_shared<bool>(false);
            connect(socket, &QLocalSocket::disconnected, this, [this, socket] { --m_connections; socket->deleteLater(); });
            QTimer::singleShot(3000, socket, [socket] { socket->disconnectFromServer(); });
            const auto receive = [this, socket, data, handled, accepted] {
                if (*handled || !m_accepting) return;
                *data += socket->readAll();
                if (data->size() < 4) return;
                const auto size = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(data->constData()));
                if (size > 65536 || data->size() > 65540) { socket->disconnectFromServer(); return; }
                if (data->size() < qsizetype(size + 4)) return;
                *handled = true;
                const auto request = QJsonDocument::fromJson(data->mid(4, size)).object();
                const auto paths = request.value("paths").toArray();
                bool valid = request.value("v").toInt() == 1 && request.value("paths").isArray() && paths.size() <= 16;
                QStringList values;
                for (const auto &value : paths) {
                    const auto path = value.toString();
                    valid &= value.isString() && path.size() <= 8192 && !path.contains(QChar(0)) && QDir::isAbsolutePath(path); values.append(path);
                }
                if (valid) accepted(values);
                socket->write(valid ? "{\"accepted\":true}\n" : "{\"accepted\":false}\n"); socket->flush(); socket->disconnectFromServer();
            };
            connect(socket, &QLocalSocket::readyRead, this, receive);
            if (socket->bytesAvailable()) QTimer::singleShot(0, socket, receive);
        }
    });
    m_error.clear(); return Result::Primary;
}
}
