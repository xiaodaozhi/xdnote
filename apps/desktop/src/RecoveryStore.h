#pragma once
#include <QObject>
#include <QJsonObject>
#include <QFutureWatcher>
namespace xdnote {
// Application-owned state only; never writes a document or deletes a source.
class RecoveryStore final : public QObject {
    Q_OBJECT
public:
    RecoveryStore(QString path,QObject *parent=nullptr);
    ~RecoveryStore() override;
    QJsonObject state() const { return m_state; }
    QString error() const { return m_error; }
    void save(QJsonObject state);
    bool flush();
    static bool validate(const QJsonObject &state);
signals:
    void failed(QString message);
    void persisted();
private:
    void startWrite();
    QString m_path,m_error;
    QJsonObject m_state;
    QByteArray m_pending,m_lastQueued;
    QFutureWatcher<QString> m_writer;
    bool m_valid=true,m_active=false;
};
}
