// apps/desktop/UpdateHandoff.cpp

#include "UpdateHandoff.h"
#include "BuildInfo.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QUuid>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#elif defined(Q_OS_LINUX)
#include <cerrno>
#include <csignal>
#endif

namespace xdnote {
namespace {
bool writeOwned(const QString &path, const QByteArray &bytes) {
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) &&
           file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) &&
           file.write(bytes) == bytes.size() && file.commit();
}
QJsonObject readControl(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4096)
        return {};
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
    return parse.error == QJsonParseError::NoError ? document.object() : QJsonObject{};
}
} // namespace
UpdateHandoff::UpdateHandoff(QObject *parent) : QObject(parent) {
    m_poll.setInterval(100);
    connect(&m_poll, &QTimer::timeout, this, &UpdateHandoff::poll);
}
void UpdateHandoff::prepare(const QString &directory, const QString &installRoot,
                            const QByteArray &manifest, const QByteArray &signature,
                            const QString &package) {
    m_directory = QFileInfo(directory).canonicalFilePath();
    m_transaction = QUuid::createUuid().toString(QUuid::Id128);
    const auto manifestFile = m_directory + "/release.json";
    const auto signatureFile = m_directory + "/release.json.sig";
    const auto requestFile = m_directory + "/request.json";
    if (m_directory.isEmpty() || !writeOwned(manifestFile, manifest) ||
        !writeOwned(signatureFile, signature)) {
        emit failed("handoff_files");
        return;
    }
    const QJsonObject request{{"schema_version", 1},
                              {"transaction", m_transaction},
                              {"parent_pid", double(QCoreApplication::applicationPid())},
                              {"current_version", BuildInfo::version()},
                              {"current_build", BuildInfo::buildNumber()},
                              {"install_root", installRoot},
                              {"package_file", package},
                              {"manifest_file", manifestFile},
                              {"signature_file", signatureFile}};
    if (!writeOwned(requestFile, QJsonDocument(request).toJson(QJsonDocument::Compact))) {
        emit failed("handoff_files");
        return;
    }
    QString program;
    QStringList arguments;
#ifdef Q_OS_WIN
    const auto source = QCoreApplication::applicationDirPath() + "/xdnote-update-helper.exe";
    program = m_directory + "/xdnote-update-helper.exe";
    if (!QFile::copy(source, program)) {
        emit failed("missing_helper");
        return;
    }
    arguments = {QStringLiteral("--request"), requestFile};
#elif defined(Q_OS_LINUX)
    program = QStringLiteral("/usr/libexec/xdnote-update-helper");
    arguments = {QStringLiteral("--authorize"), requestFile};
#else
    emit failed("unsupported_installation");
    return;
#endif
    qint64 pid = 0;
    if (!QProcess::startDetached(program, arguments, m_directory, &pid) || pid <= 0) {
        emit failed("helper_start");
        return;
    }
    m_processId = pid;
    m_elapsed.start();
    m_poll.start();
}
bool UpdateHandoff::writeControl(const QString &name) {
    return !m_directory.isEmpty() &&
           writeOwned(m_directory + '/' + name,
                      QJsonDocument(QJsonObject{{"transaction", m_transaction}})
                          .toJson(QJsonDocument::Compact));
}
bool UpdateHandoff::commit() {
    m_poll.stop();
    return m_ready && writeControl(QStringLiteral("proceed.json"));
}
void UpdateHandoff::cancel() {
    m_poll.stop();
    writeControl(QStringLiteral("cancel.json"));
}
void UpdateHandoff::poll() {
    const auto result = readControl(m_directory + "/result.json");
    if (result.value("transaction").toString() == m_transaction) {
        m_poll.stop();
        emit failed(result.value("error").toString("installation_failed"));
        return;
    }
    if (m_elapsed.hasExpired(300000)) {
        cancel();
        emit failed("authorization_timeout");
        return;
    }
    const auto prepared = readControl(m_directory + "/ready.json");
    if (prepared.value("transaction").toString() == m_transaction) {
        m_ready = true;
        m_poll.stop();
        emit ready();
        return;
    }
    // This is only an early-failure hint. Package trust and parent identity are
    // independently checked inside the helper, never inferred from this PID.
#ifdef Q_OS_WIN
    const auto process = OpenProcess(SYNCHRONIZE, FALSE, DWORD(m_processId));
    const bool exited = !process || WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
    if (process)
        CloseHandle(process);
#elif defined(Q_OS_LINUX)
    const bool exited = ::kill(pid_t(m_processId), 0) != 0 && errno == ESRCH;
#else
    const bool exited = true;
#endif
    if (exited) {
        m_poll.stop();
        emit failed("helper_exit");
    }
}

}
