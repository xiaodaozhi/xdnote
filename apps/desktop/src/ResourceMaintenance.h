#pragma once
#include "ReferenceMovePlan.h"
#include "WorkspaceReferenceScan.h"
#include "FileService.h"

namespace xdnote {
struct ResourceMaintenancePreview {
    ReferenceMovePlan plan;
    ReferenceIndexInput live;
    ReferenceIndexSnapshot scanned;
    QHash<QString, QString> actualPaths;
    QString verifiedResourcePath;
    QHash<QString, std::shared_ptr<const FileBaseline>> baselines;
};
struct ResourceMaintenanceRow {
    enum Status { PendingSession, Saved, Skipped, Failed, Uncertain };
    QString from, to, message;
    Status status = Skipped;
    std::shared_ptr<const FileBaseline> saved;
};
struct ResourceMaintenanceResult {
    WorkspaceMoveResult move;
    QList<ResourceMaintenanceRow> rows;
    QString problem;
    QString movedIdentity;
    ReferenceIndexSnapshot afterSources;
};
// Bounded worker operations. No session access; live text is captured on GUI.
ResourceMaintenancePreview prepareResourceMove(const std::shared_ptr<WorkspaceResourceReader> &root,
    ReferenceIndexInput live, ReferenceMoveRequest request,
    const QHash<QString, std::shared_ptr<const FileBaseline>> &openBaselines = {},
    const std::shared_ptr<std::atomic_bool> &cancelled = {});
ResourceMaintenanceResult executeResourceMove(const std::shared_ptr<WorkspaceResourceReader> &root,
    const ResourceMaintenancePreview &preview, bool maintain,
    const std::shared_ptr<std::atomic_bool> &cancelled = {});
// Recovery edits only recorded successful rows, after checking their after
// baselines. It never guesses that a failed/uncertain write can be reversed.
ResourceMaintenancePreview reverseResourceMove(const ResourceMaintenancePreview &preview,
    const ResourceMaintenanceResult &result, const QString &currentIdentity);
}
