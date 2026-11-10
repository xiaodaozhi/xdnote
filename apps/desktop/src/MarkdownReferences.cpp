#include "MarkdownReferences.h"
#include "MarkdownBridge.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QScopeGuard>
#include <cmath>

namespace xdnote {
namespace {
bool boundary(const QByteArray &source, qsizetype offset) {
    return offset >= 0 && offset <= source.size()
        && (offset == source.size() || (static_cast<unsigned char>(source.at(offset)) & 0xc0) != 0x80);
}
bool position(const QJsonValue &value, qsizetype limit, qsizetype &result) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble(-1);
    if (!std::isfinite(number) || number < 0 || number > limit || std::floor(number) != number) return false;
    result = qsizetype(number);
    return true;
}
bool validRange(const QByteArray &source, const QJsonValue &start, const QJsonValue &end, MarkdownRange &range) {
    return position(start, source.size(), range.start) && position(end, source.size(), range.end)
        && range.valid() && boundary(source, range.start) && boundary(source, range.end);
}
}

MarkdownReferenceAnalysis analyzeMarkdownReferences(const DocumentSnapshot &snapshot) {
    MarkdownReferenceAnalysis result; result.documentId = snapshot.documentId; result.revision = snapshot.revision;
    if (snapshot.result != EditResult::Applied) { result.reason = resultDescription(snapshot.result); return result; }
    auto output = xdnote_markdown_references(reinterpret_cast<const unsigned char *>(snapshot.utf8.constData()), std::size_t(snapshot.utf8.size()));
    const auto release = qScopeGuard([&] { xdnote_markdown_free(&output); });
    if (output.status != 0 || !output.data || output.length > 8 * 1024 * 1024) {
        result.reason = QStringLiteral("无法分析引用：源文无效或分析结果过大"); return result;
    }
    QJsonParseError error{};
    const auto json = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char *>(output.data), qsizetype(output.length)), &error);
    const auto root = json.object();
    if (error.error != QJsonParseError::NoError || !json.isObject() || root.value("version").toInt() != 1
        || !root.value("references").isArray() || !root.value("complete").isBool()) {
        result.reason = QStringLiteral("无法读取引用分析结果"); return result;
    }
    QList<MarkdownReference> items;
    for (const auto &value : root.value("references").toArray()) {
        if (!value.isObject()) { result.reason = QStringLiteral("引用分析结果无效"); return result; }
        const auto entry = value.toObject(); MarkdownReference item;
        const auto kind = entry.value("kind").toString();
        if (kind == "link") item.kind = MarkdownReference::Kind::Link;
        else if (kind == "image") item.kind = MarkdownReference::Kind::Image;
        else { result.reason = QStringLiteral("引用类型无效"); return result; }
        if (!validRange(snapshot.utf8, entry.value("start"), entry.value("end"), item.usage)
            || !entry.value("target").isString() || !entry.value("title").isString()) {
            result.reason = QStringLiteral("引用范围或目标无效"); return result;
        }
        item.target = entry.value("target").toString(); item.title = entry.value("title").toString();
        const auto label = entry.value("reference"), definition = entry.value("definition");
        if (label.isString()) {
            item.referenceStyle = true; item.referenceLabel = label.toString();
            const auto span = definition.toArray();
            if (!definition.isArray() || span.size() != 2 || !validRange(snapshot.utf8, span.at(0), span.at(1), item.definition)) {
                result.reason = QStringLiteral("引用定义的位置无效"); return result;
            }
        } else if (!label.isNull() || !definition.isNull()) {
            result.reason = QStringLiteral("引用定义无效"); return result;
        }
        items.append(std::move(item));
    }
    result.references = std::move(items); result.complete = root.value("complete").toBool(); result.success = true;
    if (!result.complete) result.reason = QStringLiteral("引用过多，分析可能不完整");
    return result;
}
}
