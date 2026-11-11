#include "NativePreviewContent.h"
#include "Utf8Text.h"
#include <algorithm>
#include <limits>
#include <QTextBoundaryFinder>

namespace xdnote {
namespace {
using Kind = NativePreviewModel::Kind;
using Content = NativePreviewContent;
constexpr int MaximumText = 8 * 1024 * 1024;
struct Frame {
    quint32 index = 0, end = 0, style = 0, link = NativePreviewModel::NoParent;
    int owner = -1, previousOwner = -1, start = 0, firstCell = 0, action = -1, image = -1;
    int listNumber = -1;
    quint16 indent = 0, quote = 0;
    Kind kind{};
};
}

std::optional<NativePreviewContent> NativePreviewContent::build(NativePreviewModel model,
    const QByteArray &states, QString *error) {
    Content result; result.model = std::move(model);
    qsizetype expectedFlows = 0, expectedCells = 0;
    for (quint32 index = 0; index < result.model.nodeCount(); ++index) {
        const auto kind = result.model.node(index).kind;
        if (kind == Kind::Paragraph || kind == Kind::Heading || kind == Kind::LiteralBlock || kind == Kind::Item
            || kind == Kind::TableHead || kind == Kind::TableRow || kind == Kind::Rule) ++expectedFlows;
        if (kind == Kind::TableCell) ++expectedCells;
    }
    // Avoid repeatedly doubling large table/list vectors while the validated
    // model remains live. This reads metadata, not a second Markdown parse.
    result.flows.reserve(expectedFlows); result.cells.reserve(expectedCells);
    QVector<Frame> stack;
    QVector<int> lists;
    quint32 style = 0, link = NativePreviewModel::NoParent;
    quint16 indent = 0, quote = 0;
    int owner = -1, flow = -1, imageDepth = 0, codeDepth = 0, cellDepth = 0;
    QString pendingMarker;
    const auto append = [&](const QString &value, quint32 flags, quint32 target) {
        const int start = int(result.text.size());
        result.text += value;
        if (value.isEmpty()) return;
        if (!result.runs.isEmpty() && result.runs.last().start + result.runs.last().length == start
            && result.runs.last().style == flags && result.runs.last().link == target)
            result.runs.last().length += int(value.size());
        else result.runs.append({start, int(value.size()), flags, target});
    };
    const auto separator = [&](const QString &value) { append(value, 0, NativePreviewModel::NoParent); };
    const auto endFlow = [&] {
        if (flow < 0) return;
        result.flows[flow].length = int(result.text.size()) - result.flows[flow].start;
        if (result.flows[flow].length == 0) result.flows.removeLast();
        else separator(QStringLiteral("\n\n"));
        flow = -1;
    };
    const auto startFlow = [&](quint32 node, quint8 heading = 0) {
        endFlow();
        Flow value; value.start = int(result.text.size()); value.node = node;
        value.indent = indent; value.quote = quote; value.heading = heading;
        result.flows.append(value); flow = int(result.flows.size()) - 1;
        if (!pendingMarker.isEmpty()) { separator(pendingMarker); pendingMarker.clear(); }
    };
    const auto in = [&](Kind kind) { return kind == Kind::CodeBlock ? codeDepth > 0 : cellDepth > 0; };
    const auto close = [&]() {
        const auto entry = stack.takeLast();
        switch (entry.kind) {
        case Kind::Paragraph: case Kind::Heading: case Kind::LiteralBlock:
            if (!in(Kind::TableCell)) endFlow(); break;
        case Kind::Item: endFlow(); pendingMarker.clear(); break;
        case Kind::CodeBlock: {
            --codeDepth;
            auto &group = result.owners[entry.owner]; group.length = int(result.text.size()) - group.start;
            result.actions[entry.action].length = group.length;
            int start = group.start; const int end = group.start + group.length;
            while (start < end) {
                int stop = start;
                while (stop < end && result.text[stop] != '\n' && result.text[stop] != QChar(0x2028) && result.text[stop] != QChar(0x2029)) ++stop;
                Flow value; value.start = start; value.length = stop - start; value.node = entry.index;
                value.owner = entry.owner; value.kind = FlowKind::CodeLine; value.indent = entry.indent; value.quote = entry.quote;
                result.flows.append(value); start = stop + 1;
            }
            if (group.length == 0) {
                Flow value; value.start = group.start; value.node = entry.index;
                value.owner = entry.owner; value.kind = FlowKind::CodeLine; result.flows.append(value);
            }
            separator(QStringLiteral("\n")); break;
        }
        case Kind::TableCell:
            result.cells.append({entry.start, int(result.text.size()) - entry.start, entry.index}); --cellDepth; break;
        case Kind::TableHead: case Kind::TableRow: {
            Flow value; value.start = entry.start; value.length = int(result.text.size()) - entry.start;
            value.node = entry.index; value.owner = entry.owner; value.kind = FlowKind::TableRow;
            value.firstCell = entry.firstCell; value.cellCount = int(result.cells.size()) - entry.firstCell;
            value.indent = entry.indent; value.quote = entry.quote; value.header = entry.kind == Kind::TableHead;
            result.flows.append(value); separator(QStringLiteral("\n")); break;
        }
        case Kind::Table:
            result.owners[entry.owner].length = int(result.text.size()) - result.owners[entry.owner].start;
            separator(QStringLiteral("\n")); break;
        case Kind::Link:
            if (entry.action >= 0) result.actions[entry.action].length = int(result.text.size()) - entry.start;
            break;
        case Kind::Image:
            result.images[entry.image].length = int(result.text.size()) - entry.start; --imageDepth; break;
        case Kind::BulletList: case Kind::OrderedList: lists.removeLast(); break;
        default: break;
        }
        style = entry.style; link = entry.link; indent = entry.indent; quote = entry.quote; owner = entry.previousOwner;
    };

    for (quint32 index = 0; index <= result.model.nodeCount(); ++index) {
        while (!stack.isEmpty() && stack.last().end == index) close();
        if (index == result.model.nodeCount()) break;
        const auto node = result.model.node(index);
        const auto kind = node.kind;
        if (kind <= Kind::Image) {
            if (kind >= Kind::Emphasis && flow < 0 && !in(Kind::CodeBlock) && !in(Kind::TableCell)) startFlow(index);
            Frame entry; entry.index = index; entry.end = node.subtreeEnd; entry.kind = kind;
            entry.style = style; entry.link = link; entry.indent = indent; entry.quote = quote;
            entry.owner = owner; entry.previousOwner = owner; entry.start = int(result.text.size()); entry.firstCell = int(result.cells.size());
            switch (kind) {
            case Kind::Paragraph: case Kind::LiteralBlock:
                if (!in(Kind::TableCell)) startFlow(index); break;
            case Kind::Heading:
                startFlow(index, quint8(node.detail)); result.headings.append({index, int(result.text.size())}); break;
            case Kind::BlockQuote: endFlow(); quote = qMin(65535, int(quote) + 1); break;
            case Kind::BulletList: case Kind::OrderedList:
                endFlow(); indent = qMin(65535, int(indent) + 1); entry.listNumber = kind == Kind::OrderedList ? int(node.detail) : -1;
                lists.append(int(stack.size())); break;
            case Kind::Item:
                endFlow();
                if (!lists.isEmpty()) {
                    auto &list = stack[lists.last()];
                    pendingMarker = list.listNumber < 0 ? QStringLiteral("• ") : QString::number(list.listNumber++) + QStringLiteral(". ");
                }
                break;
            case Kind::CodeBlock: case Kind::Table:
                endFlow();
                if (!pendingMarker.isEmpty()) { startFlow(index); endFlow(); }
                owner = int(result.owners.size()); entry.owner = owner;
                result.owners.append({index, int(result.text.size()), 0, kind == Kind::CodeBlock});
                if (kind == Kind::CodeBlock) {
                    ++codeDepth;
                    entry.action = int(result.actions.size()); result.actions.append({index, int(result.text.size()), 0, true});
                }
                break;
            case Kind::TableHead: case Kind::TableRow:
                entry.start = int(result.text.size()); entry.firstCell = int(result.cells.size()); break;
            case Kind::TableCell:
                if (int(result.cells.size()) > stack.last().firstCell) separator(QStringLiteral("\t"));
                entry.start = int(result.text.size()); ++cellDepth; break;
            case Kind::Strong: style |= Bold; break;
            case Kind::Emphasis: style |= Italic; break;
            case Kind::Strikethrough: style |= Strike; break;
            case Kind::Link:
                if (imageDepth == 0) {
                    link = index; entry.action = int(result.actions.size());
                    result.actions.append({index, entry.start, 0, false});
                }
                break;
            case Kind::Image: {
                if (flow < 0 && !in(Kind::TableCell)) startFlow(index);
                entry.start = int(result.text.size()); entry.image = int(result.images.size());
                result.images.append({entry.start, 0, node.detail, node.detail < quint32(states.size()) ? states[int(node.detail)] : char(0)});
                const char state = result.images.last().state;
                separator(state == 2 ? QStringLiteral("图片（未获授权）：") : state == 3 ? QStringLiteral("图片（超出大小或数量限制）：")
                    : state == 4 ? QStringLiteral("图片（格式不支持或无法解码）：") : state == 5 ? QStringLiteral("图片（不存在或不可读）：") : QStringLiteral("图片："));
                ++imageDepth; break;
            }
            default: break;
            }
            stack.append(entry);
        } else {
            if (kind == Kind::Rule) {
                endFlow(); Flow value; value.start = int(result.text.size()); value.node = index;
                value.kind = FlowKind::Rule; value.indent = indent; value.quote = quote;
                result.flows.append(value); separator(QStringLiteral("\n"));
            } else {
                if (flow < 0 && !in(Kind::CodeBlock) && !in(Kind::TableCell)) startFlow(index);
                if (kind == Kind::TaskMarker) {
                    // Task items replace their ordinary bullet, not the text or
                    // authoritative Markdown marker in the document session.
                    if (flow >= 0 && result.text.mid(result.flows[flow].start) == QStringLiteral("• ")) {
                        result.text.chop(2);
                        if (!result.runs.isEmpty()) { result.runs.last().length -= 2; if (result.runs.last().length == 0) result.runs.removeLast(); }
                    }
                    append(node.detail ? QStringLiteral("☑") : QStringLiteral("☐"), style | Task, NativePreviewModel::NoParent);
                    separator(QStringLiteral(" "));
                } else if (kind == Kind::SoftBreak) append(QStringLiteral(" "), style, link);
                else if (kind == Kind::HardBreak) append(QStringLiteral("\n"), style, link);
                else append(decodeUtf8Literal(result.model.text(node)), style | (kind == Kind::InlineCode ? Mono : 0), link);
            }
        }
        if (result.text.size() > MaximumText) {
            if (error) *error = QStringLiteral("预览派生文本超过当前资源预算，原文保持不变。"); return std::nullopt;
        }
    }
    endFlow();
    // Highlight only derived code text, never reparse Markdown or touch source.
    // Keep the established 128 KiB decoration budget; longer code stays literal.
    const QStringList keywords = {"fn", "let", "mut", "pub", "use", "struct", "impl", "return", "if", "else", "for", "while", "const", "class", "function", "import", "from", "def", "true", "false", "null", "None", "int", "void", "auto", "async", "await"};
    for (const auto &owner : result.owners) {
        if (!owner.code || owner.length > 128 * 1024) continue;
        if (QStringView(result.text).mid(owner.start, owner.length).toUtf8().size() > 128 * 1024) continue;
        const auto language = QString::fromUtf8(result.model.text(result.model.node(owner.node))).toLower();
        if (!QStringList{"rust", "rs", "cpp", "c", "c++", "javascript", "js", "typescript", "ts", "python", "py", "json", "css", "sh", "bash"}.contains(language)) continue;
        const int end = owner.start + owner.length;
        for (int at = owner.start; at < end;) {
            const int start = at; const auto ch = result.text[at];
            Syntax kind{}; bool decorated = true;
            if (at + 1 < end && result.text.mid(at, 2) == "//") {
                while (at < end && result.text[at] != '\n') ++at;
                kind = Syntax::Comment;
            } else if (at + 1 < end && result.text.mid(at, 2) == "/*") {
                at += 2; while (at < end && !(at > start + 2 && result.text[at - 1] == '*' && result.text[at] == '/')) ++at;
                if (at < end) ++at; kind = Syntax::Comment;
            } else if (ch == '"' || ch == '\'' || ch == '`') {
                ++at; bool escape = false;
                while (at < end) { const auto next = result.text[at++]; if (!escape && next == ch) break; escape = !escape && next == '\\'; }
                kind = Syntax::String;
            } else if (ch >= '0' && ch <= '9') {
                ++at; while (at < end && (result.text[at].isLetterOrNumber() || result.text[at] == '.' || result.text[at] == '_')) ++at;
                kind = Syntax::Number;
            } else if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_') {
                ++at; while (at < end && (result.text[at].isLetterOrNumber() || result.text[at] == '_')) ++at;
                int next = at; while (next < end && result.text[next].isSpace()) ++next;
                decorated = keywords.contains(result.text.mid(start, at - start)) || (next < end && result.text[next] == ':');
                kind = Syntax::Keyword;
            } else { ++at; decorated = false; }
            if (decorated) result.highlights.append({start, at - start, kind});
        }
    }
    if (std::any_of(result.text.cbegin(), result.text.cend(), [](QChar ch) { return ch.unicode() >= 128; })) {
        QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, result.text);
        result.graphemeBoundaries.resize(result.text.size() + 1);
        result.graphemeBoundaries.setBit(0);
        for (int at = finder.toNextBoundary(); at >= 0; at = finder.toNextBoundary()) result.graphemeBoundaries.setBit(at);
    }
    std::stable_sort(result.actions.begin(), result.actions.end(), [](const Action &a, const Action &b) { return a.start < b.start; });
    if (error) error->clear();
    return result;
}

qsizetype NativePreviewContent::estimatedBytes() const {
    return model.encodedBytes() + (graphemeBoundaries.size() + 7) / 8 + text.capacity() * qsizetype(sizeof(QChar)) + runs.capacity() * qsizetype(sizeof(Run))
        + flows.capacity() * qsizetype(sizeof(Flow)) + cells.capacity() * qsizetype(sizeof(Cell))
        + owners.capacity() * qsizetype(sizeof(Owner)) + images.capacity() * qsizetype(sizeof(Image))
        + highlights.capacity() * qsizetype(sizeof(Highlight)) + actions.capacity() * qsizetype(sizeof(Action)) + headings.capacity() * qsizetype(sizeof(Heading));
}
}
