#include "ImageImport.h"
#include "FileService.h"
#include "ImageAdmission.h"
#include <QCryptographicHash>
#include <QFileInfo>

namespace xdnote {
ImageImportResult importWorkspaceImage(const std::shared_ptr<WorkspaceResourceReader> &root,
    const QString &documentPath, const QString &documentIdentity, const QString &sourcePath, const QImage &bitmap,
    const QString &destination, const std::shared_ptr<WorkspaceResourceReader> &documentRoot) {
    ImageImportResult result;
    if (!root || !root->active()) { result.message = QStringLiteral("工作区授权已失效。"); return result; }
    QByteArray source;
    QString name;
    if (!sourcePath.isEmpty()) {
        auto read = FileService::readSelectedBinary(sourcePath, MaxImageSourceBytes);
        if (!read.success) {
            result.message = read.problem.code == FileError::TooLarge
                ? QStringLiteral("图片超过 8 MiB 的大小限制。")
                : QStringLiteral("无法读取所选图片：%1").arg(read.problem.description());
            return result;
        }
        source = std::move(read.bytes); name = QFileInfo(sourcePath).fileName();
    } else {
        if (!encodeClipboardImage(bitmap, source)) { result.message = QStringLiteral("剪贴板图片无效或超过大小限制。"); return result; }
        name = QStringLiteral("image-%1.png").arg(QString::fromLatin1(QCryptographicHash::hash(source, QCryptographicHash::Sha256).toHex().left(12)));
    }
    qint64 pixels = 0; QByteArray png, format;
    const auto status = decodeImage(source, pixels, 32 * 1024 * 1024, png, &format);
    if (status != 1) {
        result.message = status == 3 ? QStringLiteral("图片超过大小或解码尺寸限制。")
                                     : QStringLiteral("图片损坏或格式不受支持。请使用 PNG、JPEG、GIF、BMP 或 WebP。");
        return result;
    }
    result.publication = root->publishImage(documentPath, documentIdentity, source, name, QString::fromLatin1(format), {}, destination, documentRoot);
    if (result.publication.status == WorkspaceWriteStatus::Committed || result.publication.status == WorkspaceWriteStatus::Reused) {
        result.imageIdentity = result.publication.identity;
        result.imageDigest = QCryptographicHash::hash(source, QCryptographicHash::Sha256);
#ifndef Q_OS_WIN
        const auto verified = root->read(result.publication.path, MaxImageSourceBytes);
        if (!verified.ok() || verified.bytes != source || verified.identity != result.imageIdentity) result.imageIdentity.clear();
#endif
        if (result.imageIdentity.isEmpty()) { result.publication.status = WorkspaceWriteStatus::PublishedButUnverified; result.publication.error = WorkspaceReadError::Changed; }
    }
    if (result.publication.status == WorkspaceWriteStatus::Failed) {
        switch (result.publication.error) {
        case WorkspaceReadError::Unsupported: result.message = QStringLiteral("当前平台尚不支持安全写入工作区图片。"); break;
        case WorkspaceReadError::Denied: result.message = QStringLiteral("没有权限写入工作区图片目录。"); break;
        case WorkspaceReadError::Revoked: case WorkspaceReadError::RootChanged:
            result.message = QStringLiteral("工作区授权已变化，未插入图片。"); break;
        case WorkspaceReadError::OutsideOrLink: case WorkspaceReadError::InvalidPath:
            result.message = QStringLiteral("无法核验图片目录或文档位置，未写入图片。"); break;
        default: result.message = QStringLiteral("无法安全写入工作区图片，请检查目录和文档是否发生变化。"); break;
        }
    }
    return result;
}
}
