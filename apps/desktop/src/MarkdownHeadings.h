#pragma once
#include "DocumentSession.h"
#include "MarkdownReferences.h"

namespace xdnote {
struct MarkdownHeading {
    MarkdownRange range;
    int level = 0;
    QString title;
    QString id;
};
struct MarkdownHeadingMap {
    QUuid documentId;
    quint64 revision = 0;
    QList<MarkdownHeading> headings;
    bool success = false;
    QString reason;
};
// Uses the same versioned mapping as preview. The caller must recheck document
// identity and revision before locating or constructing a link to a heading.
MarkdownHeadingMap analyzeMarkdownHeadings(const DocumentSnapshot &snapshot);
}
