#pragma once
#include <QByteArray>
#include <QImage>

namespace xdnote {
constexpr qsizetype MaxImageSourceBytes = 8 * 1024 * 1024;
// 1: accepted, 3: size limit, 4: invalid or unsupported format. The preview
// receives a static PNG; import retains its original source bytes.
char decodeImage(const QByteArray &source, qint64 &pixels, qsizetype outputLimit,
                 QByteArray &png, QByteArray *format = nullptr);
bool encodeClipboardImage(const QImage &image, QByteArray &png);
}
