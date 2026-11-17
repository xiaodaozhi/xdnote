// apps/desktop/UpdateHandoff.h

#pragma once
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
namespace xdnote {
class UpdateHandoff final : public QObject {
    Q_OBJECT
  public:
    explicit UpdateHandoff(QObject *parent = nullptr);
    void prepare(const QString &directory, const QString &installRoot, const QByteArray &manifest,
                 const QByteArray &signature, const QString &package);
    bool commit();
    QString transaction() const {
        return m_transaction;
    }
    void cancel();
  signals:
    void ready();
    void failed(const QString &code);

  private:
    bool writeControl(const QString &name);
    void poll();
    QString m_directory, m_transaction;
    QTimer m_poll;
    QElapsedTimer m_elapsed;
    qint64 m_processId = 0;
    bool m_ready = false;
};

}
