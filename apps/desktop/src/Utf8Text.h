#pragma once

#include <QByteArrayView>
#include <QStringDecoder>

namespace xdnote {
// The file service removes exactly one encoding BOM. All characters remaining
// in a document, including an initial U+FEFF, participate in editor offsets.
inline QString decodeUtf8Literal(QByteArrayView bytes, bool *valid = nullptr) {
    QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless | QStringDecoder::Flag::ConvertInitialBom);
    const QString text = decoder.decode(bytes);
    if (valid) *valid = !decoder.hasError();
    return text;
}
}
