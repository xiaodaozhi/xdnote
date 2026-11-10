#include "ResourceMaintenance.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <algorithm>

namespace xdnote {
namespace {
// Resolve actual spelling by enumerated directory entries. File identities
// alone cannot distinguish two hard links, and a process-wide case-folding
// policy would incorrectly merge distinct entries on case-sensitive volumes.
QString actualPath(const std::shared_ptr<WorkspaceResourceReader> &root, const QString &path,
    QHash<QString, WorkspaceDirectoryResult> &cache) {
    QString prefix;
    for (const auto &part : path.split('/')) {
        if (!cache.contains(prefix)) cache.insert(prefix, root->listDirectory(prefix));
        const auto &listing = cache[prefix];
        if (!listing.ok() || !listing.complete) return {};
        QString chosen;
        for (const auto &entry : listing.entries) if (entry.name == part) { chosen = part; break; }
        if (chosen.isEmpty()) {
            const auto lookup = root->entry(prefix.isEmpty() ? part : prefix + '/' + part);
            if (!lookup.ok()) return {};
            for (const auto &entry : listing.entries) {
                if (entry.name.normalized(QString::NormalizationForm_D).toCaseFolded() != part.normalized(QString::NormalizationForm_D).toCaseFolded()) continue;
                const auto candidate = root->entry(prefix.isEmpty() ? entry.name : prefix + '/' + entry.name);
                if (!candidate.ok() || candidate.identity != lookup.identity) continue;
                if (!chosen.isEmpty()) return {};
                chosen = entry.name;
            }
        }
        if (chosen.isEmpty()) return {};
        prefix = prefix.isEmpty() ? chosen : prefix + '/' + chosen;
    }
    return prefix;
}
bool sameSources(const ReferenceIndexSnapshot &a, const ReferenceIndexSnapshot &b) {
    if (a.sources.size() != b.sources.size()) return false;
    for (const auto &source : a.sources) {
        const auto found = std::find_if(b.sources.cbegin(), b.sources.cend(), [&](const auto &candidate) { return candidate.path == source.path; });
        if (found == b.sources.cend() || found->identity != source.identity || found->origin != source.origin
            || found->textSha256 != source.textSha256 || found->analysisComplete != source.analysisComplete) return false;
    }
    return true;
}
}
ResourceMaintenancePreview prepareResourceMove(const std::shared_ptr<WorkspaceResourceReader> &root,
    ReferenceIndexInput live, ReferenceMoveRequest request,
    const QHash<QString, std::shared_ptr<const FileBaseline>> &openBaselines,
    const std::shared_ptr<std::atomic_bool> &cancelled) {
    ResourceMaintenancePreview result; result.live = live;
    if (!root || !root->active()) { result.plan.problem = QStringLiteral("工作区授权已失效。"); return result; }
    const auto stopped = [&] { return cancelled && cancelled->load(std::memory_order_acquire); };
    const auto scan = scanWorkspaceReferences(root, live, {}, cancelled);
    if (scan.cancelled || stopped()) { result.plan.problem = QStringLiteral("已取消影响预览。"); return result; }
    result.scanned = scan.index;
    ReferenceMoveInput input; input.index = scan.index; input.texts = scan.sourceTexts;
    QHash<QString, WorkspaceDirectoryResult> directories;
    const auto from = request.retargetOnly ? request.from : actualPath(root, request.from, directories);
    const auto actualTarget = request.retargetOnly ? actualPath(root, request.to, directories) : from;
    const auto entry = root->entry(actualTarget);
    if (from.isEmpty() || !entry.ok() || (!request.identity.isEmpty() && entry.identity != request.identity)) {
        result.plan.problem = QStringLiteral("原位置缺失、受阻或身份已经改变。"); return result;
    }
    request.from = from; request.identity = entry.identity; request.directory = entry.directory;
    if (request.retargetOnly) {
        if (root->entry(from).ok()) { result.plan.problem = QStringLiteral("原位置仍存在，请核对实际移动情况，不自动替换指向现有文件的引用。"); return result; }
        request.to = actualTarget; result.verifiedResourcePath = actualTarget;
        input.index.gaps.append(QStringLiteral("仅按已确认的新目标修复旧路径引用；外部移动文档自身的相对路径仍须检查，不猜测历史位置。"));
    }
    // Canonicalize the destination parent, never the requested new leaf name.
    const auto slash = request.to.lastIndexOf('/');
    if (slash >= 0) {
        const auto parent = actualPath(root, request.to.left(slash), directories);
        if (parent.isEmpty()) { result.plan.problem = QStringLiteral("目标父目录不存在或无法安全核验，请先选择已有目录。"); return result; }
        request.to = parent + request.to.mid(slash);
    }
    for (const auto &source : input.index.sources) {
        if (stopped()) { result.plan.problem = QStringLiteral("已取消影响预览。"); return result; }
        const auto actual = actualPath(root, source.path, directories);
        if (actual.isEmpty()) input.index.gaps.append(source.path + QStringLiteral("：无法核验实际路径"));
        else input.actualPaths.insert(source.path, actual);
    }
    for (auto it = scan.targets.cbegin(); it != scan.targets.cend(); ++it) {
        if (stopped()) { result.plan.problem = QStringLiteral("已取消影响预览。"); return result; }
        if (it->error != WorkspaceReadError::None) continue;
        const auto actual = actualPath(root, it.key(), directories);
        if (!actual.isEmpty()) input.actualPaths.insert(it.key(), actual);
        else input.index.gaps.append(it.key() + QStringLiteral("：目标路径存在歧义"));
    }
    result.plan = planReferenceMove(input, request);
    result.actualPaths = input.actualPaths;
    if (!result.plan.valid()) return result;
    for (auto &document : result.plan.documents) {
        if (stopped()) { result.plan.problem = QStringLiteral("已取消影响预览。"); return result; }
        auto baseline = openBaselines.value(document.source.path);
        if (!baseline) {
            const auto probe = FileService::probeWithin(root, document.from);
            const auto read = probe.file ? FileService::read({probe.file}) : ReadResult{};
            baseline = read.baseline;
        }
        if (!baseline || baseline->file()->identity() != document.source.identity
            || (document.source.origin == ReferenceIndexSource::Origin::Disk && baseline->utf8() != document.before.utf8))
            document.problem = QStringLiteral("磁盘基线已经改变或无法读取，不能写入。");
        if (baseline) result.baselines.insert(document.from, baseline);
        if (!document.problem.isEmpty()) result.plan.gaps.append(document.from + QStringLiteral("：") + document.problem);
    }
    return result;
}
ResourceMaintenanceResult executeResourceMove(const std::shared_ptr<WorkspaceResourceReader> &root,
    const ResourceMaintenancePreview &preview, bool maintain, const std::shared_ptr<std::atomic_bool> &cancelled) {
    ResourceMaintenanceResult result;
    const auto stopped = [&] { return cancelled && cancelled->load(std::memory_order_acquire); };
    if (!root || !root->active() || !preview.plan.valid() || stopped()) { result.problem = QStringLiteral("操作已取消或计划无效。"); return result; }
    // Recovery carries no fresh-scan promise. Its rows are individually bound
    // to the exact successful after-state instead.
    if (!preview.scanned.rootId.isNull()) {
        const auto fresh = scanWorkspaceReferences(root, preview.live, {}, cancelled);
        if (fresh.cancelled || !sameSources(preview.scanned, fresh.index)) { result.problem = QStringLiteral("预览后来源文档已变化，请重新预览。"); return result; }
    }
    for (const auto &document : preview.plan.documents) {
        if (!document.problem.isEmpty()) continue;
        const auto baseline = preview.baselines.value(document.from);
        const auto current = root->read(document.from, FileService::DiskByteLimit);
        if (!baseline || !current.ok() || current.identity != baseline->file()->identity()
            || QCryptographicHash::hash(current.bytes, QCryptographicHash::Sha256) != baseline->sha256()) {
            result.problem = QStringLiteral("%1 的磁盘内容已经改变，尚未移动任何文件。").arg(document.from); return result;
        }
    }
    if (stopped()) { result.problem = QStringLiteral("操作已取消。"); return result; }
    const auto &move = preview.plan.request;
    if (move.retargetOnly) {
        const auto entry = root->entry(preview.verifiedResourcePath);
        if (!entry.ok() || entry.identity != move.identity) { result.problem = QStringLiteral("确认的新目标已变化，未修改引用。"); return result; }
        result.move = {WorkspaceMoveStatus::Moved, WorkspaceReadError::None, 0, preview.verifiedResourcePath, {}, entry.lease};
    } else result.move = root->moveEntry(move.from, move.to, move.identity, move.directory);
    if (result.move.status != WorkspaceMoveStatus::Moved) { result.problem = QStringLiteral("移动未完成或结果不确定（错误 %1，系统代码 %2），未改写正文。请检查原位置、目标位置和恢复位置。").arg(int(result.move.error)).arg(result.move.nativeCode); return result; }
    for (const auto &document : preview.plan.documents) {
        ResourceMaintenanceRow row; row.from = document.from; row.to = document.to;
        if (!maintain || document.before.utf8 == document.after || !document.problem.isEmpty() || stopped()) {
            row.message = !document.problem.isEmpty() ? document.problem : stopped() ? QStringLiteral("已取消后续改写") : QStringLiteral("正文保持原样");
        } else if (document.source.origin == ReferenceIndexSource::Origin::Session) {
            row.status = ResourceMaintenanceRow::PendingSession; row.message = QStringLiteral("通过打开会话更新，仍需保存");
        } else {
            FileSaveRequest save; save.path = QDir(root->rootPath()).filePath(document.to);
            save.baseline = FileService::relocatedBaseline(preview.baselines.value(document.from), save.path);
            save.utf8 = document.after; save.bom = save.baseline && save.baseline->hasUtf8Bom();
            save.documentId = document.before.documentId; save.revision = document.before.revision;
            const auto saved = root->saveDocument(document.to, save);
            row.saved = saved.baseline;
            row.status = saved.status == SaveStatus::Committed ? ResourceMaintenanceRow::Saved
                : saved.status == SaveStatus::Uncertain ? ResourceMaintenanceRow::Uncertain : ResourceMaintenanceRow::Failed;
            row.message = saved.status == SaveStatus::Committed ? QStringLiteral("已写入文件") : saved.problem.description();
            if (!saved.recoveryPath.isEmpty()) row.message += QStringLiteral("；恢复文件：") + saved.recoveryPath;
            if (!saved.stagedPath.isEmpty()) row.message += QStringLiteral("；暂存文件：") + saved.stagedPath;
        }
        result.rows.append(std::move(row));
    }
    auto expected = move.identity;
    for (const auto &row : result.rows) if (row.to == move.to && row.saved) expected = row.saved->file()->identity();
    const auto moved = root->entry(move.retargetOnly ? preview.verifiedResourcePath : move.to);
    if (moved.ok() && moved.identity == expected) { result.movedIdentity = expected; result.move.lease = moved.lease; }
    else result.problem = QStringLiteral("移动及部分改写已执行，但最终资源身份无法确认；保留当前内容，请检查结果。");
    result.afterSources = preview.scanned;
    for (auto &source : result.afterSources.sources) {
        const auto actual = preview.actualPaths.value(source.path, source.path);
        source.path = move.retargetOnly ? actual : movedResourcePath(actual, move);
        for (const auto &row : result.rows) {
            if (row.from != actual || (row.status != ResourceMaintenanceRow::Saved && row.status != ResourceMaintenanceRow::PendingSession)) continue;
            const auto document = std::find_if(preview.plan.documents.cbegin(), preview.plan.documents.cend(), [&](const auto &item) { return item.from == row.from; });
            if (document != preview.plan.documents.cend()) source.textSha256 = QCryptographicHash::hash(document->after, QCryptographicHash::Sha256);
            if (row.saved) source.identity = row.saved->file()->identity();
        }
    }
    return result;
}
ResourceMaintenancePreview reverseResourceMove(const ResourceMaintenancePreview &preview,
    const ResourceMaintenanceResult &result, const QString &currentIdentity) {
    ResourceMaintenancePreview reverse;
    reverse.verifiedResourcePath = preview.verifiedResourcePath;
    reverse.scanned = result.afterSources;
    reverse.live = preview.live;
    reverse.live.sources.clear();
    reverse.plan = preview.plan; auto &request = reverse.plan.request;
    std::swap(request.from, request.to); request.identity = currentIdentity;
    reverse.plan.documents.clear(); reverse.plan.gaps.clear();
    for (const auto &row : result.rows) {
        if (row.status != ResourceMaintenanceRow::Saved && row.status != ResourceMaintenanceRow::PendingSession) continue;
        const auto found = std::find_if(preview.plan.documents.cbegin(), preview.plan.documents.cend(), [&](const auto &doc) { return doc.from == row.from; });
        if (found == preview.plan.documents.cend()) continue;
        auto document = *found; std::swap(document.from, document.to);
        document.before.utf8 = found->after; document.after = found->before.utf8;
        document.edits.clear();
        qsizetype delta = 0;
        for (const auto &edit : found->edits) {
            const auto start = edit.range.start + delta;
            document.edits.append({{start, start + edit.replacement.size()}, found->before.utf8.mid(edit.range.start, edit.range.end - edit.range.start), edit.newTarget, edit.originalTarget});
            delta += edit.replacement.size() - (edit.range.end - edit.range.start);
        }
        reverse.plan.documents.append(document);
        reverse.baselines.insert(document.from, row.saved ? row.saved : preview.baselines.value(row.from));
    }
    return reverse;
}
}
