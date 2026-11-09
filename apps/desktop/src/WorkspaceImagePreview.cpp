#include "WorkspaceImagePreview.h"
#include "ImageAdmission.h"
#include "FileService.h"
#include "MarkdownReferences.h"
#include "WorkspacePath.h"

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
    const auto file = root->read(path, FileService::SourceLimit);
    return file.ok() && file.identity == identity;
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
                const auto image = root->read(target.path, MaxImageSourceBytes);
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
