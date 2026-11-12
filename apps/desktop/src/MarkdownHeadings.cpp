#include "MarkdownHeadings.h"
#include "MarkdownBridge.h"
#include "PreviewAnalysis.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QScopeGuard>
#include <QSet>
#include <cmath>

namespace xdnote {
namespace {
bool offset(const QJsonValue &value, const QByteArray &source, qsizetype &result) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble(-1);
    if (!std::isfinite(number) || number < 0 || number > source.size() || std::floor(number) != number) return false;
    result = qsizetype(number);
    return result == source.size() || (static_cast<unsigned char>(source.at(result)) & 0xc0) != 0x80;
}
}
MarkdownHeadingMap analyzeMarkdownHeadings(const DocumentSnapshot &snapshot) {
    MarkdownHeadingMap result; result.documentId = snapshot.documentId; result.revision = snapshot.revision;
    if (snapshot.result != EditResult::Applied) { result.reason = resultDescription(snapshot.result); return result; }
    if (auto shared = PreviewAnalysis::find(snapshot)) {
        const auto &model = shared->model; qsizetype budget = 0; QList<MarkdownHeading> headings;
        for (quint32 i = 0; i < model.nodeCount(); ++i) {
            const auto node = model.node(i); if (node.kind != NativePreviewModel::Kind::Heading) continue;
            QByteArray title;
            for (quint32 child = i + 1; child < node.subtreeEnd; ++child) {
                const auto part = model.node(child);
                if (part.kind == NativePreviewModel::Kind::SoftBreak || part.kind == NativePreviewModel::Kind::HardBreak) title += ' ';
                else if (part.kind == NativePreviewModel::Kind::Text || part.kind == NativePreviewModel::Kind::InlineCode || part.kind == NativePreviewModel::Kind::LiteralHtml) {
                    const auto text = model.text(part); title.append(text.data(), text.size());
                }
                if (title.size() > 8 * 1024 * 1024) { result.reason = QStringLiteral("标题结果过大"); return result; }
            }
            const auto id = model.text(node); budget += title.size() + id.size() + 64;
            if (budget > 8 * 1024 * 1024) { result.headings.clear(); result.reason = QStringLiteral("标题结果过大"); return result; }
            headings.append({{node.sourceStart, node.sourceEnd}, int(node.detail), QString::fromUtf8(title), QString::fromUtf8(id.data(), id.size())});
            i = node.subtreeEnd - 1;
        }
        result.headings = std::move(headings); result.success = true; return result;
    }
    auto buffer = xdnote_markdown_headings(reinterpret_cast<const unsigned char *>(snapshot.utf8.constData()), std::size_t(snapshot.utf8.size()));
    const auto release = qScopeGuard([&] { xdnote_markdown_free(&buffer); });
    if (buffer.status != 0 || !buffer.data || buffer.length > 8 * 1024 * 1024) { result.reason = QStringLiteral("无法分析标题：源文无效或结果过大"); return result; }
    QJsonParseError error{};
    const auto json = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char *>(buffer.data), qsizetype(buffer.length)), &error);
    const auto root = json.object();
    if (error.error != QJsonParseError::NoError || !json.isObject() || root.value("version").toInt(-1) != 1 || !root.value("headings").isArray()) {
        result.reason = QStringLiteral("无法读取标题分析结果"); return result;
    }
    QList<MarkdownHeading> headings; QSet<QString> used; qsizetype previousEnd = 0;
    for (const auto &value : root.value("headings").toArray()) {
        if (!value.isObject()) { result.reason = QStringLiteral("标题分析结果无效"); return result; }
        const auto entry = value.toObject(); MarkdownHeading heading;
        if (!offset(entry.value("start"), snapshot.utf8, heading.range.start) || !offset(entry.value("end"), snapshot.utf8, heading.range.end)
            || !heading.range.valid() || heading.range.start < previousEnd || !entry.value("level").isDouble()
            || !entry.value("title").isString() || !entry.value("id").isString()) {
            result.reason = QStringLiteral("标题范围或内容无效"); return result;
        }
        const auto level = entry.value("level").toDouble();
        if (level < 1 || level > 6 || std::floor(level) != level) { result.reason = QStringLiteral("标题级别无效"); return result; }
        heading.level = int(level); heading.title = entry.value("title").toString(); heading.id = entry.value("id").toString();
        if (heading.id.isEmpty() || used.contains(heading.id)) { result.reason = QStringLiteral("标题标识无效"); return result; }
        used.insert(heading.id); previousEnd = heading.range.end; headings.append(std::move(heading));
    }
    result.headings = std::move(headings); result.success = true; return result;
}
}
