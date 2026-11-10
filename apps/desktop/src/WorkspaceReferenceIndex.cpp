#include "WorkspaceReferenceIndex.h"

#include <QCryptographicHash>
#include <QHash>
#include <QMap>
#include <QSet>
#include <utility>
#include <algorithm>

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
    QHash<QString, QString> identityPaths;
    QSet<QString> aliases, sessionIdentities;
    for (const auto &source : input.sources) {
        if (!resolveWorkspaceTarget(source.path, QStringLiteral("#")).success) {
            result.gaps.append(QStringLiteral("来源路径无效"));
            continue;
        }
        if (!source.identity.isEmpty()) {
            const auto previous = identityPaths.constFind(source.identity);
            if (previous != identityPaths.cend() && previous.value() != source.path) aliases.insert(source.identity);
            else identityPaths.insert(source.identity, source.path);
            if (source.origin == ReferenceIndexSource::Origin::Session) sessionIdentities.insert(source.identity);
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
        if (input.sessionsOutsideRoot.contains(source->identity)) {
            result.gaps.append(QStringLiteral("来源在工作区外的其他路径打开：%1").arg(it.key()));
            if (source->origin == ReferenceIndexSource::Origin::Disk) continue;
        }
        if (aliases.contains(source->identity)) {
            result.gaps.append(QStringLiteral("同一文件身份存在多个来源路径：%1").arg(it.key()));
            // An open session may contain unsaved text for every hard-link or
            // case alias of the same file. Do not report its disk alias as
            // current until the caller can map the live snapshot to each path.
            if (group.sessions.isEmpty() && sessionIdentities.contains(source->identity)) continue;
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
        QList<int> lineStarts{0};
        for (int offset = 0; offset < source->snapshot.utf8.size(); ++offset)
            if (source->snapshot.utf8.at(offset) == '\n') lineStarts.append(offset + 1);
        for (const auto &reference : analysis.references) {
            auto target = resolveWorkspaceTarget(source->path, reference.target);
            if (!target.success) unresolved = true;
            result.references.append({source->path, source->identity, source->pathGeneration,
                                      analysis.documentId, analysis.revision, source->origin,
                                      reference, std::move(target), int(std::upper_bound(lineStarts.cbegin(), lineStarts.cend(), reference.usage.start) - lineStarts.cbegin())});
        }
        if (unresolved) result.gaps.append(QStringLiteral("引用目标未解析：%1").arg(it.key()));
    }
    result.complete = result.gaps.isEmpty();
    return result;
}
}
