#include "WorkspaceImagePreview.h"
#include "ImageAdmission.h"
#include "FileService.h"
#include "MarkdownReferences.h"
#include "WorkspacePath.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QElapsedTimer>

namespace xdnote {
namespace {
constexpr qsizetype MaxBatchBytes = 32 * 1024 * 1024;
constexpr int MaxImages = 32;

char readFailure(WorkspaceReadError error) {
    if (error == WorkspaceReadError::TooLarge) return 3;
    if (error == WorkspaceReadError::InvalidPath || error == WorkspaceReadError::OutsideOrLink) return 2;
    return 5;
}

bool verifiedDocument(const std::shared_ptr<WorkspaceResourceReader> &root,
                      const QString &path, const QString &identity) {
    if (!root || path.isEmpty() || identity.isEmpty()) return false;
    const auto file = root->read(path, FileService::DiskByteLimit);
    return file.ok() && file.identity == identity;
}

}

PreviewImages preparePreviewImages(const DocumentSnapshot &snapshot,
    const std::shared_ptr<WorkspaceResourceReader> &root,
    const QString &documentPath, const QString &documentIdentity, const AuthorizedPreviewImages &authorizedImages) {
    return preparePreviewImages(snapshot, analyzeMarkdownReferences(snapshot), root, documentPath, documentIdentity, authorizedImages, {}, {});
}

char PreviewImageCache::decode(const QByteArray &bytes, qint64 &pixels, qsizetype limit, QByteArray &png, bool &reused) {
    reused = false;
    const auto key = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    {
        QMutexLocker lock(&m_mutex);
        if (const auto hit = m_images.object(key)) {
            if (hit->pixels > MaxBatchImagePixels - pixels || hit->png.size() > limit) return 3;
            pixels += hit->pixels; png = hit->png; reused = true; return 1;
        }
    }
    const auto before = pixels;
    const char status = decodeImage(bytes, pixels, limit, png);
    if (status == 1 && png.capacity() + 128 <= m_images.maxCost()) {
        QMutexLocker lock(&m_mutex);
        m_images.insert(key, new Image{png, pixels - before}, int(png.capacity() + 128));
    }
    return status;
}
void PreviewImageCache::clear() { QMutexLocker lock(&m_mutex); m_images.clear(); }
qsizetype PreviewImageCache::retainedBytes() { QMutexLocker lock(&m_mutex); return m_images.totalCost(); }

PreviewImages preparePreviewImages(const DocumentSnapshot &snapshot, const MarkdownReferenceAnalysis &analysis,
    const std::shared_ptr<WorkspaceResourceReader> &root, const QString &documentPath, const QString &documentIdentity,
    const AuthorizedPreviewImages &authorizedImages, const PreviewCancellation &cancel, const std::shared_ptr<PreviewImageCache> &cache) {
    PreviewImages result;
    if (previewCancelled(cancel) || !analysis.success || !analysis.complete || analysis.documentId != snapshot.documentId
        || analysis.revision != snapshot.revision || analysis.sourceDigest != QCryptographicHash::hash(snapshot.utf8, QCryptographicHash::Sha256)) return result;
    result.analyzed = true;
    bool hasImages = false;
    for (const auto &reference : analysis.references) {
        if (reference.kind == MarkdownReference::Kind::Image) hasImages = true;
        else result.linkTargets.append(reference.target);
    }
    if (!hasImages) return result;
    QElapsedTimer timer; timer.start();
    const bool documentAuthorized = verifiedDocument(root, documentPath, documentIdentity);
    result.readMs += timer.nsecsElapsed() / 1e6;
    qsizetype inputBytes = 0, outputBytes = 0;
    qint64 pixels = 0;
    for (const auto &reference : analysis.references) {
        if (previewCancelled(cancel)) return {};
        if (reference.kind != MarkdownReference::Kind::Image) continue;
        const int index = int(result.states.size());
        char state = 5;
        if (!documentAuthorized) state = 5;
        else if (index >= MaxImages) state = 3;
        else {
            const auto target = resolveWorkspaceTarget(documentPath, reference.target);
            const auto authorized = authorizedImages.constFind(reference.target);
            if ((!target.success || target.hasFragment) && authorized == authorizedImages.cend()) state = 2;
            else {
                timer.restart();
                const auto image = authorized != authorizedImages.cend() && authorized->root
                    ? authorized->root->read(authorized->path, MaxImageSourceBytes) : root->read(target.path, MaxImageSourceBytes);
                const auto imageRoot = authorized != authorizedImages.cend() ? authorized->root : root;
                const auto imagePath = authorized != authorizedImages.cend() ? authorized->path : target.path;
                if (imageRoot && imageRoot->active() && (image.ok() || image.error == WorkspaceReadError::Missing)) {
                    const auto absolute = QDir(imageRoot->rootPath()).filePath(imagePath);
                    if (image.ok()) result.watchedPaths.append(absolute);
                    auto parent = QFileInfo(imagePath).path(); if (parent == ".") parent.clear();
                    if (imageRoot->listDirectory(parent, 1).ok()) result.watchedPaths.append(QFileInfo(absolute).absolutePath());
                }
                result.readMs += timer.nsecsElapsed() / 1e6;
                if (previewCancelled(cancel)) return {};
                if (!image.ok()) state = readFailure(image.error);
                else if (authorized != authorizedImages.cend() && (image.identity != authorized->identity || !authorized->root->active()
                    || QCryptographicHash::hash(image.bytes, QCryptographicHash::Sha256) != authorized->digest)) state = 5;
                else if (image.bytes.size() > MaxBatchBytes - inputBytes) state = 3;
                else {
                    inputBytes += image.bytes.size();
                    QByteArray png;
                    bool reused = false; timer.restart();
                    state = cache ? cache->decode(image.bytes, pixels, MaxBatchBytes - outputBytes, png, reused)
                                  : decodeImage(image.bytes, pixels, MaxBatchBytes - outputBytes, png);
                    result.decodeMs += timer.nsecsElapsed() / 1e6;
                    if (state == 1) { if (reused) ++result.reusedImages; else ++result.decodedImages; }
                    if (state == 1) { outputBytes += png.size(); result.png.insert(index, std::move(png)); }
                }
            }
        }
        result.states.append(state);
    }
    // Do not return content captured under an obsolete root or replaced document.
    timer.restart();
    if (documentAuthorized && !verifiedDocument(root, documentPath, documentIdentity)) {
        result.png.clear();
        result.states.fill(5);
        result.watchedPaths.clear();
    }
    result.readMs += timer.nsecsElapsed() / 1e6;
    for (auto it = authorizedImages.cbegin(); it != authorizedImages.cend(); ++it) if (!it->root || !it->root->active()) {
        result.png.clear(); result.states.fill(5); result.watchedPaths.clear(); break;
    }
    result.watchedPaths.removeDuplicates();
    return result;
}
}
