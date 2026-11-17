#include "UpdateDownload.h"
#include "UpdateReceipt.h"
#include "UpdateHandoff.h"
#include "FileApplication.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QUuid>
#ifdef Q_OS_MACOS
#include "MacUpdateProvider.h"
#endif
#include "UpdateController.h"
#include "UpdateNetwork.h"
#include "UpdateBridge.h"
#include "BuildInfo.h"
#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSysInfo>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTimer>
#include <algorithm>
#include <memory>

namespace xdnote {
namespace {
QString currentPlatform() {
#ifdef Q_OS_WIN
    return "windows";
#elif defined(Q_OS_MACOS)
    return "macos";
#elif defined(Q_OS_LINUX)
    return "linux";
#else
    return {};
#endif
}
QString assetUrl(const QJsonObject &release, const QString &name) {
    QString result;
    int matches = 0;
    for (const auto &value : release.value("assets").toArray()) {
        const auto asset = value.toObject();
        if (asset.value("name").toString() != name) continue;
        if (++matches > 1) return {}; // Ambiguous assets fail closed.
        result = asset.value("browser_download_url").toString();
    }
    return result;
}
bool rateLimited(QNetworkReply *reply, int status, const QByteArray &bytes) {
    if (status == 429) return true;
    if (status != 403) return false;
    if (reply->rawHeader("X-RateLimit-Remaining").trimmed() == "0") return true;
    const auto retry = reply->rawHeader("Retry-After").trimmed();
    bool numeric = false;
    const auto delay = retry.toLongLong(&numeric);
    if (!retry.isEmpty() && numeric && delay >= 0 && std::all_of(retry.cbegin(), retry.cend(), [](char c) { return c >= '0' && c <= '9'; })) return true;
    const auto message = QJsonDocument::fromJson(bytes).object().value("message").toString().trimmed().toLower();
    return message.startsWith("api rate limit exceeded") || message.startsWith("you have exceeded a secondary rate limit");
}
}
UpdateController::UpdateController(QObject *parent)
    : UpdateController(nullptr, BuildInfo::version(), currentPlatform(), QSysInfo::buildCpuArchitecture(), BuildInfo::packaged(), parent) {
    m_build = BuildInfo::buildNumber(); m_channel = BuildInfo::channel();
    m_attemptPath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/updates/attempt.json";
    recoverAttempt();
}
UpdateController::UpdateController(QNetworkAccessManager *transport, QString version, QString platform,
                                   QString architecture, bool packaged, QObject *parent)
    : QObject(parent), m_network(transport ? transport : new QNetworkAccessManager(this)),
      m_version(std::move(version)), m_platform(std::move(platform)), m_architecture(std::move(architecture)), m_packaged(packaged) {
    m_download = new UpdateDownload(m_network, this);
    connect(m_download, &UpdateDownload::finished, this, [this](const QString &error) {
        if (!error.isEmpty()) { m_installRequested = false; setState("error", error); return; }
        setState("downloaded");
        if (m_installRequested) prepareInstallation();
    });
}
UpdateController::~UpdateController() {
    ++m_generation;
    if (m_reply) m_reply->abort();
    if (m_handoff && m_state != "installing") m_handoff->cancel();
    if (m_download) m_download->cancel();
}
void UpdateController::setState(const QString &state, const QString &error) { m_state = state; m_error = error; emit changed(); }
bool UpdateController::configureNetwork(const QString &mode, const QString &proxy, const QString &mirror) {
    if ((mode != "system" && mode != "none" && mode != "custom") ||
        (!proxy.isEmpty() && !UpdateNetwork::validProxy(proxy)) ||
        (mode == "custom" && !UpdateNetwork::validProxy(proxy)) || !UpdateNetwork::validMirror(mirror)) return false;
    // Staged configuration only; an active operation retains its own session.
    m_mode = mode; m_proxy = proxy; m_mirror = mirror;
    return true;
}
bool UpdateController::allowedUrl(const QUrl &url) {
    if (!url.isValid() || url.scheme() != "https" || !url.userInfo().isEmpty() || url.hasFragment() ||
        (url.port(-1) != -1 && url.port() != 443)) return false;
    const auto host = url.host().toLower();
    return host == "api.github.com" || host == "github.com" || host == "release-assets.githubusercontent.com" ||
           host == "objects.githubusercontent.com" || host == "github-releases.githubusercontent.com";
}
void UpdateController::fetch(const QUrl &url, qsizetype limit, Callback callback) {
    if (!allowedUrl(url)) { callback({}, "invalid_url"); return; }
    const auto mapped = UpdateNetwork::mapped(url, m_activeMirror);
    if (!m_activeMirror.isEmpty() && !UpdateNetwork::isMirrorUrl(mapped, m_activeMirror)) { callback({}, "invalid_url"); return; }
    QNetworkRequest request(mapped);
    auto tls = QSslConfiguration::defaultConfiguration();
    tls.setPeerVerifyMode(QSslSocket::VerifyPeer);
    tls.setProtocol(QSsl::TlsV1_2OrLater);
    request.setSslConfiguration(tls);
    request.setRawHeader("User-Agent", "xdnote/" + m_version.toUtf8());
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setTransferTimeout(15000);
    request.setMaximumRedirectsAllowed(5);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
    const auto cacheKey = mapped.toString(QUrl::FullyEncoded);
    const auto cached = m_cache.value(cacheKey);
    if (!cached.etag.isEmpty()) request.setRawHeader("If-None-Match", cached.etag);
    auto *reply = m_network->get(request); m_reply = reply;
    reply->setReadBufferSize(64 * 1024);
    const auto generation = m_generation;
    auto bytes = std::make_shared<QByteArray>();
    auto failure = std::make_shared<QString>();
    QTimer::singleShot(30000, reply, [reply, failure] { if (!reply->isFinished()) { *failure = "timeout"; reply->abort(); } });
    const auto mirror = m_activeMirror;
    connect(reply, &QNetworkReply::redirected, reply, [reply, failure, mirror](const QUrl &target) {
        const auto resolved = reply->url().resolved(target);
        if (allowedUrl(resolved) || UpdateNetwork::isMirrorUrl(resolved, mirror)) reply->redirectAllowed();
        else { *failure = "invalid_redirect"; reply->abort(); }
    });
    connect(reply, &QNetworkReply::sslErrors, reply, [reply, failure](const QList<QSslError> &) { *failure = "tls"; reply->abort(); });
    const auto drain = [reply, bytes, failure, limit] {
        if (!failure->isEmpty()) return;
        while (reply->bytesAvailable() > 0) {
            const auto chunk = reply->read(qMin(qsizetype(64 * 1024), limit - bytes->size() + 1));
            if (chunk.isEmpty()) break;
            bytes->append(chunk);
            if (bytes->size() > limit) { *failure = "response_limit"; reply->abort(); return; }
        }
    };
    connect(reply, &QIODevice::readyRead, reply, drain);
    connect(reply, &QNetworkReply::finished, this, [this, reply, bytes, failure, generation, cacheKey, cached, drain, callback = std::move(callback)] {
        reply->deleteLater();
        if (generation != m_generation) return;
        m_reply = nullptr;
        drain();
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (!failure->isEmpty()) { callback({}, *failure); return; }
        if (reply->error() == QNetworkReply::SslHandshakeFailedError) { callback({}, "tls"); return; }
        if (reply->error() == QNetworkReply::TimeoutError) { callback({}, "timeout"); return; }
        if (rateLimited(reply, status, *bytes)) { callback({}, "rate_limited"); return; }
        if (status == 304) {
            if (reply->error() == QNetworkReply::NoError && !cached.etag.isEmpty()) callback(cached.bytes, {});
            else callback({}, "invalid_cache");
            return;
        }
        if (reply->error() != QNetworkReply::NoError || status != 200) {
            callback({}, status == 404 ? "not_found" : status == 403 ? "forbidden" : "network"); return;
        }
        const auto etag = reply->rawHeader("ETag");
        if (!etag.isEmpty() && etag.size() <= 1024) {
            if (m_cache.size() >= 24) m_cache.clear();
            m_cache.insert(cacheKey, {etag, *bytes});
        }
        callback(*bytes, {});
    });
}
QString UpdateController::packageType() const { return m_platform == "windows" ? (installedRoot().isEmpty() ? "zip" : "exe") : m_platform == "linux" ? (installedRoot().isEmpty() ? "tar.gz" : "deb") : "app.zip"; }
void UpdateController::check() {
    if (m_packaged) { setState("store"); return; }
    if (busy()) return;
    const auto valid = updateRequest({{"operation", "compare"}, {"left", m_version}, {"right", m_version}});
    if (valid.value("comparison").toInt(2) != 0) { setState("error", "unknown_build"); return; }
    const auto generation = ++m_generation; m_download->cancel(); m_manifest.clear(); m_signature.clear(); m_candidate = {}; m_candidates.clear(); m_trusted = false;
    m_activeMirror = m_mirror; m_activeMode = m_mode; m_activeProxy = m_proxy;
    m_network->setProxyFactory(new UpdateNetwork::ProxyFactory(m_mode, m_proxy));
    setState("checking");
    if (generation == m_generation && busy()) fetchPage(1);
}
void UpdateController::cancel() {
    if (!busy() || m_state == "installing") return;
    ++m_generation; m_installRequested = false;
    if (m_reply) m_reply->abort(); m_reply = nullptr;
    m_download->cancel(); releasePreparation();
    if (!m_attemptPath.isEmpty() && !QFileInfo(m_attemptPath).isSymLink()) QFile::remove(m_attemptPath);
    setState("cancelled");
}
void UpdateController::fetchPage(int page) {
    fetch(QUrl(QStringLiteral("https://api.github.com/repos/xiaodaozhi/xdnote/releases?per_page=100&page=%1").arg(page)), 8 * 1024 * 1024,
          [this, page](QByteArray bytes, QString error) {
        if (!error.isEmpty()) { setState("error", error); return; }
        QJsonParseError parse; const auto doc = QJsonDocument::fromJson(bytes, &parse);
        if (parse.error != QJsonParseError::NoError || !doc.isArray() || doc.array().size() > 100) { setState("error", "invalid_response"); return; }
        const auto values = doc.array();
        for (const auto &value : values) {
            const auto release = value.toObject();
            if (!value.isObject() || !release.value("draft").isBool() || !release.value("tag_name").isString() || !release.value("assets").isArray()) { setState("error", "invalid_response"); return; }
            if (release.value("draft").toBool()) continue;
            const auto eligibility = updateRequest({{"operation", "eligible"}, {"tag", release.value("tag_name")}, {"current", m_version}, {"channel", m_channel}});
            if (!eligibility.value("eligible").isBool()) { setState("error", "update_bridge"); return; }
            if (eligibility.value("eligible").toBool()) m_candidates.append(release);
        }
        if (values.size() == 100) { if (page >= 10) setState("error", "page_limit"); else fetchPage(page + 1); return; }
        bool comparisonFailed = false;
        std::sort(m_candidates.begin(), m_candidates.end(), [&comparisonFailed](const QJsonObject &a, const QJsonObject &b) {
            const auto result = updateRequest({{"operation", "compare"}, {"left", a.value("tag_name").toString().mid(1)}, {"right", b.value("tag_name").toString().mid(1)}});
            const auto comparison = result.value("comparison").toInt(2);
            if (comparison == 2) comparisonFailed = true;
            return comparison == 1;
        });
        if (comparisonFailed) { setState("error", "update_bridge"); return; }
        inspectNext();
    });
}
void UpdateController::inspectNext() {
    if (m_candidates.isEmpty()) { setState("up_to_date"); return; }
    const auto release = m_candidates.takeFirst();
    const auto expected = "https://github.com/xiaodaozhi/xdnote/releases/download/" + release.value("tag_name").toString() + "/release.json";
    if (assetUrl(release, "release.json") != expected) { setState("error", "missing_manifest"); return; }
    fetch(QUrl(expected, QUrl::StrictMode), 256 * 1024, [this, release](QByteArray bytes, QString error) {
        if (!error.isEmpty()) { setState("error", error); return; } acceptManifest(release, bytes);
    });
}
void UpdateController::acceptManifest(const QJsonObject &release, const QByteArray &manifest) {
    if (!manifest.isValidUtf8()) { setState("error", "invalid_manifest_json"); return; }
    const auto inspection = updateRequest({{"operation", "inspect"}, {"manifest_hex", QString::fromLatin1(manifest.toHex())}, {"current", m_version},
        {"current_build", m_build}, {"current_channel", m_channel}, {"platform", m_platform}, {"architecture", m_architecture}, {"package_type", packageType()}});
    if (!inspection.value("valid").toBool()) { setState("error", inspection.value("error").toString()); return; }
    if (!inspection.value("newer").toBool()) { inspectNext(); return; }
    if ("v" + inspection.value("version").toString() != release.value("tag_name").toString()) { setState("error", "release_identity"); return; }
    int matches = 0;
    for (const auto &value : release.value("assets").toArray()) {
        const auto asset = value.toObject();
        if (asset.value("name") == inspection.value("filename") && asset.value("browser_download_url") == inspection.value("asset_url") &&
            asset.value("size").isDouble() && asset.value("size").toDouble() == inspection.value("bytes").toDouble()) ++matches;
    }
    if (matches != 1) { setState("error", "release_asset"); return; }
    m_candidate = inspection; m_manifest = manifest;
    const auto signature = assetUrl(release, "release.json.sig");
    if (signature.isEmpty()) { setState("available"); return; }
    const auto expected = "https://github.com/xiaodaozhi/xdnote/releases/download/" + release.value("tag_name").toString() + "/release.json.sig";
    if (signature != expected) { setState("error", "invalid_signature_url"); return; }
    fetch(QUrl(signature, QUrl::StrictMode), 4096, [this, manifest](QByteArray bytes, QString error) {
        if (!error.isEmpty()) { setState("error", error); return; }
        if (!bytes.isValidUtf8()) { setState("error", "invalid_signature_json"); return; }
        const auto result = updateRequest({{"operation", "authenticate"}, {"manifest_hex", QString::fromLatin1(manifest.toHex())}, {"signature_hex", QString::fromLatin1(bytes.toHex())},
            {"current", m_version}, {"current_build", m_build}, {"current_channel", m_channel}, {"platform", m_platform}, {"architecture", m_architecture}, {"package_type", packageType()}});
        const auto code = result.value("error").toString("update_bridge");
        if (code.isEmpty() && result.value("candidate").toObject() == m_candidate) { m_signature = bytes; m_trusted = true; setState("available"); }
        else if (code == "untrusted_signing_key") setState("available");
        else setState("error", code);
    });
}
bool UpdateController::busy() const {
    return m_state == "checking" || m_state == "downloading" || m_state == "verifying" || m_state == "preparing" || m_state == "waiting_tasks" || m_state == "installing";
}
QString UpdateController::installedRoot() const {
    if (m_packaged || !BuildInfo::release()) return {};
    const auto executable = QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath();
#ifdef Q_OS_WIN
    QSettings settings("HKEY_CURRENT_USER\\Software\\xdnote\\Install", QSettings::NativeFormat);
    const auto root = QFileInfo(settings.value("Location").toString()).canonicalFilePath();
    if (settings.value("InstallId").toString() == "460D7C68-C989-45F0-B312-40AE18428155" && !root.isEmpty() &&
        executable.compare(root + "/xdnote.exe", Qt::CaseInsensitive) == 0 && QFileInfo(root + "/install-manifest.txt").isFile()) return root;
#elif defined(Q_OS_LINUX)
    const auto root = QFileInfo(QFileInfo(executable).absolutePath()).absolutePath();
    if (root.startsWith("/opt/xdnote/versions/") && QFileInfo(executable).ownerId() == 0 && QFileInfo("/usr/libexec/xdnote-update-helper").ownerId() == 0 && QFileInfo(root + "/share/xdnote-install.json").isFile()) return root;
#elif defined(Q_OS_MACOS)
    return macUpdateInstallRoot();
#endif
    return {};
}
bool UpdateController::canDownload() const { return !m_packaged && m_trusted && m_state == "available"; }
bool UpdateController::canInstall() const {
    return !m_packaged && m_trusted && !installedRoot().isEmpty() && m_application && (m_state == "available" || m_state == "downloaded")
#ifdef Q_OS_MACOS
        && !m_candidate.value("appcast_url").toString().isEmpty() && UpdateNetwork::macosAutomaticTransport(m_activeMode, m_activeProxy)
#endif
        ;
}
QString UpdateController::downloadedPath() const { return m_download->packagePath(); }
void UpdateController::setApplication(FileApplication *application) { m_application = application; }
void UpdateController::download() {
    if (!canDownload()) return;
    setState("downloading"); m_download->start(m_candidate, m_activeMirror);
}
void UpdateController::openDownloadFolder() {
    if (!downloadedPath().isEmpty() && m_state == "downloaded") QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(downloadedPath()).absolutePath()));
}
void UpdateController::install() {
    if (!canInstall()) return;
    m_installRequested = true;
#ifdef Q_OS_MACOS
    prepareInstallation();
#else
    if (downloadedPath().isEmpty()) download(); else prepareInstallation();
#endif
}
void UpdateController::releasePreparation() {
    if (m_handoff) {
        m_handoff->cancel(); m_handoff->deleteLater(); m_handoff = nullptr;
        // Its independently running helper still needs the cancellation marker.
        // The leased transaction survives; a retry gets a new download/job.
        m_download->cancel();
    }
#ifdef Q_OS_MACOS
    cancelMacUpdate();
#endif
    if (m_application) m_application->cancelUpdateHandoff();
}
void UpdateController::reportNativeProgress(const QString &state, qreal progress) {
    Q_UNUSED(progress);
    if (state == "error") { m_installRequested = false; releasePreparation(); setState("recovery_required", "native_update"); }
    else if (m_installRequested && m_state != "installing") setState(state);
}
void UpdateController::prepareInstallation() {
    if (!m_installRequested || !m_application || installedRoot().isEmpty()) return;
    const auto generation = m_generation;
    const auto prepared = [this, generation](const QString &error) {
        if (generation != m_generation || !m_installRequested) return;
        if (!error.isEmpty()) { m_installRequested = false; releasePreparation(); setState("error", error); return; }
        setState("waiting_tasks");
        m_application->prepareForUpdate([this, generation](bool ready) {
            if (generation != m_generation || !m_installRequested) return;
            if (!ready) { m_installRequested = false; releasePreparation(); setState("available", "cancelled"); return; }
            if (!rememberAttempt("prepared")) { m_installRequested = false; releasePreparation(); setState("error", "handoff_receipt"); return; }
            if (!m_application->sealUpdateRecovery()) { m_installRequested = false; releasePreparation(); setState("error", "handoff_recovery"); return; }
#ifdef Q_OS_MACOS
            setMacUpdateCommitCallback(this, [this] { setState("installing"); });
            const bool committed = rememberAttempt("committed") && commitMacUpdate();
#else
            const bool committed = rememberAttempt("committed") && m_handoff && m_handoff->commit();
#endif
            if (!committed) { m_installRequested = false; releasePreparation(); setState("recovery_required", "handoff"); return; }
            if (m_download) m_download->retainForHelper();
            setState("installing");
#ifndef Q_OS_MACOS
            emit quitForUpdate();
#endif
        });
    };
    setState("preparing");
#ifdef Q_OS_MACOS
    m_transaction = QUuid::createUuid().toString(QUuid::Id128);
    startMacUpdate(this, m_candidate, prepared, {m_activeMode, m_activeProxy, m_activeMirror});
#else
    if (downloadedPath().isEmpty()) { m_installRequested = false; setState("error", "missing_package"); return; }
    m_handoff = new UpdateHandoff(this);
    connect(m_handoff, &UpdateHandoff::ready, this, [this, prepared] { m_transaction = m_handoff->transaction(); prepared({}); });
    connect(m_handoff, &UpdateHandoff::failed, this, prepared);
    m_download->retainForHelper();
    m_handoff->prepare(m_download->directory(), installedRoot(), m_manifest, m_signature, downloadedPath());
#endif
}
bool UpdateController::rememberAttempt(const QString &phase) {
    if (m_attemptPath.isEmpty() || QFileInfo(m_attemptPath).isSymLink() || !QDir().mkpath(QFileInfo(m_attemptPath).absolutePath())) return false;
    QSaveFile file(m_attemptPath); file.setDirectWriteFallback(false);
    const auto bytes = QJsonDocument(QJsonObject{{"schema_version", 1}, {"transaction", m_transaction}, {"phase", phase},
        {"version", m_candidate.value("version")}, {"build", m_candidate.value("build")}, {"commit", m_candidate.value("commit")},
        {"root", installedRoot()}, {"directory", m_download->directory()}, {"executable", QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath()}}).toJson(QJsonDocument::Compact);
    return file.open(QIODevice::WriteOnly) && file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) && file.write(bytes) == bytes.size() && file.commit();
}
void UpdateController::recoverAttempt() {
    if (!QFileInfo::exists(m_attemptPath)) return;
    QFile file(m_attemptPath);
    if (QFileInfo(m_attemptPath).isSymLink() || !file.open(QIODevice::ReadOnly) || file.size() > 16384) { setState("recovery_required", "handoff_receipt"); return; }
    const auto record = QJsonDocument::fromJson(file.readAll()).object();
    if (record.value("phase") == "completed") return;
    const bool identity = record.value("schema_version").toInt() == 1 && record.value("phase") == "committed" &&
        record.value("transaction").toString().size() == 32 && record.value("version") == BuildInfo::version() && record.value("build").toInt() == BuildInfo::buildNumber() &&
        record.value("commit") == BuildInfo::revision() && !installedRoot().isEmpty() &&
        updateReceiptPaths(m_platform, record.value("root").toString(), record.value("executable").toString(), installedRoot(), QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath());
    bool receipt = false;
#ifdef Q_OS_MACOS
    receipt = record.value("root") == installedRoot();
#else
    const auto directory = record.value("directory").toString();
    const auto base = QFileInfo(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/updates").canonicalFilePath();
    const auto canonical = QFileInfo(directory).canonicalFilePath();
    if (!base.isEmpty() && canonical.startsWith(base + "/job-") && !QFileInfo(directory).isSymLink()) {
        QFile result(canonical + "/result.json");
        if (!QFileInfo(result).isSymLink() && result.open(QIODevice::ReadOnly) && result.size() <= 16384) {
            const auto value = QJsonDocument::fromJson(result.readAll()).object();
            receipt = value.value("transaction") == record.value("transaction") && value.value("error").isString() && value.value("error").toString().isEmpty();
        }
    }
#endif
    if (!identity || !receipt) { setState("recovery_required", "installation_interrupted"); return; }
    auto completed = record; completed.insert("phase", "completed"); file.close();
    QSaveFile output(m_attemptPath); const auto bytes = QJsonDocument(completed).toJson(QJsonDocument::Compact);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) { setState("recovery_required", "handoff_receipt"); return; }
    setState("updated");
}
void UpdateController::openReleasePage() {
    if (m_packaged) return;
    const auto candidate = QUrl(m_candidate.value("release_url").toString(), QUrl::StrictMode);
    const QUrl releases("https://github.com/xiaodaozhi/xdnote/releases");
    QDesktopServices::openUrl(m_state == "available" && allowedUrl(candidate) ? candidate : releases);
}
QString UpdateController::message() const {
    if (m_packaged || m_state == "store") return QStringLiteral("此副本由 Microsoft Store 管理更新。");
    if (m_state == "updated") return QStringLiteral("更新已完成，已核对当前安装身份。");
    if (m_state == "recovery_required") return QStringLiteral("更新未能确认完成。请保留当前副本并从发布页手动恢复。");
    if (m_state == "downloading") return QStringLiteral("正在下载并校验更新包…");
    if (m_state == "downloaded") return QStringLiteral("更新包已校验，可打开下载目录或继续安装。");
    if (m_state == "preparing" || m_state == "verifying") return QStringLiteral("正在准备原生更新…");
    if (m_state == "waiting_tasks") return QStringLiteral("正在保护文档并等待后台任务结束…");
    if (m_state == "installing") return QStringLiteral("正在交接安装，完成后将重启应用。");
    if (m_state == "checking") return QStringLiteral("正在检查更新…");
    if (m_state == "up_to_date") return QStringLiteral("当前没有适用的公开更新。");
    if (m_state == "cancelled") return QStringLiteral("已取消检查。");
    if (m_state == "available" && m_trusted) return QStringLiteral("发现已认证的 %1。可下载更新包%2。").arg(candidateVersion(), canInstall() ? QStringLiteral("或安装并重启") : QStringLiteral("后手动安装"));
    if (m_state == "available") return QStringLiteral("发现 %1。此开发副本仅支持前往发布页手动更新。").arg(candidateVersion());
    if (m_state == "error") {
        if (m_error == "rate_limited") return QStringLiteral("检查受到限流，请稍后手动重试。");
        if (m_error == "tls") return QStringLiteral("无法验证安全连接，检查失败。");
        if (m_error == "timeout") return QStringLiteral("检查更新超时，请手动重试。");
        if (m_error == "unknown_build") return QStringLiteral("无法识别当前构建版本，请查看发布页。");
        return QStringLiteral("检查更新失败，请稍后重试或查看发布页。");
    }
    return QStringLiteral("仅在你点击时检查公开更新；此开发副本使用手动更新。");
}
}
