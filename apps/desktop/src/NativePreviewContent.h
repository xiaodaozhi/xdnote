#pragma once

#include "NativePreviewModel.h"
#include <QVector>
#include <QBitArray>
#include <QUuid>

namespace xdnote {
struct NativePreviewStamp {
    QUuid document;
    quint64 revision = 0, request = 0, path = 0, resources = 0;
    bool operator==(const NativePreviewStamp &other) const {
        return document == other.document && revision == other.revision && request == other.request
            && path == other.path && resources == other.resources;
    }
    bool operator!=(const NativePreviewStamp &other) const { return !(*this == other); }
};

// Derived readable text owns selection positions independently of layout caches.
// No source edits, session pointers, filesystem access or executable URLs.
struct NativePreviewContent {
    enum Style : quint32 { Bold = 1, Italic = 2, Strike = 4, Mono = 8, Task = 16 };
    enum class FlowKind { Text, CodeLine, TableRow, Rule };
    struct Run { int start = 0, length = 0; quint32 style = 0, link = NativePreviewModel::NoParent; };
    struct Flow {
        int start = 0, length = 0, owner = -1, firstCell = 0, cellCount = 0;
        quint32 node = 0;
        quint16 indent = 0, quote = 0;
        quint8 heading = 0;
        FlowKind kind = FlowKind::Text;
        bool header = false;
    };
    struct Cell { int start = 0, length = 0; quint32 node = 0; };
    struct Owner { quint32 node = 0; int start = 0, length = 0; bool code = false; };
    struct Image { int start = 0, length = 0; quint32 index = 0; char state = 0; };
    struct Action { quint32 node = 0; int start = 0, length = 0; bool copyCode = false; };
    enum class Syntax { Keyword, String, Number, Comment };
    struct Highlight { int start = 0, length = 0; Syntax kind{}; };
    QVector<Highlight> highlights;
    struct Heading { quint32 node = 0; int position = 0; };
    NativePreviewModel model;
    bool navigationAvailable = true;
    QString text;
    // Empty for ASCII; otherwise one immutable bit per global UTF-16 boundary.
    QBitArray graphemeBoundaries;
    QVector<Run> runs;
    QVector<Flow> flows;
    QVector<Cell> cells;
    QVector<Owner> owners;
    QVector<Image> images;
    QVector<Action> actions;
    QVector<Heading> headings;

    static std::optional<NativePreviewContent> build(NativePreviewModel model,
        const QByteArray &imageStates = {}, QString *error = nullptr);
    qsizetype estimatedBytes() const;
};
}
