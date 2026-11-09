#include "WorkspaceImagePreview.h"
#include "FileService.h"
#include "MarkdownReferences.h"
#include "WorkspacePath.h"

#include <QBuffer>
#include <QImageReader>
#include <QImageWriter>
#include <QSet>

namespace xdnote {
namespace {
constexpr qsizetype MaxImageBytes = 8 * 1024 * 1024;
constexpr qsizetype MaxBatchBytes = 32 * 1024 * 1024;
constexpr int MaxImages = 32;
constexpr qint64 MaxImagePixels = 12'000'000;
constexpr qint64 MaxBatchPixels = 24'000'000;

class LimitedOutput final : public QIODevice {
public:
    explicit LimitedOutput(qsizetype limit) : m_limit(limit) { open(QIODevice::WriteOnly); }
    bool isSequential() const override { return true; }
    QByteArray take() { return std::move(m_bytes); }
protected:
    qint64 readData(char *, qint64) override { return -1; }
    qint64 writeData(const char *data, qint64 length) override {
        if (length < 0 || length > m_limit - m_bytes.size()) return -1;
        m_bytes.append(data, qsizetype(length));
        return length;
    }
private:
    qsizetype m_limit;
    QByteArray m_bytes;
};

char readFailure(WorkspaceReadError error) {
    if (error == WorkspaceReadError::TooLarge) return 3;
    if (error == WorkspaceReadError::InvalidPath || error == WorkspaceReadError::OutsideOrLink) return 2;
    return 5;
}

bool verifiedDocument(const std::shared_ptr<WorkspaceResourceReader> &root,
                      const QString &path, const QString &identity) {
    if (!root || path.isEmpty() || identity.isEmpty()) return false;
    const auto file = root->read(path, FileService::SourceLimit);
    return file.ok() && file.identity == identity;
}

char decodeImage(const QByteArray &source, qint64 &pixels, qsizetype outputLimit, QByteArray &png) {
    QBuffer input;
    input.setData(source);
    if (!input.open(QIODevice::ReadOnly)) return 4;
    QImageReader reader(&input);
    reader.setAutoTransform(true);
    if (!reader.canRead()) return 4;
    static const QSet<QByteArray> admitted{"png", "jpeg", "jpg", "gif", "bmp", "webp"};
    if (!admitted.contains(reader.format().toLower())) return 4;
    const auto size = reader.size();
    if (!size.isValid() || size.width() > 8192 || size.height() > 8192) return 3;
    const qint64 count = qint64(size.width()) * size.height();
    if (count > MaxImagePixels || count > MaxBatchPixels - pixels) return 3;
    const auto image = reader.read();
    if (image.isNull()) return 4;
    if (image.width() > 8192 || image.height() > 8192 || qint64(image.width()) * image.height() != count) return 4;
    LimitedOutput output(outputLimit);
    QImageWriter writer(&output, "png");
    if (!writer.write(image)) return 3;
    png = output.take();
    pixels += count;
    return 1;
}
}

PreviewImages preparePreviewImages(const DocumentSnapshot &snapshot,
    const std::shared_ptr<WorkspaceResourceReader> &root,
    const QString &documentPath, const QString &documentIdentity) {
    PreviewImages result;
    const auto analysis = analyzeMarkdownReferences(snapshot);
    if (!analysis.success || !analysis.complete) return result;
    result.analyzed = true;
    bool hasImages = false;
    for (const auto &reference : analysis.references)
        if (reference.kind == MarkdownReference::Kind::Image) { hasImages = true; break; }
    if (!hasImages) return result;
    const bool documentAuthorized = verifiedDocument(root, documentPath, documentIdentity);
    qsizetype inputBytes = 0, outputBytes = 0;
    qint64 pixels = 0;
    for (const auto &reference : analysis.references) {
        if (reference.kind != MarkdownReference::Kind::Image) continue;
        const int index = int(result.states.size());
        char state = 5;
        if (!documentAuthorized) state = 5;
        else if (index >= MaxImages) state = 3;
        else {
            const auto target = resolveWorkspaceTarget(documentPath, reference.target);
            if (!target.success || target.hasFragment) state = 2;
            else {
                const auto image = root->read(target.path, MaxImageBytes);
                if (!image.ok()) state = readFailure(image.error);
                else if (image.bytes.size() > MaxBatchBytes - inputBytes) state = 3;
                else {
                    inputBytes += image.bytes.size();
                    QByteArray png;
                    state = decodeImage(image.bytes, pixels, MaxBatchBytes - outputBytes, png);
                    if (state == 1) { outputBytes += png.size(); result.png.insert(index, std::move(png)); }
                }
            }
        }
        result.states.append(state);
    }
    // Do not return content captured under an obsolete root or replaced document.
    if (documentAuthorized && !verifiedDocument(root, documentPath, documentIdentity)) {
        result.png.clear();
        result.states.fill(5);
    }
    return result;
}
}
