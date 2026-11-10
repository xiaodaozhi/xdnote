#pragma once
#include "WorkspaceReferenceIndex.h"
#include "WorkspaceResourceReader.h"
#include <atomic>
#include <memory>
#include <QHash>

namespace xdnote {
struct ReferenceScanLimits {
    int entries = 50000;
    int directories = 512;
    int documents = 1024;
    qsizetype sourceBytes = 32 * 1024 * 1024;
};
struct ReferenceTargetState {
    WorkspaceReadError error = WorkspaceReadError::Unsupported;
    QString identity;
};
struct ReferenceScanResult {
    ReferenceIndexSnapshot index;
    int visitedEntries = 0;
    int visitedDirectories = 0;
    int readDocuments = 0;
    qsizetype readBytes = 0;
    QStringList excludedDirectories;
    QStringList watchedDirectories, imageCandidates;
    QHash<QString, ReferenceTargetState> targets;
    bool cancelled = false;
};

// Blocking worker operation. Live snapshots must be captured by the caller on
// the session thread. The result remains derived data, never a write permit.
ReferenceScanResult scanWorkspaceReferences(const std::shared_ptr<WorkspaceResourceReader> &root,
    ReferenceIndexInput input, const ReferenceScanLimits &limits = {},
    const std::shared_ptr<std::atomic_bool> &cancelled = {});
}
