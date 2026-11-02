#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "FileService.h"
#include "Utf8Text.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>
#include <utility>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#ifdef Q_OS_MACOS
#include <copyfile.h>
#else
#include <sys/xattr.h>
#endif
#endif

namespace xdnote {
QString FileProblem::description() const {
    switch (code) {
    case FileError::None: return {};
    case FileError::InvalidPath: return QStringLiteral("文件路径无效或当前入口不支持此路径");
    case FileError::Missing: return QStringLiteral("文件或所在目录不存在");
    case FileError::Denied: return QStringLiteral("没有访问此文件或目录的权限，请检查权限或另存为");
    case FileError::InUse: return QStringLiteral("文件正被其他程序占用，请稍后重试");
    case FileError::UnsupportedFile: return QStringLiteral("当前入口只支持普通文件，不支持最终链接或重解析点");
    case FileError::TooLarge: return QStringLiteral("正文超过当前支持的 2 MiB，请缩小内容后重试");
    case FileError::InvalidUtf8: return QStringLiteral("文件不是有效的 UTF-8 文本，请先转换编码");
    case FileError::UnsupportedContent: return QStringLiteral("文件含空字符，当前文件入口不支持此内容");
    case FileError::Changed: return QStringLiteral("磁盘文件已变化，请重新载入或另存为");
    case FileError::Io:
#ifdef Q_OS_WIN
        if (nativeCode == ERROR_DISK_FULL || nativeCode == ERROR_HANDLE_DISK_FULL) return QStringLiteral("存储空间不足，请释放空间后重试，或另存到其他存储位置");
#else
        if (nativeCode == ENOSPC || nativeCode == EDQUOT) return QStringLiteral("存储空间不足或配额已满，请释放空间后重试，或另存到其他存储位置");
#endif
        return QStringLiteral("无法完成文件操作，请检查存储设备后重试");
    case FileError::HardLinks: return QStringLiteral("此文件有多个硬链接，请另存为以保留原文件及其他链接");
    case FileError::AtomicUnsupported: return QStringLiteral("此存储位置不支持安全替换，请另存到普通本地目录");
    }
    return {};
}

struct NativeFile {
    bool borrowed = false;
#ifdef Q_OS_WIN
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~NativeFile() { if (!borrowed && handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
#else
    int handle = -1;
    ~NativeFile() { if (!borrowed && handle >= 0) ::close(handle); }
#endif
};

namespace {
struct NativeInfo {
    QString identity;
    qint64 size = 0;
    qint64 modified = 0;
    qint64 changed = 0;
    qint64 modifiedNanos = 0;
    qint64 changedNanos = 0;
    quint64 links = 0;
    quint32 mode = 0;
#ifndef Q_OS_WIN
    uid_t owner = 0;
    gid_t group = 0;
#endif
    bool operator==(const NativeInfo &other) const {
        return identity == other.identity && size == other.size && modified == other.modified
            && changed == other.changed && modifiedNanos == other.modifiedNanos && changedNanos == other.changedNanos;
    }
};

FileProblem nativeProblem(qint64 code) {
#ifdef Q_OS_WIN
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return {FileError::Missing, code};
    if (code == ERROR_ACCESS_DENIED) return {FileError::Denied, code};
    if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION) return {FileError::InUse, code};
    if (code == ERROR_INVALID_NAME || code == ERROR_FILENAME_EXCED_RANGE) return {FileError::InvalidPath, code};
#else
    if (code == ENOENT || code == ENOTDIR) return {FileError::Missing, code};
    if (code == EACCES || code == EPERM) return {FileError::Denied, code};
    if (code == ELOOP) return {FileError::UnsupportedFile, code};
#endif
    return {FileError::Io, code};
}

FileProblem inspect(const NativeFile &file, NativeInfo &info) {
#ifdef Q_OS_WIN
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    FILE_STANDARD_INFO standard{};
    FILE_BASIC_INFO basic{};
    FILE_ID_INFO id{};
    if (!GetFileInformationByHandleEx(file.handle, FileAttributeTagInfo, &attributes, sizeof(attributes))
        || !GetFileInformationByHandleEx(file.handle, FileStandardInfo, &standard, sizeof(standard))
        || !GetFileInformationByHandleEx(file.handle, FileBasicInfo, &basic, sizeof(basic))
        || !GetFileInformationByHandleEx(file.handle, FileIdInfo, &id, sizeof(id))) return nativeProblem(GetLastError());
    if (standard.Directory || (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        || GetFileType(file.handle) != FILE_TYPE_DISK) return {FileError::UnsupportedFile};
    info.identity = QStringLiteral("win:%1:%2").arg(id.VolumeSerialNumber, 0, 16).arg(
        QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(id.FileId.Identifier), 16).toHex()));
    info.size = standard.EndOfFile.QuadPart;
    info.modified = basic.LastWriteTime.QuadPart;
    info.changed = basic.ChangeTime.QuadPart;
    info.links = standard.NumberOfLinks;
    info.mode = attributes.FileAttributes;
#else
    struct stat value{};
    if (::fstat(file.handle, &value) != 0) return nativeProblem(errno);
    if (!S_ISREG(value.st_mode)) return {FileError::UnsupportedFile};
    info.identity = QStringLiteral("posix:%1:%2").arg(qulonglong(value.st_dev), 0, 16).arg(qulonglong(value.st_ino), 0, 16);
    info.size = value.st_size;
    info.links = value.st_nlink;
    info.mode = value.st_mode;
    info.owner = value.st_uid;
    info.group = value.st_gid;
#ifdef Q_OS_MACOS
    info.modified = value.st_mtimespec.tv_sec;
    info.modifiedNanos = value.st_mtimespec.tv_nsec;
    info.changed = value.st_ctimespec.tv_sec;
    info.changedNanos = value.st_ctimespec.tv_nsec;
#else
    info.modified = value.st_mtim.tv_sec;
    info.modifiedNanos = value.st_mtim.tv_nsec;
    info.changed = value.st_ctim.tv_sec;
    info.changedNanos = value.st_ctim.tv_nsec;
#endif
#endif
    if (info.size < 0) return {FileError::UnsupportedFile};
    return {};
}

struct NativeOpen {
    std::shared_ptr<NativeFile> file;
    NativeInfo info;
    FileProblem problem;
};

NativeOpen openFile(const QString &path, bool contents) {
    auto file = std::make_shared<NativeFile>();
#ifdef Q_OS_WIN
    const auto nativePath = QDir::toNativeSeparators(path);
    file->handle = CreateFileW(reinterpret_cast<LPCWSTR>(nativePath.utf16()), contents ? GENERIC_READ : FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file->handle == INVALID_HANDLE_VALUE) return {{}, {}, nativeProblem(GetLastError())};
#else
    const auto nativePath = path.toUtf8();
    int flags = O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
#ifdef Q_OS_MACOS
    flags |= contents ? O_RDONLY : O_EVTONLY;
#else
    flags |= contents ? O_RDONLY : O_PATH;
#endif
    file->handle = ::open(nativePath.constData(), flags);
    if (file->handle < 0) return {{}, {}, nativeProblem(errno)};
#endif
    NativeInfo info;
    auto problem = inspect(*file, info);
    if (problem.code != FileError::None) return {{}, {}, problem};
    return {std::move(file), info, {}};
}

FileProblem readBytes(const NativeFile &file, QByteArray &bytes) {
#ifdef Q_OS_WIN
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(file.handle, zero, nullptr, FILE_BEGIN)) return nativeProblem(GetLastError());
#else
    if (::lseek(file.handle, 0, SEEK_SET) < 0) return nativeProblem(errno);
#endif
    char chunk[64 * 1024];
    for (;;) {
#ifdef Q_OS_WIN
        DWORD count = 0;
        if (!ReadFile(file.handle, chunk, sizeof(chunk), &count, nullptr)) return nativeProblem(GetLastError());
#else
        const auto count = ::read(file.handle, chunk, sizeof(chunk));
        if (count < 0) { if (errno == EINTR) continue; return nativeProblem(errno); }
#endif
        if (!count) return {};
        if (bytes.size() + qsizetype(count) > FileService::SourceLimit + 3) return {FileError::TooLarge};
        bytes.append(chunk, qsizetype(count));
    }
}
}

FileProbe::FileProbe(QString path, QString identity, std::shared_ptr<NativeFile> lease)
    : m_path(std::move(path)), m_identity(std::move(identity)), m_lease(std::move(lease)) {}

FileBaseline::FileBaseline(std::shared_ptr<const FileProbe> file, QUuid requestId, QByteArray disk, QByteArray utf8, bool bom)
    : m_file(std::move(file)), m_utf8(std::move(utf8)), m_diskBytes(std::move(disk)),
      m_sha256(QCryptographicHash::hash(m_diskBytes, QCryptographicHash::Sha256)), m_bom(bom), m_requestId(requestId) {}

ProbeResult FileService::probe(const QString &absolutePath) {
    if (absolutePath.isEmpty() || absolutePath.contains(QChar(0)) || !QDir::isAbsolutePath(absolutePath))
        return {{}, {FileError::InvalidPath}};
    if (decodeUtf8Literal(absolutePath.toUtf8()) != absolutePath) return {{}, {FileError::InvalidPath}};
#ifdef Q_OS_WIN
    // Exclude device/pipe namespaces, UNC and alternate streams until admitted.
    const auto drive = absolutePath[0].unicode();
    if (absolutePath.size() < 3 || !((drive >= 'A' && drive <= 'Z') || (drive >= 'a' && drive <= 'z')) || absolutePath[1] != ':'
        || (absolutePath[2] != '/' && absolutePath[2] != '\\') || absolutePath.mid(2).contains(':'))
        return {{}, {FileError::InvalidPath}};
#endif
    auto opened = openFile(absolutePath, false);
    if (!opened.file) return {{}, opened.problem};
    return {std::shared_ptr<const FileProbe>(new FileProbe(absolutePath, opened.info.identity, std::move(opened.file))), {}};
}

ReadResult FileService::read(const FileReadRequest &request) {
    const auto &file = request.file;
    if (!file || request.requestId.isNull()) return {{}, {FileError::InvalidPath}};
    auto opened = openFile(file->path(), true);
    if (!opened.file) return {{}, opened.problem};
    if (opened.info.identity != file->identity()) return {{}, {FileError::Changed}};
    if (opened.info.size > SourceLimit + 3) return {{}, {FileError::TooLarge}};
    QByteArray first, second;
    auto problem = readBytes(*opened.file, first);
    if (problem.code != FileError::None) return {{}, problem};
    NativeInfo between, after;
    problem = inspect(*opened.file, between);
    if (problem.code != FileError::None) return {{}, problem};
    if (!(opened.info == between) || first.size() != between.size) return {{}, {FileError::Changed}};
    problem = readBytes(*opened.file, second);
    if (problem.code != FileError::None) return {{}, problem};
    problem = inspect(*opened.file, after);
    if (problem.code != FileError::None) return {{}, problem};
    auto namespaceCheck = probe(file->path());
    if (!(between == after) || first != second || !namespaceCheck.file || namespaceCheck.file->identity() != file->identity())
        return {{}, {FileError::Changed, namespaceCheck.problem.nativeCode}};
    // These observations are not a cross-process atomic read or write token.
    const bool bom = first.startsWith(QByteArray::fromHex("efbbbf"));
    auto utf8 = bom ? first.mid(3) : first;
    if (utf8.size() > SourceLimit) return {{}, {FileError::TooLarge}};
    bool valid = false;
    decodeUtf8Literal(utf8, &valid);
    if (!valid) return {{}, {FileError::InvalidUtf8}};
    if (utf8.contains('\0')) return {{}, {FileError::UnsupportedContent}};
    return {std::shared_ptr<const FileBaseline>(new FileBaseline(file, request.requestId, std::move(first), std::move(utf8), bom)), {}};
}

FileSaveResult FileService::save(const FileSaveRequest &request, const std::function<void(SaveStage)> &observer) {
    FileSaveResult result;
    result.documentId = request.documentId;
    result.revision = request.revision;
    result.requestId = request.requestId;
    const auto path = request.path;
    bool valid = false;
    if (request.utf8.size() <= SourceLimit) decodeUtf8Literal(request.utf8, &valid);
    if (!valid || request.utf8.contains('\0') || request.requestId.isNull()) {
        result.problem = {request.utf8.size() > SourceLimit ? FileError::TooLarge : !valid ? FileError::InvalidUtf8 : FileError::UnsupportedContent};
        return result;
    }
    const auto normalized = QDir::fromNativeSeparators(path);
    const int separator = int(normalized.lastIndexOf('/'));
    const auto parent = normalized.left(separator + 1);
    const auto leaf = normalized.mid(separator + 1);
    auto initialProbe = probe(path);
    if (initialProbe.problem.code == FileError::InvalidPath || separator < 0 || leaf.isEmpty() || leaf == "." || leaf == "..") {
        result.problem = {FileError::InvalidPath}; return result;
    }
    if (request.baseline) {
        if (!initialProbe.file || initialProbe.file->identity() != request.baseline->file()->identity()) {
            result.status = SaveStatus::Conflict;
            result.problem = initialProbe.file ? FileProblem{FileError::Changed} : initialProbe.problem;
            return result;
        }
    } else if (initialProbe.file || initialProbe.problem.code != FileError::Missing) {
        result.status = SaveStatus::Conflict;
        result.problem = initialProbe.file ? FileProblem{FileError::Changed} : initialProbe.problem;
        return result;
    }
    NativeFile directory;
    NativeFile writerExclusion;
#ifdef Q_OS_WIN
    const auto nativeParent = QDir::toNativeSeparators(parent);
    const auto nativeTarget = QDir::toNativeSeparators(path);
    directory.handle = CreateFileW(reinterpret_cast<LPCWSTR>(nativeParent.utf16()), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (directory.handle == INVALID_HANDLE_VALUE) { result.problem = nativeProblem(GetLastError()); return result; }
    if (request.baseline) {
        HANDLE permission = CreateFileW(reinterpret_cast<LPCWSTR>(nativeTarget.utf16()), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (permission == INVALID_HANDLE_VALUE) { result.problem = nativeProblem(GetLastError()); return result; }
        CloseHandle(permission);
        // Exclude open/new data writers while still permitting ReplaceFile's
        // read/delete handles. Namespace replacement remains independently checked.
        writerExclusion.handle = CreateFileW(reinterpret_cast<LPCWSTR>(nativeTarget.utf16()), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (writerExclusion.handle == INVALID_HANDLE_VALUE) { result.problem = nativeProblem(GetLastError()); return result; }
    }
#else
    const auto parentBytes = parent.toUtf8(), leafBytes = leaf.toUtf8();
    directory.handle = ::open(parentBytes.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NONBLOCK);
    if (directory.handle < 0) { result.problem = nativeProblem(errno); return result; }
    if (request.baseline && ::faccessat(directory.handle, leafBytes.constData(), W_OK, 0) != 0) {
        result.problem = nativeProblem(errno); return result;
    }
#endif
    auto source = request.baseline ? openFile(path, true) : NativeOpen{};
    if (request.baseline && !source.file) { result.problem = source.problem; return result; }
    if (source.file && source.info.identity != request.baseline->file()->identity()) {
        result.status = SaveStatus::Conflict; result.problem = {FileError::Changed}; return result;
    }
    if (source.file && source.info.links != 1) { result.problem = {FileError::HardLinks}; return result; }
#ifdef Q_OS_WIN
    if (source.file && (source.info.mode & FILE_ATTRIBUTE_READONLY)) { result.problem = {FileError::Denied}; return result; }
#else
    if (source.file && (!(source.info.mode & 0222) || (source.info.mode & 06000))) { result.problem = {FileError::Denied}; return result; }
#endif
    auto unchanged = [&]() -> FileProblem {
        auto current = probe(path);
        if (!request.baseline) return !current.file && current.problem.code == FileError::Missing
            ? FileProblem{} : FileProblem{FileError::Changed, current.problem.nativeCode};
        if (!current.file || current.file->identity() != request.baseline->file()->identity())
            return {FileError::Changed, current.problem.nativeCode};
        auto readResult = read({current.file, request.requestId});
        if (!readResult.baseline) return readResult.problem;
        if (readResult.baseline->diskBytes() != request.baseline->diskBytes()) return {FileError::Changed};
        NativeInfo info;
        auto problem = inspect(*source.file, info);
        if (problem.code != FileError::None) return problem;
        if (info.links != 1) return {FileError::HardLinks};
        return {};
    };
    auto before = unchanged();
    if (before.code != FileError::None) { result.status = SaveStatus::Conflict; result.problem = before; return result; }
    auto temporary = std::make_unique<QTemporaryFile>(parent + QStringLiteral(".xdnote-XXXXXX"));
    temporary->setAutoRemove(false);
    QString temporaryPath;
    QString temporaryIdentity;
    bool committed = false;
    const bool bom = request.bom || request.utf8.startsWith(QByteArray::fromHex("efbbbf"));
    const auto disk = (bom ? QByteArray::fromHex("efbbbf") : QByteArray()) + request.utf8;
    auto finish = [&](SaveStatus status, FileProblem problem) {
        temporary.reset();
        result.status = status;
        result.problem = problem;
        if (status != SaveStatus::Uncertain && !temporaryIdentity.isEmpty()) {
            auto own = probe(temporaryPath);
            if (own.file && own.file->identity() == temporaryIdentity) QFile::remove(temporaryPath);
        }
        if (QFileInfo::exists(temporaryPath)) result.stagedPath = temporaryPath;
        return result;
    };
    try {
        if (observer) observer(SaveStage::Validate);
        result.stage = SaveStage::StageWrite;
        if (!temporary->open()) return finish(SaveStatus::Failed, {FileError::Io});
        temporaryPath = temporary->fileName();
        NativeFile borrowed;
        borrowed.borrowed = true;
#ifdef Q_OS_WIN
        borrowed.handle = reinterpret_cast<HANDLE>(_get_osfhandle(temporary->handle()));
#else
        borrowed.handle = temporary->handle();
#endif
        NativeInfo stagedInfo;
        auto stageProblem = inspect(borrowed, stagedInfo);
        if (stageProblem.code != FileError::None) return finish(SaveStatus::Failed, stageProblem);
        temporaryIdentity = stagedInfo.identity;
        if (temporary->write(disk) != disk.size() || !temporary->flush()) return finish(SaveStatus::Failed, {FileError::Io});
#ifdef Q_OS_WIN
        if (!FlushFileBuffers(borrowed.handle)) return finish(SaveStatus::Failed, nativeProblem(GetLastError()));
#else
        if (source.file) {
#ifdef Q_OS_MACOS
            if (::fcopyfile(source.file->handle, temporary->handle(), nullptr, COPYFILE_METADATA) != 0)
                return finish(SaveStatus::Failed, nativeProblem(errno));
#else
            if (::fchown(temporary->handle(), source.info.owner, source.info.group) != 0
                || ::fchmod(temporary->handle(), mode_t(source.info.mode & 0777)) != 0)
                return finish(SaveStatus::Failed, nativeProblem(errno));
            const auto length = ::flistxattr(source.file->handle, nullptr, 0);
            if (length < 0 || length > 65536) return finish(SaveStatus::Failed, {FileError::Denied, errno});
            QByteArray names(qsizetype(length), '\0');
            if (length && ::flistxattr(source.file->handle, names.data(), size_t(length)) != length)
                return finish(SaveStatus::Failed, nativeProblem(errno));
            for (qsizetype offset = 0; offset < names.size();) {
                const char *name = names.constData() + offset;
                const QByteArray attribute(name);
                offset += attribute.size() + 1;
                if (attribute == "security.capability") return finish(SaveStatus::Failed, {FileError::Denied});
                const auto size = ::fgetxattr(source.file->handle, name, nullptr, 0);
                if (size < 0 || size > 1024 * 1024) return finish(SaveStatus::Failed, {FileError::Denied, errno});
                QByteArray value(qsizetype(size), '\0');
                if (::fgetxattr(source.file->handle, name, value.data(), size_t(size)) != size
                    || ::fsetxattr(temporary->handle(), name, value.constData(), size_t(size), 0) != 0)
                    return finish(SaveStatus::Failed, nativeProblem(errno));
            }
#endif
        }
        if (::futimens(temporary->handle(), nullptr) != 0 || ::fsync(temporary->handle()) != 0)
            return finish(SaveStatus::Failed, nativeProblem(errno));
#endif
        temporary.reset();
        if (observer) observer(SaveStage::StageWrite);
        before = unchanged();
        if (before.code != FileError::None) return finish(SaveStatus::Conflict, before);
        const auto stagedProbe = probe(temporaryPath);
        if (!stagedProbe.file || stagedProbe.file->identity() != temporaryIdentity)
            return finish(SaveStatus::Failed, {FileError::Changed});
#ifndef Q_OS_WIN
        NativeFile anchoredStage;
        const auto stageName = QFileInfo(temporaryPath).fileName().toUtf8();
        anchoredStage.handle = ::openat(directory.handle, stageName.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        NativeInfo anchoredInfo;
        if (anchoredStage.handle < 0 || inspect(anchoredStage, anchoredInfo).code != FileError::None || anchoredInfo.identity != temporaryIdentity)
            return finish(SaveStatus::Failed, {FileError::Changed});
#endif
        result.stage = SaveStage::Commit;
        if (observer) observer(SaveStage::Commit);
#ifdef Q_OS_WIN
        const auto nativeStage = QDir::toNativeSeparators(temporaryPath);
        if (request.baseline) {
            result.recoveryPath = parent + QStringLiteral(".xdnote-") + request.requestId.toString(QUuid::WithoutBraces) + QStringLiteral(".previous");
            const auto nativeBackup = QDir::toNativeSeparators(result.recoveryPath);
            if (!ReplaceFileW(reinterpret_cast<LPCWSTR>(nativeTarget.utf16()), reinterpret_cast<LPCWSTR>(nativeStage.utf16()),
                reinterpret_cast<LPCWSTR>(nativeBackup.utf16()), 0, nullptr, nullptr)) {
                const auto error = GetLastError();
                const auto proof = unchanged();
                if (proof.code == FileError::None && !QFileInfo::exists(result.recoveryPath)) {
                    result.recoveryPath.clear();
                    return finish(SaveStatus::Failed, nativeProblem(error));
                }
                return finish(SaveStatus::Uncertain, nativeProblem(error));
            }
        } else if (!MoveFileExW(reinterpret_cast<LPCWSTR>(nativeStage.utf16()), reinterpret_cast<LPCWSTR>(nativeTarget.utf16()), MOVEFILE_WRITE_THROUGH)) {
            const auto error = GetLastError();
            return finish(error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS ? SaveStatus::Conflict : SaveStatus::Failed, nativeProblem(error));
        }
#else
        const auto temporaryLeaf = QFileInfo(temporaryPath).fileName().toUtf8();
        if (request.baseline) {
#ifdef Q_OS_MACOS
            const int status = ::renameatx_np(directory.handle, temporaryLeaf.constData(), directory.handle, leafBytes.constData(), RENAME_SWAP);
#else
            const int status = ::renameat2(directory.handle, temporaryLeaf.constData(), directory.handle, leafBytes.constData(), RENAME_EXCHANGE);
#endif
            if (status != 0) {
                const auto error = errno;
                return finish(SaveStatus::Failed, error == ENOSYS || error == EINVAL || error == ENOTSUP
                    ? FileProblem{FileError::AtomicUnsupported, error} : nativeProblem(error));
            }
            result.recoveryPath = temporaryPath;
        } else if (::linkat(directory.handle, temporaryLeaf.constData(), directory.handle, leafBytes.constData(), 0) != 0) {
            const auto error = errno;
            return finish(error == EEXIST ? SaveStatus::Conflict : SaveStatus::Failed, error == EEXIST ? FileProblem{FileError::Changed, error} : nativeProblem(error));
        }
#endif
        committed = true;
        result.stage = SaveStage::Verify;
        if (observer) observer(SaveStage::Verify);
        if (request.baseline) {
            const auto displacedProbe = probe(result.recoveryPath);
            if (!displacedProbe.file || displacedProbe.file->identity() != request.baseline->file()->identity())
                return finish(SaveStatus::Uncertain, {FileError::Changed});
            const auto displaced = read({displacedProbe.file, request.requestId});
            if (!displaced.baseline || displaced.baseline->diskBytes() != request.baseline->diskBytes())
                return finish(SaveStatus::Uncertain, {FileError::Changed});
        }
        auto savedProbe = probe(path);
        if (!savedProbe.file || savedProbe.file->identity() != temporaryIdentity)
            return finish(SaveStatus::Uncertain, {FileError::Changed, savedProbe.problem.nativeCode});
        auto saved = read({savedProbe.file, request.requestId});
        if (!saved.baseline || saved.baseline->diskBytes() != disk || saved.baseline->utf8() != request.utf8)
            return finish(SaveStatus::Uncertain, {FileError::Changed});
        if (!result.recoveryPath.isEmpty() && !QFile::remove(result.recoveryPath))
            return finish(SaveStatus::Uncertain, {FileError::Io});
        result.recoveryPath.clear();
        result.baseline = saved.baseline;
        return finish(SaveStatus::Committed, {});
    } catch (...) {
        return finish(committed ? SaveStatus::Uncertain : SaveStatus::Failed, {FileError::Io});
    }
}
}
