#include "ImageAdmission.h"
#include <QBuffer>
#include <QImageReader>
#include <QImageWriter>
#include <QSet>

namespace xdnote {
namespace {
constexpr qint64 MaxImagePixels = 12'000'000;
class LimitedOutput final : public QIODevice {
public:
    explicit LimitedOutput(qsizetype limit) : m_limit(limit) { open(QIODevice::WriteOnly); }
    bool isSequential() const override { return true; }
    QByteArray take() { return std::move(m_bytes); }
protected:
    qint64 readData(char *, qint64) override { return -1; }
    qint64 writeData(const char *data, qint64 length) override {
        if (length < 0 || length > m_limit - m_bytes.size()) return -1;
        m_bytes.append(data, qsizetype(length)); return length;
    }
private:
    qsizetype m_limit;
    QByteArray m_bytes;
};
}

char decodeImage(const QByteArray &source, qint64 &pixels, qsizetype outputLimit, QByteArray &png, QByteArray *format) {
    if (source.isEmpty() || source.size() > MaxImageSourceBytes || outputLimit < 0) return 3;
    QBuffer input; input.setData(source);
    if (!input.open(QIODevice::ReadOnly)) return 4;
    QImageReader reader(&input); reader.setAutoTransform(true);
    if (!reader.canRead()) return 4;
    const auto actualFormat = reader.format().toLower();
    static const QSet<QByteArray> admitted{"png", "jpeg", "jpg", "gif", "bmp", "webp"};
    if (!admitted.contains(actualFormat)) return 4;
    const auto size = reader.size();
    if (!size.isValid() || size.width() > 8192 || size.height() > 8192) return 3;
    const qint64 count = qint64(size.width()) * size.height();
    if (count > MaxImagePixels || count > MaxBatchImagePixels - pixels) return 3;
    const auto image = reader.read();
    if (image.isNull()) return 4;
    if (image.width() > 8192 || image.height() > 8192 || qint64(image.width()) * image.height() != count) return 4;
    LimitedOutput output(outputLimit); QImageWriter writer(&output, "png");
    if (!writer.write(image)) return 3;
    png = output.take(); pixels += count;
    if (format) *format = actualFormat == "jpeg" ? QByteArray("jpg") : actualFormat;
    return 1;
}

bool encodeClipboardImage(const QImage &image, QByteArray &png) {
    if (image.isNull() || image.width() > 8192 || image.height() > 8192
        || qint64(image.width()) * image.height() > MaxImagePixels) return false;
    LimitedOutput output(MaxImageSourceBytes); QImageWriter writer(&output, "png");
    if (!writer.write(image)) return false;
    png = output.take(); return !png.isEmpty();
}
}
