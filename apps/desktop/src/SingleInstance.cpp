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
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <vector>
#ifdef XDNOTE_IPC_TEST_HOOKS
#include <cstdio>
#endif
#endif

namespace xdnote {
#ifdef Q_OS_WIN
namespace {
class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle = nullptr) : m_handle(handle) {}
    ~ScopedHandle() { if (m_handle && m_handle != INVALID_HANDLE_VALUE) CloseHandle(m_handle); }
    ScopedHandle(const ScopedHandle &) = delete;
    ScopedHandle &operator=(const ScopedHandle &) = delete;
    HANDLE get() const { return m_handle; }
private:
    HANDLE m_handle;
};

bool tokenBytes(HANDLE token, TOKEN_INFORMATION_CLASS kind, std::vector<unsigned char> &bytes) {
    DWORD length = 0;
    if (GetTokenInformation(token, kind, nullptr, 0, &length) || GetLastError() != ERROR_INSUFFICIENT_BUFFER || !length)
        return false;
    bytes.resize(length);
    return GetTokenInformation(token, kind, bytes.data(), length, &length);
}

bool isAppContainer(bool &result) {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return false;
    ScopedHandle token(raw);
    DWORD value = 0, size = 0;
    if (!GetTokenInformation(token.get(), TokenIsAppContainer, &value, sizeof(value), &size) || size != sizeof(value)) return false;
    result = value != 0;
    return true;
}

HANDLE createContainerBootstrap(const QString &name) {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return INVALID_HANDLE_VALUE;
    ScopedHandle token(raw);
    std::vector<unsigned char> userBytes, groupBytes, packageBytes;
    if (!tokenBytes(token.get(), TokenUser, userBytes) || !tokenBytes(token.get(), TokenPrimaryGroup, groupBytes) ||
        !tokenBytes(token.get(), TokenAppContainerSid, packageBytes)) return INVALID_HANDLE_VALUE;
    const auto user = reinterpret_cast<const TOKEN_USER *>(userBytes.data())->User.Sid;
    const auto group = reinterpret_cast<const TOKEN_PRIMARY_GROUP *>(groupBytes.data())->PrimaryGroup;
    const auto package = reinterpret_cast<const TOKEN_APPCONTAINER_INFORMATION *>(packageBytes.data())->TokenAppContainer;
    if (!user || !group || !package || !IsValidSid(user) || !IsValidSid(group) || !IsValidSid(package)) return INVALID_HANDLE_VALUE;
    const DWORD aclSize = sizeof(ACL) + 2 * (sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD)) + GetLengthSid(user) + GetLengthSid(package);
    std::vector<unsigned char> aclBytes(aclSize);
    auto acl = reinterpret_cast<PACL>(aclBytes.data());
    SECURITY_DESCRIPTOR descriptor;
    if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
        !InitializeAcl(acl, aclSize, ACL_REVISION) ||
        !AddAccessAllowedAce(acl, ACL_REVISION, FILE_ALL_ACCESS, user) ||
        !AddAccessAllowedAce(acl, ACL_REVISION, FILE_ALL_ACCESS, package) ||
        !SetSecurityDescriptorOwner(&descriptor, user, FALSE) ||
        !SetSecurityDescriptorGroup(&descriptor, group, FALSE) ||
        !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE)) return INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), &descriptor, FALSE};
    const auto pipe = QStringLiteral("\\\\.\\pipe\\") + name;
    return CreateNamedPipeW(reinterpret_cast<LPCWSTR>(pipe.utf16()), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES, 0, 0, 3000, &attributes);
}
}
#endif

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
    auto name = QStringLiteral("xdnote-v1-") + QString::fromLatin1(QCryptographicHash::hash(scopeDirectory.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
#ifdef Q_OS_WIN
    bool appContainer = false;
    if (!isAppContainer(appContainer)) return Result::Error;
    if (appContainer) name.prepend(QStringLiteral("LOCAL\\"));
#endif
    m_lock = std::make_unique<QLockFile>(scopeDirectory + "/instance.lock"); m_lock->setStaleLockTime(0);
    if (!m_lock->tryLock(0)) {
        QElapsedTimer timer; timer.start();
        while (timer.elapsed() < 5000) {
            QLocalSocket socket; socket.connectToServer(name);
            if (!socket.waitForConnected(200)) { QThread::msleep(25); continue; }
#if defined(Q_OS_WIN) && defined(XDNOTE_IPC_TEST_HOOKS)
            if (appContainer && qEnvironmentVariable("XDNOTE_IPC_TEST_BOOTSTRAP_GATE") == QStringLiteral("gate")) {
                std::puts("client_connected"); std::fflush(stdout);
            }
#endif
            const quint32 size = qToBigEndian(quint32(payload.size()));
            socket.write(reinterpret_cast<const char *>(&size), 4); socket.write(payload); socket.flush();
            QByteArray reply;
            while (timer.elapsed() < 5000 && !reply.contains('\n')) {
                if (!socket.bytesAvailable() && !socket.waitForReadyRead(200)) { if (socket.state() == QLocalSocket::UnconnectedState) break; continue; }
                reply += socket.readAll(); if (reply.size() > 1024) return Result::Error;
            }
            if (QJsonDocument::fromJson(reply).object().value("accepted").toBool()) return Result::Forwarded;
#ifdef Q_OS_WIN
            // A client can briefly reach the bootstrap instance before Qt has
            // listeners. Retry a closed connection only if it sent no reply.
            // Open intents are deduplicated by the session's file identity.
            if (appContainer && reply.isEmpty() && socket.state() == QLocalSocket::UnconnectedState) {
                QThread::msleep(25);
                continue;
            }
#endif
            return Result::Error;
        }
        return Result::Error;
    }
    m_server.setSocketOptions(QLocalServer::UserAccessOption); m_server.setMaxPendingConnections(8);
#ifdef Q_OS_WIN
    if (appContainer) {
        // The first instance fixes the DACL for every Qt listener. Refuse an
        // existing pipe rather than adopting an endpoint owned by another process.
        ScopedHandle bootstrap(createContainerBootstrap(name));
#ifdef XDNOTE_IPC_TEST_HOOKS
        if (bootstrap.get() != INVALID_HANDLE_VALUE && qEnvironmentVariable("XDNOTE_IPC_TEST_BOOTSTRAP_GATE") == QStringLiteral("gate")) {
            std::puts("bootstrap_ready"); std::fflush(stdout);
            if (std::getchar() != 'g') { m_lock.reset(); return Result::Error; }
        }
#endif
        if (bootstrap.get() == INVALID_HANDLE_VALUE || !m_server.listen(name)) {
            m_lock.reset();
            return Result::Error;
        }
    } else
#endif
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
