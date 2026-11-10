#include "WorkspaceReferenceScan.h"
#include "FileService.h"
#include "Utf8Text.h"
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <utility>

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
        if (stopped() || !root || !root->active()) {
            result.cancelled = stopped(); result.index.complete = false;
            result.index.sources.clear(); result.index.references.clear();
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
    if (input.sources.size() > limits.documents) {
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
            if (suffix != "md" && suffix != "markdown") continue;
            if (result.readDocuments >= limits.documents || result.readBytes + liveBytes >= limits.sourceBytes) { limited = true; break; }
            const auto read = root->read(path, qMin<qsizetype>(FileService::DiskByteLimit, limits.sourceBytes - result.readBytes - liveBytes));
            ++result.readDocuments;
            if (!read.ok()) {
                input.enumerationGaps.append(QStringLiteral("无法读取文档：%1（%2）").arg(path).arg(int(read.error))); continue;
            }
            result.readBytes += read.bytes.size();
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
