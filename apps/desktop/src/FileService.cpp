#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "FileService.h"
#include "Utf8Text.h"

#include <QCryptographicHash>
#include <QDir>
#include <utility>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace xdnote {
QString FileProblem::description() const {
    switch (code) {
    case FileError::None: return {};
    case FileError::InvalidPath: return QStringLiteral("文件路径无效或当前入口不支持此路径");
    case FileError::Missing: return QStringLiteral("文件或所在目录不存在");
    case FileError::Denied: return QStringLiteral("没有读取此文件的权限");
    case FileError::InUse: return QStringLiteral("文件正被其他程序占用，请稍后重试");
    case FileError::UnsupportedFile: return QStringLiteral("当前入口只支持普通文件，不支持最终链接或重解析点");
    case FileError::TooLarge: return QStringLiteral("文件超过当前读取额度，未载入正文");
    case FileError::InvalidUtf8: return QStringLiteral("文件不是有效的 UTF-8 文本，请先转换编码");
    case FileError::UnsupportedContent: return QStringLiteral("文件含空字符，当前文件入口不支持此内容");
    case FileError::Changed: return QStringLiteral("文件在读取期间发生变化，请重新打开");
    case FileError::Io: return QStringLiteral("无法读取文件，请检查存储设备后重试");
    }
    return {};
}

struct NativeFile {
#ifdef Q_OS_WIN
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~NativeFile() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
#else
    int handle = -1;
    ~NativeFile() { if (handle >= 0) ::close(handle); }
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
#else
    struct stat value{};
    if (::fstat(file.handle, &value) != 0) return nativeProblem(errno);
    if (!S_ISREG(value.st_mode)) return {FileError::UnsupportedFile};
    info.identity = QStringLiteral("posix:%1:%2").arg(qulonglong(value.st_dev), 0, 16).arg(qulonglong(value.st_ino), 0, 16);
    info.size = value.st_size;
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
}
