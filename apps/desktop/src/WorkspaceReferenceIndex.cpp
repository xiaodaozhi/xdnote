#include "WorkspaceReferenceIndex.h"

#include <QCryptographicHash>
#include <QMap>
#include <utility>

namespace xdnote {
namespace {
struct SourceGroup {
    QList<const ReferenceIndexSource *> disks;
    QList<const ReferenceIndexSource *> sessions;
};
}

QList<IndexedReference> ReferenceIndexSnapshot::inbound(const QString &targetPath) const {
    QList<IndexedReference> matches;
    for (const auto &item : references)
        if (item.target.success && item.target.path == targetPath) matches.append(item);
    return matches;
}

QList<IndexedReference> ReferenceIndexSnapshot::outbound(const QString &sourcePath) const {
    QList<IndexedReference> matches;
    for (const auto &item : references)
        if (item.sourcePath == sourcePath) matches.append(item);
    return matches;
}

ReferenceIndexSnapshot buildWorkspaceReferenceIndex(const ReferenceIndexInput &input) {
    ReferenceIndexSnapshot result;
    result.rootId = input.rootId;
    result.rootGeneration = input.rootGeneration;
    result.scanGeneration = input.scanGeneration;
    result.gaps = input.enumerationGaps;
    if (input.rootId.isNull() || input.rootGeneration == 0 || input.scanGeneration == 0)
        result.gaps.append(QStringLiteral("工作区或索引代次无效"));
    if (!input.enumerationComplete)
        result.gaps.append(QStringLiteral("工作区文件枚举尚未完成"));

    QMap<QString, SourceGroup> groups;
    for (const auto &source : input.sources) {
        if (!resolveWorkspaceTarget(source.path, QStringLiteral("#")).success) {
            result.gaps.append(QStringLiteral("来源路径无效"));
            continue;
        }
        auto &group = groups[source.path];
        (source.origin == ReferenceIndexSource::Origin::Session ? group.sessions : group.disks).append(&source);
    }

    for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
        const auto &group = it.value();
        if (group.disks.size() > 1 || group.sessions.size() > 1) {
            result.gaps.append(QStringLiteral("同一来源出现多个磁盘快照或会话：%1").arg(it.key()));
            continue;
        }
        const auto *source = !group.sessions.isEmpty() ? group.sessions.first() : group.disks.first();
        if (source->identity.isEmpty() || source->pathGeneration == 0 || source->snapshot.documentId.isNull()) {
            result.gaps.append(QStringLiteral("来源身份或代次无效：%1").arg(it.key()));
            continue;
        }
        if (!group.sessions.isEmpty() && !group.disks.isEmpty()
            && (group.sessions.first()->identity != group.disks.first()->identity
                || group.sessions.first()->pathGeneration != group.disks.first()->pathGeneration)) {
            // Keep known references from the live text, but never count a
            // changed disk object as a completely scanned source.
            result.gaps.append(QStringLiteral("会话与磁盘身份或路径代次不一致：%1").arg(it.key()));
        }
        const auto analysis = analyzeMarkdownReferences(source->snapshot);
        if (!analysis.success) {
            result.gaps.append(QStringLiteral("无法分析来源：%1").arg(it.key()));
            continue;
        }
        result.sources.append({source->path, source->identity, source->pathGeneration,
                               analysis.documentId, analysis.revision, source->origin,
                               QCryptographicHash::hash(source->snapshot.utf8, QCryptographicHash::Sha256),
                               analysis.complete});
        if (!analysis.complete)
            result.gaps.append(QStringLiteral("来源分析不完整：%1").arg(it.key()));
        bool unresolved = false;
        for (const auto &reference : analysis.references) {
            auto target = resolveWorkspaceTarget(source->path, reference.target);
            if (!target.success) unresolved = true;
            result.references.append({source->path, source->identity, source->pathGeneration,
                                      analysis.documentId, analysis.revision, source->origin,
                                      reference, std::move(target)});
        }
        if (unresolved) result.gaps.append(QStringLiteral("引用目标未解析：%1").arg(it.key()));
    }
    result.complete = result.gaps.isEmpty();
    return result;
}
}
