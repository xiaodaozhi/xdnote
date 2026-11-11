#include "NativePreviewModel.h"
#include "MarkdownBridge.h"

#include <QCryptographicHash>
#include <QSet>
#include <QVarLengthArray>
#include <QtEndian>
#include <cstring>

namespace xdnote {
namespace {
constexpr qsizetype HeaderBytes = 32;
constexpr qsizetype MaximumSourceBytes = 2 * 1024 * 1024;
quint32 word(const char *bytes) { return qFromLittleEndian<quint32>(bytes); }
bool boundary(QByteArrayView bytes, quint32 at) {
    return at <= bytes.size() && (at == bytes.size() || (quint8(bytes[at]) & 0xc0) != 0x80);
}
bool container(NativePreviewModel::Kind kind) {
    return kind >= NativePreviewModel::Kind::Paragraph && kind <= NativePreviewModel::Kind::Image;
}
class Reader {
public:
    Reader(const char *begin, const char *end) : at(begin), end(end) {}
    bool variable(quint32 &value) {
        value = 0;
        for (unsigned shift = 0; shift <= 28; shift += 7) {
            if (at == end) return false;
            const auto byte = quint8(*at++);
            if (shift == 28 && byte > 15) return false;
            value |= quint32(byte & 127) << shift;
            if (byte < 128) return shift == 0 || byte != 0; // minimal encoding
        }
        return false;
    }
    bool fixed(quint32 &value) {
        if (end - at < 4) return false;
        value = word(at); at += 4; return true;
    }
    bool finished() const { return at == end; }
private:
    const char *at, *end;
};
}

NativePreviewModel::Node NativePreviewModel::node(quint32 index) const {
    Node value;
    const bool valid = readNode(index, value);
    Q_ASSERT(valid);
    return valid ? value : Node{};
}

bool NativePreviewModel::readNode(quint32 index, Node &value) const {
    if (index >= m_nodes) return false;
    const char *base = m_bytes.constData();
    const auto offset = word(base + m_index + qsizetype(index) * 4);
    const auto next = index + 1 == m_nodes ? m_recordBytes : word(base + m_index + qsizetype(index + 1) * 4);
    if ((index == 0 && offset != 0) || next <= offset || next > m_recordBytes || next - offset < 6) return false;
    const char *record = base + HeaderBytes + offset;
    value = {}; value.kind = Kind(quint8(record[0]));
    const auto flags = quint8(record[1]);
    if (flags > 7) return false;
    Reader reader(record + 2, base + HeaderBytes + next);
    quint32 delta = 0, length = 0;
    if (!reader.fixed(value.subtreeEnd) || !reader.variable(delta) || delta > index
        || !reader.variable(value.sourceStart) || !reader.variable(length)
        || quint64(value.sourceStart) + length > 0xffffffffu) return false;
    value.parent = delta == 0 ? NoParent : index - delta;
    value.sourceEnd = value.sourceStart + length;
    if (flags & 1) {
        if (value.kind == Kind::Heading) {
            if (!reader.fixed(value.textOffset) || !reader.fixed(value.textLength)) return false;
        } else if (!reader.variable(value.textOffset) || !reader.variable(value.textLength)) return false;
    }
    if ((flags & 2) && (!reader.variable(value.auxiliaryOffset) || !reader.variable(value.auxiliaryLength))) return false;
    if ((flags & 4) && (!reader.variable(value.detail) || value.detail == 0)) return false;
    return reader.finished();
}

QByteArrayView NativePreviewModel::text(const Node &value) const {
    if (quint64(value.textOffset) + value.textLength > quint64(m_bytes.size() - m_arena)) return {};
    return QByteArrayView(m_bytes).sliced(m_arena + value.textOffset, value.textLength);
}
QByteArrayView NativePreviewModel::auxiliary(const Node &value) const {
    if (quint64(value.auxiliaryOffset) + value.auxiliaryLength > quint64(m_bytes.size() - m_arena)) return {};
    return QByteArrayView(m_bytes).sliced(m_arena + value.auxiliaryOffset, value.auxiliaryLength);
}

std::optional<NativePreviewModel> NativePreviewModel::parse(const QByteArray &source, QString *error) {
    auto buffer = xdnote_markdown_native_model(reinterpret_cast<const unsigned char *>(source.constData()),
                                               std::size_t(source.size()));
    const quint32 status = buffer.status;
    if (status != 0) {
        xdnote_markdown_free(&buffer);
        if (error) *error = status == 1 ? QStringLiteral("预览原文不是有效的 UTF-8。")
            : status == 2 ? QStringLiteral("预览语义模型超过当前解析范围或资源预算。")
                          : QStringLiteral("预览语义解析失败。");
        return std::nullopt;
    }
    if (!buffer.data || buffer.length > std::size_t(MaximumBytes)) {
        xdnote_markdown_free(&buffer);
        if (error) *error = QStringLiteral("预览语义模型的版本、结构或原文范围无效。");
        return std::nullopt;
    }
    auto storage = std::shared_ptr<void>(buffer.data, [buffer](void *) mutable { xdnote_markdown_free(&buffer); });
    auto result = decode(QByteArray::fromRawData(reinterpret_cast<const char *>(buffer.data), qsizetype(buffer.length)), source, error);
    if (result) result->m_storage = std::move(storage);
    return result;
}

std::optional<NativePreviewModel> NativePreviewModel::decode(QByteArray bytes, QByteArrayView source, QString *error) {
    const auto fail = [error]() -> std::optional<NativePreviewModel> {
        if (error) *error = QStringLiteral("预览语义模型的版本、结构或原文范围无效。");
        return std::nullopt;
    };
    if (bytes.size() < HeaderBytes || bytes.size() > MaximumBytes || source.size() > MaximumSourceBytes
        || !source.isValidUtf8() || std::memcmp(bytes.constData(), "XDNOTE\0M", 8) != 0)
        return fail();
    const char *header = bytes.constData();
    const auto version = word(header + 8), sourceBytes = word(header + 12), count = word(header + 16);
    const auto arenaBytes = word(header + 20), flags = word(header + 24), recordBytes = word(header + 28);
    // Compute in 64 bits before any pointer arithmetic, including on 32-bit hosts.
    const quint64 indexAt = quint64(HeaderBytes) + recordBytes;
    const quint64 arenaAt = indexAt + quint64(count) * 4;
    if (version != 1 || sourceBytes != source.size() || flags > 1 || (count == 0 && recordBytes != 0)
        || arenaAt + arenaBytes != quint64(bytes.size())) return fail();

    NativePreviewModel model;
    model.m_bytes = std::move(bytes); model.m_nodes = count; model.m_sourceBytes = sourceBytes;
    model.m_arena = qsizetype(arenaAt); model.m_index = qsizetype(indexAt); model.m_recordBytes = recordBytes;
    model.m_referencesComplete = (flags & 1) != 0;
    const auto arena = QByteArrayView(model.m_bytes).sliced(model.m_arena);
    if (!arena.isValidUtf8()) return fail();
    QVarLengthArray<quint32, 64> stack;
    QSet<QByteArray> headings;
    quint32 links = 0, images = 0, codes = 0;
    for (quint32 index = 0; index < count; ++index) {
        while (!stack.isEmpty() && model.node(stack.last()).subtreeEnd == index) stack.removeLast();
        Node value;
        if (!model.readNode(index, value)) return fail();
        if (value.kind < Kind::Paragraph || value.kind > Kind::TaskMarker
            || value.parent != (stack.isEmpty() ? NoParent : stack.last())
            || value.subtreeEnd <= index || value.subtreeEnd > count
            || (!stack.isEmpty() && value.subtreeEnd > model.node(stack.last()).subtreeEnd)
            || (!container(value.kind) && value.subtreeEnd != index + 1)
            || value.sourceStart > value.sourceEnd || !boundary(source, value.sourceStart) || !boundary(source, value.sourceEnd)
            || quint64(value.textOffset) + value.textLength > arenaBytes
            || quint64(value.auxiliaryOffset) + value.auxiliaryLength > arenaBytes)
            return fail();
        const auto text = model.text(value), auxiliary = model.auxiliary(value);
        if (!text.isValidUtf8() || !auxiliary.isValidUtf8()) return fail();
        // Only links and images carry a second string. No field is an executable
        // URL: resource and navigation policies remain outside this data object.
        if (value.kind != Kind::Link && value.kind != Kind::Image && !auxiliary.isEmpty()) return fail();
        switch (value.kind) {
        case Kind::Heading:
            if (value.detail < 1 || value.detail > 6 || text.isEmpty() || headings.contains(text.toByteArray())) return fail();
            headings.insert(text.toByteArray()); break;
        case Kind::Link: if (value.detail != links++) return fail(); break;
        case Kind::Image: if (value.detail != images++) return fail(); break;
        case Kind::CodeBlock: if (value.detail != codes++) return fail(); break;
        case Kind::OrderedList: if (value.detail > 999999999u) return fail(); break;
        case Kind::Table:
            if (value.detail != text.size()) return fail();
            for (char alignment : text) if (alignment != 'N' && alignment != 'L' && alignment != 'C' && alignment != 'R') return fail();
            break;
        case Kind::TaskMarker: if (value.detail > 1 || !text.isEmpty()) return fail(); break;
        case Kind::Text: case Kind::InlineCode: case Kind::LiteralHtml:
            if (value.detail != 0) return fail(); break;
        default:
            if (value.detail != 0 || !text.isEmpty()) return fail(); break;
        }
        if (container(value.kind)) stack.append(index);
    }
    model.m_sourceDigest = QCryptographicHash::hash(source, QCryptographicHash::Sha256);
    if (error) error->clear();
    return model;
}
}
