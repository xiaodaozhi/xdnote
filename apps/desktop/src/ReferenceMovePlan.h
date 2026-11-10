#pragma once
#include "WorkspaceReferenceIndex.h"
#include <QHash>

namespace xdnote {
struct ReferenceMoveRequest {
    QString from, to, identity;
    bool directory = false;
    bool retargetOnly = false;
};
struct ReferenceMoveEdit {
    MarkdownRange range;
    QByteArray replacement;
    QString originalTarget, newTarget;
};
struct ReferenceMoveDocument {
    IndexedSource source;
    QString from, to;
    DocumentSnapshot before;
    QByteArray after;
    QList<ReferenceMoveEdit> edits;
    QString problem;
};
struct ReferenceMoveInput {
    ReferenceIndexSnapshot index;
    QHash<QString, DocumentSnapshot> texts;
    // Obtained from actual objects, never platform-wide case folding.
    QHash<QString, QString> actualPaths;
};
struct ReferenceMovePlan {
    ReferenceMoveRequest request;
    QUuid rootId;
    quint64 rootGeneration = 0, scanGeneration = 0;
    QList<ReferenceMoveDocument> documents;
    QStringList gaps;
    QString problem;
    bool valid() const { return problem.isEmpty(); }
};
// Pure preview. It neither grants filesystem authority nor executes edits.
ReferenceMovePlan planReferenceMove(const ReferenceMoveInput &input, const ReferenceMoveRequest &request);
QString movedResourcePath(const QString &path, const ReferenceMoveRequest &request);
}
