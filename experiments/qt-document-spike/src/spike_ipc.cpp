#include "spike_ipc.h"

#include <QDataStream>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QtEndian>

QByteArray SpikeIpc::encode(const QJsonObject &object)
{
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (payload.isEmpty() || payload.size() > maxPayloadBytes)
        return {};
    QByteArray frame;
    QDataStream stream(&frame, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << quint32(payload.size());
    frame.append(payload);
    return frame;
}

SpikeIpc::DecodeResult SpikeIpc::takeFrame(QByteArray *input, QJsonObject *object, QString *error)
{
    if (input->size() < 4)
        return DecodeResult::NeedMore;
    const quint32 length = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(input->constData()));
    if (length == 0 || length > maxPayloadBytes) {
        *error = QStringLiteral("Invalid frame length");
        return DecodeResult::Invalid;
    }
    if (input->size() < qsizetype(length) + 4)
        return DecodeResult::NeedMore;

    QJsonParseError parseError;
    const QJsonDocument parsed = QJsonDocument::fromJson(input->mid(4, length), &parseError);
    if (parseError.error != QJsonParseError::NoError || !parsed.isObject()) {
        *error = QStringLiteral("Invalid JSON object");
        return DecodeResult::Invalid;
    }
    *object = parsed.object();
    input->remove(0, qsizetype(length) + 4);
    error->clear();
    return DecodeResult::Ready;
}
