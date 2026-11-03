#pragma once
#include <QDateTime>
#include <QJsonObject>
#include <QObject>

namespace xdnote {
struct NavigationEntry { QString path; QDateTime opened; };
class NavigationStore final : public QObject {
    Q_OBJECT
public:
    explicit NavigationStore(QObject *parent = nullptr, QString path = {});
    QList<NavigationEntry> recent() const { return m_recent; }
    QStringList favorites() const { return m_favorites; }
    bool favorite(const QString &path) const;
    bool recordOpen(const QString &path);
    bool setFavorite(const QString &path, bool favorite);
    QString error() const { return m_error; }
signals:
    void changed();
    void storageFailed(QString message);
private:
    bool commit(const QList<NavigationEntry> &recent, const QStringList &favorites);
    bool fail(const QString &message);
    QString m_path, m_error;
    QJsonObject m_settings;
    QList<NavigationEntry> m_recent;
    QStringList m_favorites;
    bool m_valid = true;
};
}
