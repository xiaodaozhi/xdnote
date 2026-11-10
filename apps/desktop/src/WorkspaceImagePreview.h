#pragma once

#include "DocumentSession.h"
#include "WorkspaceResourceReader.h"
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
};

PreviewImages preparePreviewImages(const DocumentSnapshot &snapshot,
    const std::shared_ptr<WorkspaceResourceReader> &root,
    const QString &documentPath, const QString &documentIdentity, const AuthorizedPreviewImages &authorizedImages = {});
}
