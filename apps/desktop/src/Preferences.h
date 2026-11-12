#pragma once
#include <QObject>
#include <QJsonObject>

namespace xdnote {
// One application-scoped store. Publish only after the entire change commits.
class Preferences final : public QObject {
    Q_OBJECT
public:
    explicit Preferences(QObject *parent = nullptr, QString path = {});
    QJsonValue value(const QString &section, const QString &key) const;
    QJsonObject section(const QString &name) const { return m_values.value(name).toObject(); }
    bool setValues(const QString &section, const QJsonObject &patch);
    QString error() const { return m_error; }
    bool canRetryWrite() const { return m_retryWrite; }
    bool retryWrite();
signals:
    void changed(QString section);
    void writeFailed(QString message);
private:
    bool fail(const QString &message);
    bool valid(const QString &section, const QString &key, const QJsonValue &value) const;
    QString m_path, m_error;
    QJsonObject m_values, m_defaults;
    bool m_valid = true;
    bool m_retryWrite = false;
    QString m_failedSection;
    QJsonObject m_failedPatch;
};
}
