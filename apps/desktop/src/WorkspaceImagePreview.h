#pragma once

#include "DocumentSession.h"
#include "WorkspaceResourceReader.h"
#include "PreviewAnalysis.h"
#include <QCache>
#include <QMutex>
#include <QHash>
#include <QStringList>
#include <memory>

namespace xdnote {
struct AuthorizedPreviewImage {
    std::shared_ptr<WorkspaceResourceReader> root;
    QString path, identity;
    QByteArray digest;
    bool operator==(const AuthorizedPreviewImage &other) const { return root == other.root && path == other.path && identity == other.identity && digest == other.digest; }
};
using AuthorizedPreviewImages = QHash<QString, AuthorizedPreviewImage>;
// A status is returned for every parsed image in source order. Only status 1
// has a corresponding application-produced static PNG. Source URLs and paths
// never become WebEngine URLs. This object owns no editor or GUI resources.
struct PreviewImages {
    QByteArray states;
    QHash<int, QByteArray> png;
    QStringList linkTargets;
    QStringList watchedPaths;
    bool analyzed = false;
    int decodedImages = 0, reusedImages = 0;
    double readMs = 0, decodeMs = 0;
};

class PreviewImageCache final {
public:
    char decode(const QByteArray &bytes, qint64 &pixels, qsizetype limit, QByteArray &png, bool &reused);
    void clear();
    qsizetype retainedBytes();
private:
    struct Image { QByteArray png; qint64 pixels = 0; };
    QMutex m_mutex;
    QCache<QByteArray, Image> m_images{8 * 1024 * 1024};
};

PreviewImages preparePreviewImages(const DocumentSnapshot &snapshot,
    const std::shared_ptr<WorkspaceResourceReader> &root,
    const QString &documentPath, const QString &documentIdentity, const AuthorizedPreviewImages &authorizedImages = {});
PreviewImages preparePreviewImages(const DocumentSnapshot &snapshot, const MarkdownReferenceAnalysis &analysis,
    const std::shared_ptr<WorkspaceResourceReader> &root,
    const QString &documentPath, const QString &documentIdentity, const AuthorizedPreviewImages &authorizedImages,
    const PreviewCancellation &cancel, const std::shared_ptr<PreviewImageCache> &cache);
}
