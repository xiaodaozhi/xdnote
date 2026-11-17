#pragma once
#include <QObject>
#include <QPointer>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QSaveFile>
#include <QCryptographicHash>
#include <memory>
class QNetworkAccessManager;
class QNetworkReply;
namespace xdnote {
// A byte transport, never an installation authorization. The controller supplies
// only the candidate returned by Rust authentication; helpers reauthenticate it.
class UpdateDownload final : public QObject {
    Q_OBJECT
public:
    explicit UpdateDownload(QNetworkAccessManager *network, QObject *parent = nullptr);
    ~UpdateDownload() override;
    void start(const QJsonObject &candidate, const QString &mirror = {}, const QString &cache = {});
    void cancel();
    QString packagePath() const { return m_path; }
    QString directory() const { return m_directory ? m_directory->path() : QString(); }
    void retainForHelper();
    bool active() const { return !m_reply.isNull(); }
signals:
    void progress(qint64 received, qint64 total);
    void finished(QString error);
private:
    void drain();
    void fail(const QString &error);
    QNetworkAccessManager *m_network;
    QPointer<QNetworkReply> m_reply;
    std::unique_ptr<QTemporaryDir> m_directory;
    std::unique_ptr<QSaveFile> m_file;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    QString m_path, m_error, m_expectedHash;
    qint64 m_expected = 0, m_received = 0;
    quint64 m_generation = 0;
};
}
