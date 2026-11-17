#pragma once
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QNetworkAccessManager>
#include <QPointer>
#include <functional>
class QNetworkReply;
namespace xdnote {
class FileApplication;
class UpdateDownload;
class UpdateHandoff;
class Preferences;
class UpdateController final : public QObject {
    Q_OBJECT
public:
    explicit UpdateController(QObject *parent = nullptr);
    // The transport injection supports isolated network fixtures; it cannot
    // override production trust or create an installation entry point.
    UpdateController(QNetworkAccessManager *transport, QString version, QString platform,
                     QString architecture, bool packaged, QObject *parent = nullptr);
    ~UpdateController() override;
    QString state() const { return m_state; }
    QString errorCode() const { return m_error; }
    QString message() const;
    QString candidateVersion() const { return m_candidate.value("version").toString(); }
    bool busy() const;
    bool packaged() const { return m_packaged; }
    bool trusted() const { return m_trusted; }
    bool canDownload() const;
    bool canInstall() const;
    QString downloadedPath() const;
    void setApplication(FileApplication *application);
    void reportNativeProgress(const QString &state, qreal progress);
    bool configureNetwork(const QString &mode, const QString &proxy = {}, const QString &mirror = {});
    void bindPreferences(Preferences *preferences);
    static bool allowedUrl(const QUrl &url);
public slots:
    void check();
    void cancel();
    void openReleasePage();
    void download();
    void install();
    void openDownloadFolder();
signals:
    void changed();
    void quitForUpdate();
private:
    using Callback = std::function<void(QByteArray, QString)>;
    void setState(const QString &state, const QString &error = {});
    void fetch(const QUrl &url, qsizetype limit, Callback callback);
    void fetchPage(int page);
    void inspectNext();
    void acceptManifest(const QJsonObject &release, const QByteArray &manifest);
    QString packageType() const;
    QString installedRoot() const;
    void prepareInstallation();
    void releasePreparation();
    bool rememberAttempt(const QString &phase);
    void recoverAttempt();
    QNetworkAccessManager *m_network;
    QString m_version, m_platform, m_architecture;
    bool m_packaged = false, m_trusted = false;
    QString m_state = "idle", m_error;
    struct Cache { QByteArray etag, bytes; };
    QHash<QString, Cache> m_cache;
    QPointer<QNetworkReply> m_reply;
    QList<QJsonObject> m_candidates;
    QJsonObject m_candidate;
    QByteArray m_manifest, m_signature;
    QString m_channel = "development";
    int m_build = 0;
    QPointer<FileApplication> m_application;
    QPointer<Preferences> m_preferences;
    QMetaObject::Connection m_preferencesChanged;
    UpdateDownload *m_download = nullptr;
    UpdateHandoff *m_handoff = nullptr;
    bool m_installRequested = false;
    QString m_activeMode, m_activeProxy, m_transaction, m_attemptPath;
    QString m_mode = "system", m_proxy, m_mirror, m_activeMirror;
    quint64 m_generation = 0;
};
}
