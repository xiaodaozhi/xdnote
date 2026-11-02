#pragma once
#include <QLocalServer>
#include <QLockFile>
#include <QStringList>
#include <functional>
#include <memory>
namespace xdnote {
class SingleInstance final : public QObject {
public:
    enum class Result { Primary, Forwarded, Error };
    explicit SingleInstance(QObject *parent = nullptr) : QObject(parent), m_server(this) {}
    ~SingleInstance() override { stop(); }
    void stop();
    Result start(const QString &scopeDirectory, const QStringList &absolutePaths, std::function<void(QStringList)> accepted);
    QString error() const { return m_error; }
private:
    QLocalServer m_server;
    std::unique_ptr<QLockFile> m_lock;
    QString m_error;
    int m_connections = 0;
    bool m_accepting = false;
};
}
