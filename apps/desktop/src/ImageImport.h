#pragma once
#include "WorkspaceResourceReader.h"
#include <QImage>

namespace xdnote {
struct ImageImportResult {
    WorkspaceWriteResult publication;
    QString message;
};
// Runs off the UI thread with only immutable inputs. A bitmap is used only
// when sourcePath is empty; source files are never rewritten or re-encoded.
ImageImportResult importWorkspaceImage(const std::shared_ptr<WorkspaceResourceReader> &root,
    const QString &documentPath, const QString &documentIdentity,
    const QString &sourcePath, const QImage &bitmap);
}
