#include "WorkspaceImagePreview.h"
#include "ImageAdmission.h"
#include "FileService.h"
#include "MarkdownReferences.h"
#include "WorkspacePath.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>

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
    PreviewImages result;
    const auto analysis = analyzeMarkdownReferences(snapshot);
    if (!analysis.success || !analysis.complete) return result;
    result.analyzed = true;
    bool hasImages = false;
    for (const auto &reference : analysis.references) {
        if (reference.kind == MarkdownReference::Kind::Image) hasImages = true;
        else result.linkTargets.append(reference.target);
    }
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
            const auto authorized = authorizedImages.constFind(reference.target);
            if ((!target.success || target.hasFragment) && authorized == authorizedImages.cend()) state = 2;
            else {
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
                if (!image.ok()) state = readFailure(image.error);
                else if (authorized != authorizedImages.cend() && (image.identity != authorized->identity || !authorized->root->active()
                    || QCryptographicHash::hash(image.bytes, QCryptographicHash::Sha256) != authorized->digest)) state = 5;
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
        result.watchedPaths.clear();
    }
    for (auto it = authorizedImages.cbegin(); it != authorizedImages.cend(); ++it) if (!it->root || !it->root->active()) {
        result.png.clear(); result.states.fill(5); result.watchedPaths.clear(); break;
    }
    result.watchedPaths.removeDuplicates();
    return result;
}
}
