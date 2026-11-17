#include "UpdateDownload.h"
#include "UpdateController.h"
#include "UpdateNetwork.h"
#include <QDir>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QStandardPaths>
#include <QTimer>
#include <algorithm>
namespace xdnote {
UpdateDownload::UpdateDownload(QNetworkAccessManager *network, QObject *parent) : QObject(parent), m_network(network) {}
UpdateDownload::~UpdateDownload() { cancel(); }
void UpdateDownload::retainForHelper() { if (m_directory && !active() && !m_path.isEmpty()) m_directory->setAutoRemove(false); }
void UpdateDownload::cancel() {
    ++m_generation;
    if (m_reply) { auto reply = m_reply; m_reply = nullptr; reply->abort(); reply->deleteLater(); }
    if (m_file) m_file->cancelWriting();
    m_file.reset(); m_directory.reset(); m_path.clear();
}
void UpdateDownload::fail(const QString &error) { m_error = error; if (m_reply) m_reply->abort(); }
void UpdateDownload::start(const QJsonObject &candidate, const QString &mirror, const QString &cache) {
    if (active()) return;
    cancel(); m_error.clear(); m_hash.reset(); m_received = 0;
    const auto url = QUrl(candidate.value("asset_url").toString(), QUrl::StrictMode);
    const auto name = candidate.value("filename").toString();
    const auto length = candidate.value("bytes").toDouble();
    m_expectedHash = candidate.value("sha256").toString();
    const auto validHash = m_expectedHash.size() == 64 && std::all_of(m_expectedHash.cbegin(), m_expectedHash.cend(), [](QChar c) { return (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f'); });
    if (!UpdateController::allowedUrl(url) || !UpdateNetwork::validMirror(mirror) || name.isEmpty() || name == "." || name == ".." ||
        name.contains('/') || name.contains('\\') || name.contains(':') || name.contains(QChar(0)) ||
        length < 1 || length > qint64(2) * 1024 * 1024 * 1024 || length != qint64(length) || !validHash) { emit finished("invalid_package"); return; }
    m_expected = qint64(length);
    const auto base = cache.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/updates" : cache;
    const QFileInfo info(base);
    if (info.isSymLink() || !QDir().mkpath(base) || !QFileInfo(base).isDir()) { emit finished("cache_io"); return; }
    m_directory = std::make_unique<QTemporaryDir>(QFileInfo(base).canonicalFilePath() + "/job-XXXXXX");
    if (!m_directory->isValid() || !QFile::setPermissions(m_directory->path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) { cancel(); emit finished("cache_io"); return; }
    m_path = m_directory->path() + '/' + name;
    m_file = std::make_unique<QSaveFile>(m_path);
    m_file->setDirectWriteFallback(false);
    if (!m_file->open(QIODevice::WriteOnly) || !m_file->setPermissions(QFile::ReadOwner | QFile::WriteOwner)) { cancel(); emit finished("cache_io"); return; }
    QNetworkRequest request(UpdateNetwork::mapped(url, mirror));
    auto tls = QSslConfiguration::defaultConfiguration(); tls.setPeerVerifyMode(QSslSocket::VerifyPeer); tls.setProtocol(QSsl::TlsV1_2OrLater); request.setSslConfiguration(tls);
    request.setRawHeader("User-Agent", "xdnote-update"); request.setRawHeader("Accept-Encoding", "identity");
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
    request.setMaximumRedirectsAllowed(5); request.setTransferTimeout(30000);
    auto *reply = m_network->get(request); m_reply = reply; reply->setReadBufferSize(64 * 1024);
    const auto generation = m_generation;
    connect(reply, &QNetworkReply::redirected, this, [this, reply, mirror](const QUrl &target) {
        const auto resolved = reply->url().resolved(target);
        if (UpdateController::allowedUrl(resolved) || UpdateNetwork::isMirrorUrl(resolved, mirror)) reply->redirectAllowed(); else fail("invalid_redirect");
    });
    connect(reply, &QNetworkReply::sslErrors, this, [this](const QList<QSslError> &) { fail("tls"); });
    connect(reply, &QIODevice::readyRead, this, &UpdateDownload::drain);
    QTimer::singleShot(3600000, reply, [this, generation] { if (generation == m_generation && active()) fail("timeout"); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
        reply->deleteLater(); if (generation != m_generation) return;
        drain(); // Account for data delivered only with finished().
        if (generation != m_generation) return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (m_error.isEmpty() && (status != 200 || reply->error() != QNetworkReply::NoError))
            m_error = status == 429 ? "rate_limited" : reply->error() == QNetworkReply::TimeoutError ? "timeout" : reply->error() == QNetworkReply::SslHandshakeFailedError ? "tls" : "network";
        if (m_error.isEmpty() && m_received != m_expected) m_error = "package_length_mismatch";
        if (m_error.isEmpty() && m_hash.result().toHex() != m_expectedHash.toLatin1()) m_error = "package_digest_mismatch";
        m_reply = nullptr;
        if (m_error.isEmpty() && !m_file->commit()) m_error = "cache_io";
        const auto error = m_error;
        if (!error.isEmpty()) cancel(); else m_file.reset();
        emit finished(error);
    });
}
void UpdateDownload::drain() {
    if (!m_reply || !m_error.isEmpty() || !m_file) return;
    while (m_reply->bytesAvailable() > 0) {
        const auto bytes = m_reply->read(qMin(qint64(64 * 1024), m_expected - m_received + 1));
        if (bytes.isEmpty()) break;
        m_received += bytes.size();
        if (m_received > m_expected) { fail("package_length_mismatch"); return; }
        if (m_file->write(bytes) != bytes.size()) { fail("cache_io"); return; }
        m_hash.addData(bytes);
    }
    emit progress(m_received, m_expected);
}
}
