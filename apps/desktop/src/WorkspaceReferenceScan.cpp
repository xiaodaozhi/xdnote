#include "WorkspaceReferenceScan.h"
#include "FileService.h"
#include "Utf8Text.h"
#include "ImageAdmission.h"
#include "MarkdownHeadings.h"
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <utility>
#include <algorithm>

namespace xdnote {
ReferenceScanResult scanWorkspaceReferences(const std::shared_ptr<WorkspaceResourceReader> &root,
    ReferenceIndexInput input, const ReferenceScanLimits &limits, const std::shared_ptr<std::atomic_bool> &cancelled) {
    ReferenceScanResult result;
    input.enumerationComplete = false;
    const auto stopped = [&] { return cancelled && cancelled->load(std::memory_order_acquire); };
    const auto finish = [&]() {
        if (stopped()) { result.cancelled = true; input.sources.clear(); input.enumerationGaps.append(QStringLiteral("扫描已取消")); input.enumerationComplete = false; }
        if (!root || !root->active()) { input.sources.clear(); input.enumerationGaps.append(QStringLiteral("工作区授权已失效")); input.enumerationComplete = false; }
        result.index = buildWorkspaceReferenceIndex(input);
        for (const auto &source : result.index.sources)
            for (const auto &captured : input.sources)
                if (captured.path == source.path && captured.snapshot.documentId == source.documentId
                    && captured.snapshot.revision == source.revision) { result.sourceTexts.insert(source.path, captured.snapshot); break; }
        if (root && root->active() && !stopped()) {
            int images = 0; qsizetype imageBytes = 0; qint64 pixels = 0;
            for (const auto &reference : result.index.references) {
                if (stopped()) break;
                if (!reference.target.success) continue;
                const bool imageReference = reference.reference.kind == MarkdownReference::Kind::Image;
                if (result.targets.contains(reference.target.path) && (!imageReference || result.targets.value(reference.target.path).imageChecked)) continue;
                if (result.targets.size() >= 4096) { result.index.complete = false; result.index.gaps.append(QStringLiteral("目标检查达到上限")); break; }
                const auto target = root->inspect(reference.target.path);
                result.targets.insert(reference.target.path, {target.error, target.identity});
                if (imageReference) {
                    auto &state = result.targets[reference.target.path]; state.imageChecked = true;
                    if (!target.ok()) continue;
                    if (++images > 32 || imageBytes >= 32 * 1024 * 1024) {
                        state.diagnostic = QStringLiteral("图片诊断达到本轮数量或字节上限"); result.index.complete = false;
                        result.index.gaps.append(state.diagnostic); continue;
                    }
                    const auto read = root->read(reference.target.path, qMin<qsizetype>(MaxImageSourceBytes, 32 * 1024 * 1024 - imageBytes));
                    if (!read.ok() || read.identity != target.identity) {
                        state.error = read.ok() ? WorkspaceReadError::Changed : read.error;
                        state.diagnostic = QStringLiteral("图片无法读取或对象已变化"); continue;
                    }
                    imageBytes += read.bytes.size(); QByteArray png;
                    const auto status = decodeImage(read.bytes, pixels, 32 * 1024 * 1024, png);
                    if (status != 1) state.diagnostic = status == 3 ? QStringLiteral("图片超过字节、像素或解码限制") : QStringLiteral("图片损坏、格式不支持或包含未准入活动内容");
                }
            }
        }
        if (root && root->active() && !stopped()) {
            QHash<QString, MarkdownHeadingMap> maps;
            for (const auto &reference : result.index.references) {
                if (stopped()) break;
                if (!reference.target.success || !reference.target.hasFragment || reference.target.fragment.isEmpty()
                    || reference.reference.kind != MarkdownReference::Kind::Link) continue;
                const auto state = result.targets.value(reference.target.path);
                if (state.error != WorkspaceReadError::None) continue;
                const auto key = referenceDiagnosticKey(reference);
                if (!maps.contains(state.identity)) {
                    if (maps.size() >= 1024) { result.fragmentProblems.insert(key, QStringLiteral("标题诊断达到本轮上限")); continue; }
                    DocumentSnapshot text;
                    for (const auto &source : result.index.sources) if (source.identity == state.identity) { text = result.sourceTexts.value(source.path); break; }
                    // No disk fallback for reserved/ambiguous live sessions.
                    if (text.result != EditResult::Applied) { result.fragmentProblems.insert(key, QStringLiteral("目标标题未覆盖，请重新检查或打开目标确认")); continue; }
                    maps.insert(state.identity, analyzeMarkdownHeadings(text));
                }
                const auto &map = maps[state.identity];
                if (!map.success) result.fragmentProblems.insert(key, QStringLiteral("目标标题暂时无法分析"));
                else if (std::none_of(map.headings.cbegin(), map.headings.cend(), [&](const auto &heading) { return heading.id == reference.target.fragment; }))
                    result.fragmentProblems.insert(key, QStringLiteral("标题不存在或已变化；可定位源文并重新选择目标标题"));
            }
        }
        if (stopped() || !root || !root->active()) {
            result.cancelled = stopped(); result.index.complete = false;
            result.index.sources.clear(); result.index.references.clear();
            result.sourceTexts.clear();
            result.fragmentProblems.clear();
            result.targets.clear(); result.imageCandidates.clear(); result.watchedDirectories.clear();
            result.index.gaps.append(result.cancelled ? QStringLiteral("扫描已取消") : QStringLiteral("工作区授权已失效"));
        }
        return result;
    };
    if (!root || !root->active() || stopped()) return finish();
    if (limits.entries < 1 || limits.entries > 50000 || limits.directories < 1 || limits.directories > 512
        || limits.documents < 1 || limits.documents > 1024 || limits.sourceBytes < 1 || limits.sourceBytes > 32 * 1024 * 1024) {
        input.sources.clear(); input.enumerationGaps.append(QStringLiteral("扫描上限无效")); return finish();
    }
    QHash<QString, quint64> livePathGenerations;
    qsizetype liveBytes = 0;
    if (input.sources.size() > limits.documents || input.sessionsOutsideRoot.size() > limits.documents) {
        input.sources.clear(); input.enumerationGaps.append(QStringLiteral("打开会话数量超过扫描上限")); return finish();
    }
    for (const auto &source : input.sources) {
        if (source.origin != ReferenceIndexSource::Origin::Session) {
            input.sources.clear(); input.enumerationGaps.append(QStringLiteral("磁盘快照必须由本次受控扫描取得")); return finish();
        }
        if (source.snapshot.utf8.size() > FileService::SourceLimit || source.snapshot.utf8.size() > limits.sourceBytes - liveBytes) {
            input.sources.clear(); input.enumerationGaps.append(QStringLiteral("打开会话文本超过扫描上限")); return finish();
        }
        liveBytes += source.snapshot.utf8.size();
        livePathGenerations.insert(source.path, source.pathGeneration);
    }
    QStringList pending{QString()};
    QSet<QString> directories;
    bool limited = false;
    while (!pending.isEmpty() && !stopped()) {
        if (result.visitedDirectories >= limits.directories || result.visitedEntries >= limits.entries) { limited = true; break; }
        const auto directory = pending.takeFirst();
        const auto listing = root->listDirectory(directory, qMin(10000, limits.entries - result.visitedEntries));
        ++result.visitedDirectories;
        if (!listing.ok()) {
            input.enumerationGaps.append(QStringLiteral("无法完整读取目录：%1（%2）").arg(directory.isEmpty() ? QStringLiteral(".") : directory).arg(int(listing.error)));
            continue;
        }
        if (directories.contains(listing.identity)) {
            input.enumerationGaps.append(QStringLiteral("目录身份重复：%1").arg(directory)); continue;
        }
        directories.insert(listing.identity);
        result.watchedDirectories.append(directory);
        if (!listing.complete) input.enumerationGaps.append(QStringLiteral("目录枚举达到上限：%1").arg(directory.isEmpty() ? QStringLiteral(".") : directory));
        result.visitedEntries += listing.entries.size();
        for (const auto &entry : listing.entries) {
            if (stopped()) break;
            const auto path = directory.isEmpty() ? entry.name : directory + '/' + entry.name;
            if (entry.kind == WorkspaceDirectoryEntry::Kind::Unsupported) {
                input.enumerationGaps.append(QStringLiteral("未扫描链接或不支持的对象：%1").arg(path)); continue;
            }
            if (entry.kind == WorkspaceDirectoryEntry::Kind::Directory) {
                if (entry.name == ".git" || entry.name == ".hg" || entry.name == ".svn") result.excludedDirectories.append(path);
                else if (pending.size() + result.visitedDirectories < limits.directories) pending.append(path);
                else limited = true;
                continue;
            }
            const auto suffix = QFileInfo(entry.name).suffix().toLower();
            if (path.startsWith("assets/images/") && QStringList{"png", "jpg", "jpeg", "gif", "bmp", "webp"}.contains(suffix)) result.imageCandidates.append(path);
            if (suffix != "md" && suffix != "markdown") continue;
            if (result.readDocuments >= limits.documents || result.readBytes + liveBytes >= limits.sourceBytes) { limited = true; break; }
            const auto read = root->read(path, qMin<qsizetype>(FileService::DiskByteLimit, limits.sourceBytes - result.readBytes - liveBytes));
            ++result.readDocuments;
            if (!read.ok()) {
                input.enumerationGaps.append(QStringLiteral("无法读取文档：%1（%2）").arg(path).arg(int(read.error))); continue;
            }
            result.readBytes += read.bytes.size();
            if (input.sessionsOutsideRoot.contains(read.identity)) {
                input.enumerationGaps.append(QStringLiteral("文档同时在工作区外的其他路径打开，无法确认相对引用基准：%1").arg(path)); continue;
            }
            auto text = read.bytes.startsWith(QByteArray::fromHex("efbbbf")) ? read.bytes.mid(3) : read.bytes;
            bool valid = false; decodeUtf8Literal(text, &valid);
            if (!valid || text.contains('\0') || text.size() > FileService::SourceLimit) {
                input.enumerationGaps.append(QStringLiteral("文档不是受支持的 UTF-8 源文：%1").arg(path)); continue;
            }
            input.sources.append({path, read.identity, livePathGenerations.value(path, input.rootGeneration),
                {EditResult::Applied, std::move(text), input.scanGeneration, QUuid::createUuid()}, ReferenceIndexSource::Origin::Disk});
        }
        if (limited) break;
    }
    if (limited) input.enumerationGaps.append(QStringLiteral("扫描达到目录、条目、文档或字节上限"));
    input.enumerationComplete = pending.isEmpty() && !limited && !stopped();
    return finish();
}
}
