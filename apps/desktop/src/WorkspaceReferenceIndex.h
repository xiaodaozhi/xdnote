#pragma once

#include "MarkdownReferences.h"
#include "WorkspacePath.h"
#include <QList>
#include <QStringList>
#include <QUuid>
#include <QSet>

namespace xdnote {

// Captured by the caller, never read from a path by the index. A session
// snapshot is authoritative over a disk snapshot for the same source path.
struct ReferenceIndexSource {
    enum class Origin { Disk, Session };
    QString path;
    QString identity;
    quint64 pathGeneration = 0;
    DocumentSnapshot snapshot;
    Origin origin = Origin::Disk;
};

struct ReferenceIndexInput {
    QUuid rootId;
    quint64 rootGeneration = 0;
    quint64 scanGeneration = 0;
    bool enumerationComplete = false;
    QStringList enumerationGaps;
    QList<ReferenceIndexSource> sources;
    QSet<QString> sessionsOutsideRoot;
};

struct IndexedReference {
    QString sourcePath;
    QString sourceIdentity;
    quint64 sourcePathGeneration = 0;
    QUuid documentId;
    quint64 revision = 0;
    ReferenceIndexSource::Origin origin = ReferenceIndexSource::Origin::Disk;
    MarkdownReference reference;
    // This is a lexical resolution only, not evidence of existence, identity,
    // authorization, or a safe edit location.
    WorkspacePathResult target;
    int sourceLine = 1;
};

// Present even when a successfully analyzed document has no references. The
// digest describes captured text, not a verified current filesystem object.
struct IndexedSource {
    QString path;
    QString identity;
    quint64 pathGeneration = 0;
    QUuid documentId;
    quint64 revision = 0;
    ReferenceIndexSource::Origin origin = ReferenceIndexSource::Origin::Disk;
    QByteArray textSha256;
    bool analysisComplete = false;
};

struct ReferenceIndexSnapshot {
    QUuid rootId;
    quint64 rootGeneration = 0;
    quint64 scanGeneration = 0;
    bool complete = false;
    QStringList gaps;
    QList<IndexedSource> sources;
    QList<IndexedReference> references;

    // Exact root-relative lexical matches only. An empty result must never be
    // treated as proof that a resource has no references.
    QList<IndexedReference> inbound(const QString &targetPath) const;
    QList<IndexedReference> outbound(const QString &sourcePath) const;
};

// Pure, bounded-by-input analysis. The caller is responsible for bounded
// enumeration, capturing disk and session snapshots on the correct threads,
// and rechecking all root/path/document generations before consuming results.
// This function does not scan, open, mutate, or authorize filesystem objects.
ReferenceIndexSnapshot buildWorkspaceReferenceIndex(const ReferenceIndexInput &input);
}
