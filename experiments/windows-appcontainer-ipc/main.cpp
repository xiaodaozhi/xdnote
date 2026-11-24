#include "SingleInstance.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QCryptographicHash>
#include <QLocalSocket>
#include <QProcess>
#include <QThread>
#include <memory>
#include <vector>
#include <cstdio>
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv); const auto args=app.arguments();
    if(args.size()!=3) return 2;
    xdnote::SingleInstance instance;
    const QString expected=args[2]+QStringLiteral("/含空格;文档.md");
    if(args[1]=="client") return instance.start(args[2], {expected}, [](QStringList){})==xdnote::SingleInstance::Result::Forwarded ? 0 : 3;
    int received=0;
    if(instance.start(args[2], {}, [&](QStringList paths){if(paths==QStringList{expected}) ++received;})!=xdnote::SingleInstance::Result::Primary) {std::puts("primary: failed");return 4;}
    std::puts("primary: ready"); std::fflush(stdout);
    if(std::getchar()!='x') return 7;
    for(int i=0;i<3;++i) {
        QProcess client; client.start(QCoreApplication::applicationFilePath(), {"client",args[2]});
        if(!client.waitForStarted(3000)) return 5;
        QElapsedTimer timer;timer.start();
        while(client.state()!=QProcess::NotRunning && timer.elapsed()<10000) {app.processEvents();client.waitForFinished(20);}
        app.processEvents();
        if(client.state()!=QProcess::NotRunning || client.exitCode()!=0 || received!=i+1) {std::printf("client %d failed, received=%d exit=%d\n",i,received,client.exitCode());return 6;}
        std::printf("client %d forwarded\n",i+1);
    }
    std::vector<std::unique_ptr<QProcess>> clients;
    for(int i=0;i<4;++i) {
        auto client=std::make_unique<QProcess>(); client->start(QCoreApplication::applicationFilePath(), {"client",args[2]});
        if(!client->waitForStarted(3000)) return 8;
        clients.push_back(std::move(client));
    }
    QElapsedTimer timer;timer.start();
    while(timer.elapsed()<10000) {
        app.processEvents();int running=0;
        for(const auto &client:clients) {client->waitForFinished(10);if(client->state()!=QProcess::NotRunning)++running;}
        if(!running)break;
    }
    app.processEvents();
    for(const auto &client:clients) if(client->state()!=QProcess::NotRunning || client->exitCode()!=0) return 9;
    if(received!=7) return 10;
    std::puts("four concurrent clients forwarded");
    const auto name=QStringLiteral("xdnote-v1-")+QString::fromLatin1(QCryptographicHash::hash(args[2].toUtf8(),QCryptographicHash::Sha256).toHex().left(24));
    QLocalSocket bad;bad.connectToServer(name);if(!bad.waitForConnected(3000)) return 11;
    bad.write(QByteArray(4, char(0xff)));bad.flush();
    QElapsedTimer badTimer;badTimer.start();
    while(bad.state()!=QLocalSocket::UnconnectedState && badTimer.elapsed()<5000) {app.processEvents();bad.waitForReadyRead(10);}
    if(bad.state()!=QLocalSocket::UnconnectedState) return 12;
    QProcess recovered;recovered.start(QCoreApplication::applicationFilePath(),{"client",args[2]});
    if(!recovered.waitForStarted(3000)) return 13;
    QElapsedTimer recoveryTimer;recoveryTimer.start();
    while(recovered.state()!=QProcess::NotRunning && recoveryTimer.elapsed()<10000) {app.processEvents();recovered.waitForFinished(20);}
    app.processEvents();if(recovered.state()!=QProcess::NotRunning || recovered.exitCode()!=0 || received!=8) return 14;
    std::puts("invalid request rejected, next client forwarded");
    instance.stop(); std::puts("stopped"); return 0;
}
