#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <optional>

namespace xdnote {

// Read-only semantic data, not an editor buffer or a layout cache. Views returned
// by text()/auxiliary() live only as long as this immutable model's storage.
class NativePreviewModel final {
public:
    enum class Kind : quint32 {
        Paragraph = 1, Heading, BlockQuote, CodeBlock, LiteralBlock, BulletList,
        OrderedList, Item, Table, TableHead, TableRow, TableCell, Emphasis, Strong,
        Strikethrough, Link, Image, Text, InlineCode, LiteralHtml, SoftBreak,
        HardBreak, Rule, TaskMarker
    };
    static constexpr quint32 NoParent = 0xffffffffu;
    static constexpr qsizetype MaximumBytes = 64 * 1024 * 1024;
    struct Node {
        Kind kind{};
        quint32 parent = NoParent;
        quint32 subtreeEnd = 0;
        quint32 sourceStart = 0, sourceEnd = 0;
        quint32 textOffset = 0, textLength = 0;
        quint32 auxiliaryOffset = 0, auxiliaryLength = 0;
        quint32 detail = 0;
    };

    // Call on an owned, immutable session snapshot. The caller must additionally
    // bind completion to document/revision/path/resource generations; a digest
    // identifies bytes, not current session authority or resource permission.
    static std::optional<NativePreviewModel> parse(const QByteArray &source, QString *error = nullptr);
    static std::optional<NativePreviewModel> decode(QByteArray bytes, QByteArrayView source, QString *error = nullptr);

    quint32 nodeCount() const { return m_nodes; }
    quint32 sourceBytes() const { return m_sourceBytes; }
    bool referencesComplete() const { return m_referencesComplete; }
    qsizetype encodedBytes() const { return m_bytes.size(); }
    const QByteArray &sourceDigest() const { return m_sourceDigest; }
    Node node(quint32 index) const;
    QByteArrayView text(const Node &node) const;
    QByteArrayView auxiliary(const Node &node) const;

private:
    bool readNode(quint32 index, Node &node) const;
    QByteArray m_bytes, m_sourceDigest;
    qsizetype m_arena = 0, m_index = 0;
    quint32 m_recordBytes = 0;
    quint32 m_nodes = 0, m_sourceBytes = 0;
    bool m_referencesComplete = false;
};
}
