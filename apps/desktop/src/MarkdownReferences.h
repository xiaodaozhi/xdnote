#pragma once

#include "DocumentSession.h"
#include <QList>
#include <QString>

namespace xdnote {
struct MarkdownRange {
    qsizetype start = -1;
    qsizetype end = -1;
    bool valid() const { return start >= 0 && end > start; }
};

struct MarkdownReference {
    enum class Kind { Link, Image };
    Kind kind = Kind::Link;
    MarkdownRange usage;
    MarkdownRange definition;
    QString target;
    QString title;
    QString referenceLabel;
    bool referenceStyle = false;
    MarkdownRange destination;
};

struct MarkdownReferenceAnalysis {
    QUuid documentId;
    quint64 revision = 0;
    QByteArray sourceDigest;
    QList<MarkdownReference> references;
    bool success = false;
    bool complete = false;
    QString reason;
};

// Pure analysis of a captured snapshot; no filesystem access or editor state.
// The ranges cover complete Markdown constructs, not the destination alone.
// A caller must recheck document identity and revision before using results.
MarkdownReferenceAnalysis analyzeMarkdownReferences(const DocumentSnapshot &snapshot);
MarkdownReferenceAnalysis decodeMarkdownReferences(const DocumentSnapshot &snapshot, const QByteArray &json);
}
