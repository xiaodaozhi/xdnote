#include "spike_ipc.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QLocalSocket>
#include <QUuid>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList arguments = app.arguments();
    if (arguments.size() != 3)
        return 2;

    QLocalSocket socket;
    socket.connectToServer(arguments.at(1));
    if (!socket.waitForConnected(5000))
        return 3;

    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QJsonObject request{
        {QStringLiteral("version"), 1},
        {QStringLiteral("requestId"), id},
        {QStringLiteral("path"), arguments.at(2)},
    };
    const QByteArray frame = SpikeIpc::encode(request);
    if (frame.isEmpty() || socket.write(frame) != frame.size())
        return 4;
    if (socket.bytesToWrite() > 0 && !socket.waitForBytesWritten(5000))
        return 5;

    QElapsedTimer deadline;
    deadline.start();
    QByteArray received;
    while (deadline.elapsed() < 5000) {
        if (socket.bytesAvailable() == 0
            && !socket.waitForReadyRead(int(5000 - deadline.elapsed())))
            return 6;
        received += socket.readAll();
        QJsonObject reply;
        QString error;
        const auto state = SpikeIpc::takeFrame(&received, &reply, &error);
        if (state == SpikeIpc::DecodeResult::Invalid)
            return 7;
        if (state == SpikeIpc::DecodeResult::Ready) {
            return reply.value(QStringLiteral("version")).toInt() == 1
                    && reply.value(QStringLiteral("requestId")).toString() == id
                    && reply.value(QStringLiteral("accepted")).toBool()
                ? 0 : 8;
        }
    }
    return 9;
}
