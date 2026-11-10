#include "ReferenceMovePlan.h"
#include "FileService.h"
#include <QCryptographicHash>
#include <QMap>
#include <QUrl>
#include <algorithm>

namespace xdnote {
namespace {
bool validPath(const QString &path) {
    if (path.isEmpty() || path.startsWith('/') || path.contains('\\')) return false;
    QStringList parts;
    for (const auto &part : path.split('/')) {
        if (part.isEmpty() || part == "." || part == "..") return false;
        parts.append(QString::fromLatin1(QUrl::toPercentEncoding(part)));
    }
    const auto resolved = resolveWorkspaceTarget("document.md", parts.join('/'));
    return resolved.success && resolved.path == path;
}
}
QString movedResourcePath(const QString &path, const ReferenceMoveRequest &request) {
    if (path == request.from) return request.to;
    if (request.directory && path.startsWith(request.from + '/')) return request.to + path.mid(request.from.size());
    return path;
}
ReferenceMovePlan planReferenceMove(const ReferenceMoveInput &input, const ReferenceMoveRequest &request) {
    ReferenceMovePlan result; result.request = request; result.rootId = input.index.rootId;
    result.rootGeneration = input.index.rootGeneration; result.scanGeneration = input.index.scanGeneration; result.gaps = input.index.gaps;
    if (result.rootId.isNull() || !result.rootGeneration || !result.scanGeneration || !validPath(request.from) || !validPath(request.to)
        || request.from == request.to || request.identity.isEmpty() || (request.directory && request.to.startsWith(request.from + '/'))) {
        result.problem = QStringLiteral("移动位置、身份或索引代次无效，不能预览。"); return result;
    }
    if (!input.index.complete && result.gaps.isEmpty()) result.gaps.append(QStringLiteral("扫描尚未完整覆盖工作区"));
    for (const auto &source : input.index.sources) {
        ReferenceMoveDocument document; document.source = source; document.before = input.texts.value(source.path);
        document.from = input.actualPaths.value(source.path, source.path);
        document.to = request.retargetOnly ? document.from : movedResourcePath(document.from, request);
        QMap<QPair<qsizetype, qsizetype>, ReferenceMoveEdit> edits;
        for (const auto &reference : input.index.outbound(source.path)) {
            if (!reference.target.success || reference.reference.target.startsWith('#')) continue;
            const auto oldTarget = input.actualPaths.value(reference.target.path, reference.target.path);
            const auto newTarget = movedResourcePath(oldTarget, request);
            if (document.from == document.to && oldTarget == newTarget) continue;
            const auto relative = relativeWorkspaceUrl(document.to, newTarget);
            if (!relative.success || !reference.reference.destination.valid()) {
                document.problem = QStringLiteral("部分目标没有可安全改写的语法范围，保留原文并需手动处理。"); continue;
            }
            auto target = relative.path;
            if (reference.target.hasFragment) target += '#' + QString::fromLatin1(QUrl::toPercentEncoding(reference.target.fragment));
            const auto range = reference.reference.destination;
            const auto key = qMakePair(range.start, range.end);
            const auto found = edits.constFind(key);
            if (found != edits.cend() && found->replacement != target.toUtf8()) {
                document.problem = QStringLiteral("共享引用定义产生不同改写目标，未自动修改。"); continue;
            }
            edits.insert(key, {range, target.toUtf8(), reference.reference.target, target});
        }
        if (edits.isEmpty() && document.from == document.to && document.problem.isEmpty()) continue;
        if (document.before.result != EditResult::Applied || document.before.documentId != source.documentId
            || document.before.revision != source.revision || QCryptographicHash::hash(document.before.utf8, QCryptographicHash::Sha256) != source.textSha256
            || !source.analysisComplete) document.problem = QStringLiteral("来源快照过期、缺失或分析不完整，不能自动改写。");
        document.after = document.before.utf8;
        qsizetype previousEnd = -1;
        for (const auto &edit : edits) {
            if (edit.range.start < previousEnd || edit.range.start < 0 || edit.range.end > document.before.utf8.size()) {
                document.problem = QStringLiteral("引用目标范围重叠或已失效，未自动修改。"); break;
            }
            previousEnd = edit.range.end; document.edits.append(edit);
        }
        if (document.problem.isEmpty()) {
            for (auto it = document.edits.crbegin(); it != document.edits.crend(); ++it)
                document.after.replace(it->range.start, it->range.end - it->range.start, it->replacement);
            if (document.after.size() > FileService::SourceLimit) document.problem = QStringLiteral("改写后的文档超过大小上限，未自动修改。");
        }
        if (!document.problem.isEmpty()) { document.after = document.before.utf8; result.gaps.append(source.path + QStringLiteral("：") + document.problem); }
        result.documents.append(std::move(document));
    }
    return result;
}
}
