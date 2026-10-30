#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace SpikeIpc {
constexpr quint32 maxPayloadBytes = 64 * 1024;
enum class DecodeResult { NeedMore, Ready, Invalid };

QByteArray encode(const QJsonObject &object);
DecodeResult takeFrame(QByteArray *input, QJsonObject *object, QString *error);
} // namespace SpikeIpc
