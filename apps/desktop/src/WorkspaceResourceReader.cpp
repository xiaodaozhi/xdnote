#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "WorkspaceResourceReader.h"
#include "FileService.h"

#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStringList>
#include <QUuid>
#include <atomic>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>
#include <stdexcept>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
extern "C" NTSYSAPI NTSTATUS NTAPI NtSetInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef Q_OS_LINUX
#include <dirent.h>
#include <linux/openat2.h>
#include <sys/syscall.h>
#endif
#endif

namespace xdnote {
namespace {
// Bound both memory use and the namespace supplied to native APIs. The
// caller must impose the smaller format-specific byte and pixel limits.
constexpr qsizetype MaxReadBytes = 32 * 1024 * 1024;

QStringList components(const QString &path) {
    if (path.isEmpty() || path.size() > 8192 || path.startsWith('/') || path.contains('\\')) return {};
    const auto parts = path.split('/');
    for (const auto &part : parts) {
        if (part.isEmpty() || part == "." || part == ".." || part.size() > 255) return {};
        if (QString::fromUtf8(part.toUtf8()) != part) return {};
        for (const auto ch : part) {
            if (ch.isNull() || ch.category() == QChar::Other_Control) return {};
#ifdef Q_OS_WIN
            // Prevent alternate streams, wildcard/device namespaces and Win32
            // names that the normal user-facing path APIs reinterpret.
            if (QStringLiteral(":*?\"<>|").contains(ch)) return {};
#endif
        }
#ifdef Q_OS_WIN
        if (part.endsWith(' ') || part.endsWith('.')) return {};
        const auto stem = part.section('.', 0, 0).toUpper();
        if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL"
            || ((stem.startsWith("COM") || stem.startsWith("LPT")) && stem.size() == 4
                && stem.at(3) >= '1' && stem.at(3) <= '9')) return {};
#endif
    }
    return parts;
}

#ifndef Q_OS_WIN
QJsonObject helperRequest(const QString &root, const QString &identity, const QString &operation,
                         const QString &path, int limit = 0, QJsonObject extra = {}) {
    const auto executable = QCoreApplication::applicationDirPath() + QStringLiteral("/xdnote-resource-helper");
    if (!QFileInfo::exists(executable)) return {{"error", int(WorkspaceReadError::Unsupported)}, {"moveStatus", int(WorkspaceMoveStatus::Failed)}};
    extra["root"] = root; extra["identity"] = identity; extra["operation"] = operation; extra["path"] = path; extra["limit"] = limit;
    QProcess process; process.setProgram(executable); process.setProcessChannelMode(QProcess::SeparateChannels); process.start();
    if (!process.waitForStarted(3000)) return {{"error", int(WorkspaceReadError::Unsupported)}, {"moveStatus", int(WorkspaceMoveStatus::Failed)}};
    const auto input = QJsonDocument(extra).toJson(QJsonDocument::Compact);
    process.write(input); process.closeWriteChannel();
    QElapsedTimer elapsed; elapsed.start(); QByteArray response;
    while (process.state() != QProcess::NotRunning && elapsed.elapsed() < 15000) {
        process.waitForFinished(20); response += process.readAllStandardOutput();
        if (response.size() > 48 * 1024 * 1024) break;
    }
    if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(3000); }
    response += process.readAllStandardOutput();
    QJsonParseError error{}; const auto parsed = QJsonDocument::fromJson(response, &error);
    if (response.size() > 48 * 1024 * 1024 || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0
        || error.error != QJsonParseError::NoError || !parsed.isObject() || !parsed.object().contains("error")) {
        // A lost response after a write is uncertain, never a fabricated failure
        // with an implicit rollback or a reason to remove a published image.
        return {{"error", int(WorkspaceReadError::Io)}, {"status", int(WorkspaceWriteStatus::PublishedButUnverified)}, {"path", path}};
    }
    return parsed.object();
}
#endif

QString safeStem(const QString &suggestedName) {
    auto value = QFileInfo(QDir::fromNativeSeparators(suggestedName)).completeBaseName();
    if (QString::fromUtf8(value.toUtf8()) != value) value.clear();
    for (auto &ch : value) {
        if (ch.isNull() || ch.category() == QChar::Other_Control || QStringLiteral("<>:\"/\\|?*").contains(ch)) ch = '_';
    }
    value = value.trimmed();
    while (value.endsWith('.') || value.endsWith(' ')) value.chop(1);
    if (value.size() > 80) { value.truncate(80); if (!value.isEmpty() && value.back().isHighSurrogate()) value.chop(1); }
    while (value.endsWith('.') || value.endsWith(' ')) value.chop(1);
    if (value.isEmpty() || value == "." || value == "..") value = QStringLiteral("image");
    const auto reserved = value.section('.', 0, 0).toUpper();
    if (reserved == "CON" || reserved == "PRN" || reserved == "AUX" || reserved == "NUL" || reserved == "CONIN$" || reserved == "CONOUT$"
        || ((reserved.startsWith("COM") || reserved.startsWith("LPT")) && reserved.size() == 4
            && ((reserved.at(3) >= '1' && reserved.at(3) <= '9')
                || reserved.at(3) == QChar(u'\u00b9') || reserved.at(3) == QChar(u'\u00b2') || reserved.at(3) == QChar(u'\u00b3')))) value.prepend('_');
    return value;
}

#ifdef Q_OS_WIN
struct NativeHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~NativeHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    NativeHandle() = default;
    NativeHandle(const NativeHandle &) = delete;
    NativeHandle &operator=(const NativeHandle &) = delete;
};

WorkspaceReadError nativeError(DWORD error) {
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return WorkspaceReadError::Missing;
    if (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION) return WorkspaceReadError::Denied;
    if (error == ERROR_CANT_ACCESS_FILE || error == ERROR_REPARSE_TAG_INVALID) return WorkspaceReadError::OutsideOrLink;
    return WorkspaceReadError::Io;
}

bool directoryInfo(HANDLE handle, FILE_ID_INFO &identity) {
    FILE_ATTRIBUTE_TAG_INFO attrs{};
    FILE_STANDARD_INFO standard{};
    return GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attrs, sizeof(attrs))
        && GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard))
        && GetFileInformationByHandleEx(handle, FileIdInfo, &identity, sizeof(identity))
        && !(attrs.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && standard.Directory
        && GetFileType(handle) == FILE_TYPE_DISK;
}

bool sameIdentity(const FILE_ID_INFO &left, const FILE_ID_INFO &right) {
    return left.VolumeSerialNumber == right.VolumeSerialNumber
        && std::memcmp(left.FileId.Identifier, right.FileId.Identifier, sizeof(left.FileId.Identifier)) == 0;
}

HANDLE openRoot(const QString &path, DWORD sharing, DWORD access = FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY) {
    const auto native = QDir::toNativeSeparators(path);
    return CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), access,
        sharing, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
}

NTSTATUS openRelative(HANDLE parent, const QString &part, ACCESS_MASK access, ULONG sharing,
                      ULONG disposition, ULONG options, HANDLE &opened) {
    UNICODE_STRING name{};
    name.Buffer = const_cast<PWSTR>(reinterpret_cast<LPCWSTR>(part.utf16()));
    name.Length = USHORT(part.size() * sizeof(wchar_t));
    name.MaximumLength = name.Length;
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(attributes);
    attributes.RootDirectory = parent;
    attributes.ObjectName = &name;
    attributes.Attributes = OBJ_CASE_INSENSITIVE;
    IO_STATUS_BLOCK status{};
    return NtCreateFile(&opened, access, &attributes, &status, nullptr, FILE_ATTRIBUTE_NORMAL,
                        sharing, disposition, options, nullptr, 0);
}

NTSTATUS openChild(HANDLE parent, const QString &part, bool directory, HANDLE &opened) {
    // No sharing of DELETE on any directory prevents a checked parent from
    // being moved or replaced before the final object has been read. Deny
    // WRITE on the file itself to obtain one bounded, stable byte snapshot.
    const ACCESS_MASK access = (directory ? FILE_LIST_DIRECTORY : GENERIC_READ) | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
    const ULONG sharing = directory ? FILE_SHARE_READ | FILE_SHARE_WRITE : FILE_SHARE_READ;
    const ULONG options = FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT
        | (directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE);
    return openRelative(parent, part, access, sharing, FILE_OPEN, options, opened);
}


bool directoryHandle(HANDLE handle) {
    FILE_ATTRIBUTE_TAG_INFO attrs{}; FILE_STANDARD_INFO standard{};
    return GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attrs, sizeof(attrs))
        && GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard))
        && !(attrs.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && standard.Directory
        && GetFileType(handle) == FILE_TYPE_DISK;
}

enum class Existing { Missing, Same, Occupied };
QString nativeIdentity(HANDLE handle) {
    FILE_ID_INFO info{};
    if (!GetFileInformationByHandleEx(handle, FileIdInfo, &info, sizeof(info))) return {};
    return QStringLiteral("win:%1:%2").arg(info.VolumeSerialNumber, 0, 16)
        .arg(QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(info.FileId.Identifier), 16).toHex()));
}
struct ExistingResult { Existing state; std::shared_ptr<NativeHandle> file; };
ExistingResult existingBytes(HANDLE directory, const QString &name, const QByteArray &bytes) {
    auto file = std::make_shared<NativeHandle>();
    const auto status = openChild(directory, name, false, file->value);
    if (status < 0) {
        const auto error = RtlNtStatusToDosError(status);
        return {error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? Existing::Missing : Existing::Occupied, {}};
    }
    FILE_ATTRIBUTE_TAG_INFO attrs{}; FILE_STANDARD_INFO info{};
    if (!GetFileInformationByHandleEx(file->value, FileAttributeTagInfo, &attrs, sizeof(attrs))
        || !GetFileInformationByHandleEx(file->value, FileStandardInfo, &info, sizeof(info))
        || (attrs.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || info.Directory
        || GetFileType(file->value) != FILE_TYPE_DISK || info.EndOfFile.QuadPart != bytes.size()) return {Existing::Occupied, {}};
    QByteArray current(bytes.size(), Qt::Uninitialized); qsizetype offset = 0;
    while (offset < current.size()) {
        DWORD count = 0; const auto chunk = DWORD(qMin<qsizetype>(current.size() - offset, 64 * 1024));
        if (!ReadFile(file->value, current.data() + offset, chunk, &count, nullptr) || count == 0) return {Existing::Occupied, {}};
        offset += count;
    }
    return current == bytes ? ExistingResult{Existing::Same, std::move(file)} : ExistingResult{Existing::Occupied, {}};
}

struct PublicationLease {
    NativeHandle root, assets, images, documentRoot;
    std::shared_ptr<NativeHandle> resource;
    std::vector<std::shared_ptr<NativeHandle>> documentPath;
};

bool lockDocument(PublicationLease &lease, const QString &path, const QString &identity) {
    const auto parts = components(path);
    if (parts.isEmpty()) return false;
    HANDLE parent = lease.documentRoot.value != INVALID_HANDLE_VALUE ? lease.documentRoot.value : lease.root.value;
    std::vector<std::shared_ptr<NativeHandle>> handles;
    for (qsizetype i = 0; i < parts.size(); ++i) {
        const bool directory = i + 1 < parts.size();
        auto handle = std::make_shared<NativeHandle>();
        if (openChild(parent, parts.at(i), directory, handle->value) < 0) return false;
        FILE_ATTRIBUTE_TAG_INFO attrs{}; FILE_STANDARD_INFO info{};
        if (!GetFileInformationByHandleEx(handle->value, FileAttributeTagInfo, &attrs, sizeof(attrs))
            || !GetFileInformationByHandleEx(handle->value, FileStandardInfo, &info, sizeof(info))
            || (attrs.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || bool(info.Directory) != directory
            || GetFileType(handle->value) != FILE_TYPE_DISK) return false;
        if (!directory) {
            FILE_ID_INFO actual{};
            if (!GetFileInformationByHandleEx(handle->value, FileIdInfo, &actual, sizeof(actual))) return false;
            const auto found = QStringLiteral("win:%1:%2").arg(actual.VolumeSerialNumber, 0, 16).arg(
                QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(actual.FileId.Identifier), 16).toHex()));
            if (found != identity || info.EndOfFile.QuadPart < 0 || info.EndOfFile.QuadPart > FileService::DiskByteLimit) return false;
        }
        parent = handle->value; handles.push_back(std::move(handle));
    }
    lease.documentPath = std::move(handles);
    return true;
}

bool deleteOwnedTemporary(NativeHandle &file) {
    FILE_DISPOSITION_INFO disposition{}; disposition.DeleteFile = TRUE;
    if (!SetFileInformationByHandle(file.value, FileDispositionInfo, &disposition, sizeof(disposition))) return false;
    CloseHandle(file.value); file.value = INVALID_HANDLE_VALUE;
    return true;
}
#else
struct NativeHandle {
    int value = -1;
    ~NativeHandle() { if (value >= 0) ::close(value); }
    NativeHandle() = default;
    NativeHandle(const NativeHandle &) = delete;
    NativeHandle &operator=(const NativeHandle &) = delete;
};

WorkspaceReadError nativeError(int error) {
    if (error == ENOENT || error == ENOTDIR) return WorkspaceReadError::Missing;
    if (error == EACCES || error == EPERM) return WorkspaceReadError::Denied;
    if (error == ELOOP || error == EXDEV) return WorkspaceReadError::OutsideOrLink;
    if (error == ENOSYS || error == EINVAL) return WorkspaceReadError::Unsupported;
    return WorkspaceReadError::Io;
}
#endif
}

struct WorkspaceResourceReader::State {
    QString path;
    NativeHandle root;
    std::atomic<bool> active{true};
    std::mutex commitMutex;
#ifdef Q_OS_WIN
    FILE_ID_INFO identity{};
#else
    dev_t device{};
    ino_t inode{};
#endif
};

WorkspaceResourceReader::WorkspaceResourceReader(std::unique_ptr<State> state) : m_state(std::move(state)) {}
WorkspaceResourceReader::~WorkspaceResourceReader() = default;
bool WorkspaceResourceReader::active() const { return m_state->active.load(std::memory_order_acquire); }
void WorkspaceResourceReader::revoke() {
    std::lock_guard<std::mutex> lock(m_state->commitMutex);
    m_state->active.store(false, std::memory_order_release);
}

WorkspaceRootResult WorkspaceResourceReader::open(const QString &absoluteRoot) {
    if (!QDir::isAbsolutePath(absoluteRoot) || absoluteRoot.contains(QChar::Null)) return {{}, WorkspaceReadError::InvalidPath};
#if !defined(Q_OS_WIN) && !defined(Q_OS_LINUX) && !defined(Q_OS_MACOS)
    return {{}, WorkspaceReadError::Unsupported};
#else
    auto state = std::make_unique<State>();
    state->path = QDir::cleanPath(absoluteRoot);
#ifdef Q_OS_WIN
    state->root.value = openRoot(state->path, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
    if (state->root.value == INVALID_HANDLE_VALUE) { const DWORD code = GetLastError(); return {{}, nativeError(code), code}; }
    if (!directoryInfo(state->root.value, state->identity)) return {{}, WorkspaceReadError::OutsideOrLink};
#else
    const auto path = QFile::encodeName(state->path);
    state->root.value = ::open(path.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (state->root.value < 0) { const int code = errno; return {{}, nativeError(code), code}; }
    struct stat info{};
    if (::fstat(state->root.value, &info) != 0 || !S_ISDIR(info.st_mode)) return {{}, WorkspaceReadError::Unsupported};
    state->device = info.st_dev; state->inode = info.st_ino;
#endif
    return {std::shared_ptr<WorkspaceResourceReader>(new WorkspaceResourceReader(std::move(state)))};
#endif
}

WorkspaceDirectoryResult WorkspaceResourceReader::listDirectory(const QString &path, int maxEntries) const {
    if (!active()) return {WorkspaceReadError::Revoked};
    const auto parts = components(path);
    if ((!path.isEmpty() && parts.isEmpty()) || maxEntries < 1 || maxEntries > 10000)
        return {WorkspaceReadError::InvalidPath};
    WorkspaceDirectoryResult result;
#ifdef Q_OS_WIN
    NativeHandle lockedRoot;
    lockedRoot.value = openRoot(m_state->path, FILE_SHARE_READ | FILE_SHARE_WRITE);
    if (lockedRoot.value == INVALID_HANDLE_VALUE) { const auto code = GetLastError(); return {nativeError(code), code}; }
    FILE_ID_INFO rootIdentity{};
    if (!directoryInfo(lockedRoot.value, rootIdentity) || !sameIdentity(rootIdentity, m_state->identity))
        return {WorkspaceReadError::RootChanged};
    std::vector<std::unique_ptr<NativeHandle>> opened;
    HANDLE directory = lockedRoot.value;
    for (const auto &part : parts) {
        auto handle = std::make_unique<NativeHandle>();
        const auto status = openChild(directory, part, true, handle->value);
        if (status < 0) { const auto code = RtlNtStatusToDosError(status); return {nativeError(code), code}; }
        FILE_ID_INFO identity{};
        if (!directoryInfo(handle->value, identity)) return {WorkspaceReadError::OutsideOrLink};
        directory = handle->value; opened.push_back(std::move(handle));
    }
    FILE_ID_INFO identity{}; FILE_BASIC_INFO before{}, after{};
    if (!directoryInfo(directory, identity) || !GetFileInformationByHandleEx(directory, FileBasicInfo, &before, sizeof(before)))
        return {WorkspaceReadError::Io, GetLastError()};
    result.identity = QStringLiteral("win:%1:%2").arg(identity.VolumeSerialNumber, 0, 16).arg(
        QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(identity.FileId.Identifier), 16).toHex()));
    alignas(FILE_ID_BOTH_DIR_INFO) std::array<unsigned char, 64 * 1024> buffer{};
    bool restart = true, limited = false;
    while (!limited) {
        if (!active()) return {WorkspaceReadError::Revoked};
        buffer.fill(0);
        if (!GetFileInformationByHandleEx(directory, restart ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo,
                                         buffer.data(), DWORD(buffer.size()))) {
            const auto code = GetLastError();
            if (code == ERROR_NO_MORE_FILES) { result.complete = true; break; }
            return {nativeError(code), code};
        }
        restart = false;
        std::size_t offset = 0;
        for (;;) {
            constexpr auto header = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
            if (offset > buffer.size() - header) return {WorkspaceReadError::Io};
            const auto *entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO *>(buffer.data() + offset);
            if (entry->FileNameLength == 0 || entry->FileNameLength % sizeof(wchar_t)
                || entry->FileNameLength > buffer.size() - offset - header) return {WorkspaceReadError::Io};
            const auto name = QString::fromWCharArray(entry->FileName, int(entry->FileNameLength / sizeof(wchar_t)));
            if (name != "." && name != "..") {
                if (result.entries.size() == maxEntries) { limited = true; break; }
                auto kind = WorkspaceDirectoryEntry::Kind::Unsupported;
                if (!components(name).isEmpty() && !(entry->FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)))
                    kind = entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY ? WorkspaceDirectoryEntry::Kind::Directory : WorkspaceDirectoryEntry::Kind::File;
                result.entries.append({name, kind});
            }
            if (!entry->NextEntryOffset) break;
            if (entry->NextEntryOffset < header + entry->FileNameLength || entry->NextEntryOffset % alignof(FILE_ID_BOTH_DIR_INFO)
                || entry->NextEntryOffset > buffer.size() - offset - header) return {WorkspaceReadError::Io};
            offset += entry->NextEntryOffset;
        }
    }
    if (!GetFileInformationByHandleEx(directory, FileBasicInfo, &after, sizeof(after))) return {WorkspaceReadError::Io, GetLastError()};
    if (before.LastWriteTime.QuadPart != after.LastWriteTime.QuadPart || before.ChangeTime.QuadPart != after.ChangeTime.QuadPart)
        return {WorkspaceReadError::Changed};
#elif defined(Q_OS_LINUX) && defined(SYS_openat2)
    NativeHandle currentRoot;
    const auto rootPath = QFile::encodeName(m_state->path);
    currentRoot.value = ::open(rootPath.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (currentRoot.value < 0) { const int code = errno; return {nativeError(code), code}; }
    struct stat rootInfo{};
    if (::fstat(currentRoot.value, &rootInfo) != 0 || rootInfo.st_dev != m_state->device || rootInfo.st_ino != m_state->inode)
        return {WorkspaceReadError::RootChanged};
    const auto nativePath = QFile::encodeName(path.isEmpty() ? QStringLiteral(".") : path);
    open_how how{};
    how.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_XDEV;
    NativeHandle directory;
    directory.value = int(::syscall(SYS_openat2, currentRoot.value, nativePath.constData(), &how, sizeof(how)));
    if (directory.value < 0) { const int code = errno; return {nativeError(code), code}; }
    struct stat before{}, after{};
    if (::fstat(directory.value, &before) != 0) return {WorkspaceReadError::Io, errno};
    result.identity = QStringLiteral("posix:%1:%2").arg(qulonglong(before.st_dev), 0, 16).arg(qulonglong(before.st_ino), 0, 16);
    DIR *raw = ::fdopendir(directory.value);
    if (!raw) return {WorkspaceReadError::Io, errno};
    directory.value = -1;
    const auto stream = std::unique_ptr<DIR, decltype(&::closedir)>(raw, &::closedir);
    for (;;) {
        if (!active()) return {WorkspaceReadError::Revoked};
        errno = 0; const auto *entry = ::readdir(stream.get());
        if (!entry) {
            if (errno) return {WorkspaceReadError::Io, errno};
            result.complete = true; break;
        }
        const QByteArray nativeName(entry->d_name);
        if (nativeName == "." || nativeName == "..") continue;
        if (result.entries.size() == maxEntries) break;
        const auto name = QFile::decodeName(nativeName);
        auto kind = WorkspaceDirectoryEntry::Kind::Unsupported;
        if (QFile::encodeName(name) == nativeName && !components(name).isEmpty()) {
            struct stat info{};
            if (::fstatat(::dirfd(stream.get()), entry->d_name, &info, AT_SYMLINK_NOFOLLOW) != 0)
                return {nativeError(errno), errno};
            if (info.st_dev == m_state->device && !S_ISLNK(info.st_mode)) {
                if (S_ISDIR(info.st_mode)) kind = WorkspaceDirectoryEntry::Kind::Directory;
                else if (S_ISREG(info.st_mode)) kind = WorkspaceDirectoryEntry::Kind::File;
            }
        }
        result.entries.append({name, kind});
    }
    if (::fstat(::dirfd(stream.get()), &after) != 0) return {WorkspaceReadError::Io, errno};
    if (before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec
        || before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
        return {WorkspaceReadError::Changed};
    NativeHandle verifyRoot;
    verifyRoot.value = ::open(rootPath.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat verified{};
    if (verifyRoot.value < 0 || ::fstat(verifyRoot.value, &verified) != 0
        || verified.st_dev != m_state->device || verified.st_ino != m_state->inode) return {WorkspaceReadError::RootChanged};
#elif defined(Q_OS_MACOS)
    const auto rootIdentity = QStringLiteral("posix:%1:%2").arg(qulonglong(m_state->device), 0, 16).arg(qulonglong(m_state->inode), 0, 16);
    const auto response = helperRequest(m_state->path, rootIdentity, "list", path, maxEntries);
    if (!active()) return {WorkspaceReadError::Revoked};
    const auto error = WorkspaceReadError(response.value("error").toInt(int(WorkspaceReadError::Io)));
    if (error != WorkspaceReadError::None) return {error, response.value("native").toInteger()};
    result.identity = response.value("identity").toString();
    for (const auto value : response.value("entries").toArray()) {
        const auto entry = value.toObject(); result.entries.append({entry.value("name").toString(), WorkspaceDirectoryEntry::Kind(entry.value("kind").toInt(2))});
    }
    result.complete = response.value("complete").toBool();
    return result;
#else
    return {WorkspaceReadError::Unsupported};
#endif
    if (!active()) return {WorkspaceReadError::Revoked};
    return result;
}

WorkspaceReadResult WorkspaceResourceReader::read(const QString &path, qsizetype maxBytes) const {
    return readImpl(path, maxBytes, false);
}
WorkspaceReadResult WorkspaceResourceReader::inspect(const QString &path) const { return readImpl(path, 0, true); }
QString WorkspaceResourceReader::rootPath() const { return m_state->path; }
WorkspaceEntry WorkspaceResourceReader::entry(const QString &path) const {
    if (components(path).isEmpty()) return {WorkspaceReadError::InvalidPath};
    auto file = inspect(path);
    if (file.ok()) return {WorkspaceReadError::None, file.identity, path, false, std::move(file.lease)};
    const auto directory = listDirectory(path, 1);
    if (directory.ok()) return {WorkspaceReadError::None, directory.identity, path, true};
    for (const auto error : {WorkspaceReadError::Revoked, WorkspaceReadError::RootChanged, WorkspaceReadError::OutsideOrLink, WorkspaceReadError::Denied, WorkspaceReadError::Unsupported})
        if (file.error == error || directory.error == error) return {error};
    return {directory.error};
}

FileSaveResult WorkspaceResourceReader::saveDocument(const QString &path, const FileSaveRequest &request,
    const std::function<void(SaveStage)> &observer) const {
    FileSaveResult result; result.documentId = request.documentId; result.revision = request.revision; result.requestId = request.requestId;
    if (components(path).isEmpty() || !request.baseline || request.path != QDir(rootPath()).filePath(path)) {
        result.problem = {FileError::InvalidPath}; return result;
    }
    const auto pinned = inspect(path);
    if (!pinned.ok() || pinned.identity != request.baseline->file()->identity()) {
        result.status = SaveStatus::Conflict; result.problem = {FileError::Changed}; return result;
    }
#ifdef Q_OS_WIN
    NativeHandle root;
    const auto nativeRoot = QDir::toNativeSeparators(m_state->path);
    root.value = CreateFileW(reinterpret_cast<LPCWSTR>(nativeRoot.utf16()), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    FILE_ID_INFO rootId{};
    if (!directoryInfo(root.value, rootId) || !sameIdentity(rootId, m_state->identity)) { result.problem = {FileError::Changed}; return result; }
    std::vector<std::unique_ptr<NativeHandle>> parents; HANDLE parent = root.value;
    const auto parts = components(path);
    for (qsizetype i = 0; i + 1 < parts.size(); ++i) {
        auto child = std::make_unique<NativeHandle>(); FILE_ID_INFO id{};
        if (openChild(parent, parts.at(i), true, child->value) < 0 || !directoryInfo(child->value, id)) { result.problem = {FileError::Changed}; return result; }
        parent = child->value; parents.push_back(std::move(child));
    }
    // These parent handles exclude directory replacement through the save.
    // The ordinary save engine retains conflict and hard-link protections.
    return FileService::save(request, [&](SaveStage stage) {
        if (!active()) throw std::runtime_error("workspace revoked");
        if (observer) observer(stage);
        const auto current = inspect(path);
        if (!current.ok() || (stage != SaveStage::Verify && current.identity != pinned.identity))
            throw std::runtime_error("workspace entry changed");
    });
#else
    try {
        if (observer) { observer(SaveStage::Validate); observer(SaveStage::StageWrite); observer(SaveStage::Commit); }
    } catch (...) { result.problem = {FileError::Io}; return result; }
    QJsonObject response;
    {
        std::lock_guard<std::mutex> lock(m_state->commitMutex);
        if (!active()) { result.problem = {FileError::Denied}; return result; }
        const auto rootIdentity = QStringLiteral("posix:%1:%2").arg(qulonglong(m_state->device), 0, 16).arg(qulonglong(m_state->inode), 0, 16);
        response = helperRequest(m_state->path, rootIdentity, "save", path, 0,
            {{"expected", pinned.identity}, {"sha256", QString::fromLatin1(request.baseline->sha256().toHex())},
             {"bytes", QString::fromLatin1(request.utf8.toBase64())}, {"bom", request.bom}});
    }
    if (!response.contains("saveStatus")) {
        result.status = response.contains("moveStatus") ? SaveStatus::Failed : SaveStatus::Uncertain;
        result.problem = {FileError::Changed, qint64(response.value("native").toDouble())}; return result;
    }
    result.status = SaveStatus(response.value("saveStatus").toInt()); result.stage = SaveStage(response.value("saveStage").toInt());
    result.problem = {FileError(response.value("fileError").toInt()), qint64(response.value("native").toDouble())};
    result.recoveryPath = response.value("recovery").toString(); result.stagedPath = response.value("staged").toString();
    if (result.status != SaveStatus::Committed) return result;
    try { if (observer) observer(SaveStage::Verify); } catch (...) { result.status = SaveStatus::Uncertain; result.problem = {FileError::Io}; return result; }
    auto borrowed = std::shared_ptr<WorkspaceResourceReader>(const_cast<WorkspaceResourceReader *>(this), [](auto *) {});
    const auto probe = FileService::probeWithin(borrowed, path);
    const auto read = probe.file ? FileService::read({probe.file, request.requestId}) : ReadResult{};
    if (!active() || !read.baseline || read.baseline->file()->identity() != response.value("savedIdentity").toString()
        || read.baseline->utf8() != request.utf8 || read.baseline->hasUtf8Bom() != request.bom) {
        result.status = SaveStatus::Uncertain; result.problem = {FileError::Changed}; return result;
    }
    result.baseline = read.baseline; return result;
#endif
}

WorkspaceMoveResult WorkspaceResourceReader::moveEntry(const QString &from, const QString &to, const QString &identity, bool directory) const {
    const auto sourceParts = components(from), targetParts = components(to);
    if (sourceParts.isEmpty() || targetParts.isEmpty() || from == to || identity.isEmpty()
        || (directory && to.startsWith(from + '/'))) return {WorkspaceMoveStatus::Failed, WorkspaceReadError::InvalidPath};
    if (!active()) return {WorkspaceMoveStatus::Failed, WorkspaceReadError::Revoked};
    const auto before = entry(from);
    if (!before.ok()) return {WorkspaceMoveStatus::Failed, before.error};
    if (before.identity != identity || before.directory != directory) return {WorkspaceMoveStatus::Failed, WorkspaceReadError::Changed};
    auto sourceParent = QFileInfo(from).path(); if (sourceParent == ".") sourceParent.clear();
    auto targetParent = QFileInfo(to).path(); if (targetParent == ".") targetParent.clear();
    const auto existing = entry(to);
    if (existing.ok()) {
        const auto sourceDirectory = listDirectory(sourceParent, 10000), targetDirectory = listDirectory(targetParent, 1);
        const auto folded = [](const QString &name) { return name.normalized(QString::NormalizationForm_D).toCaseFolded(); };
        const auto sourceName = sourceParts.last(), targetName = targetParts.last(); int equivalents = 0; bool exact = false;
        for (const auto &item : sourceDirectory.entries) { if (item.name == targetName) exact = true; if (folded(item.name) == folded(targetName)) ++equivalents; }
        if (existing.identity != identity || !sourceDirectory.ok() || !sourceDirectory.complete || !targetDirectory.ok()
            || sourceDirectory.identity != targetDirectory.identity || exact || equivalents != 1 || folded(sourceName) != folded(targetName))
            return {WorkspaceMoveStatus::Failed, WorkspaceReadError::Changed};
        // A case/normalization alias on this actual filesystem is handled as
        // two no-replace moves. Distinct hard-link entries are never replaced.
        const auto temporaryName = QStringLiteral(".xdnote-rename-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        const auto temporary = sourceParent.isEmpty() ? temporaryName : sourceParent + '/' + temporaryName;
        auto staged = moveEntry(from, temporary, identity, directory);
        if (staged.status != WorkspaceMoveStatus::Moved) return staged;
        auto committed = moveEntry(temporary, to, identity, directory);
        if (committed.status == WorkspaceMoveStatus::Moved) return committed;
        return {WorkspaceMoveStatus::Uncertain, committed.error, committed.nativeCode, temporary, temporary, staged.lease};
    }
#ifdef Q_OS_WIN
    auto root = std::make_shared<NativeHandle>(); root->value = openRoot(m_state->path, FILE_SHARE_READ | FILE_SHARE_WRITE);
    FILE_ID_INFO actualRoot{};
    if (root->value == INVALID_HANDLE_VALUE || !directoryInfo(root->value, actualRoot) || !sameIdentity(actualRoot, m_state->identity))
        return {WorkspaceMoveStatus::Failed, WorkspaceReadError::RootChanged};
    std::vector<std::shared_ptr<NativeHandle>> anchors{root};
    const auto parentHandle = [&](const QStringList &parts, bool writing) -> HANDLE {
        HANDLE parent = root->value;
        if (parts.size() == 1 && writing) {
            auto handle = std::make_shared<NativeHandle>(); handle->value = openRoot(m_state->path, FILE_SHARE_READ | FILE_SHARE_WRITE,
                FILE_LIST_DIRECTORY | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_READ_ATTRIBUTES);
            FILE_ID_INFO info{};
            if (handle->value == INVALID_HANDLE_VALUE || !directoryInfo(handle->value, info) || !sameIdentity(info, m_state->identity)) return INVALID_HANDLE_VALUE;
            anchors.push_back(handle); return handle->value;
        }
        for (qsizetype i = 0; i + 1 < parts.size(); ++i) {
            auto handle = std::make_shared<NativeHandle>();
            const auto access = FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE
                | (writing && i + 2 == parts.size() ? FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY : 0);
            const auto status = openRelative(parent, parts.at(i), access, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
                FILE_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT, handle->value);
            if (status < 0 || !directoryHandle(handle->value)) return INVALID_HANDLE_VALUE;
            parent = handle->value; anchors.push_back(std::move(handle));
        }
        return parent;
    };
    const auto oldParent = parentHandle(sourceParts, false), newParent = parentHandle(targetParts, true);
    if (oldParent == INVALID_HANDLE_VALUE || newParent == INVALID_HANDLE_VALUE) return {WorkspaceMoveStatus::Failed, WorkspaceReadError::Denied};
    auto source = std::make_shared<NativeHandle>();
    auto status = openRelative(oldParent, sourceParts.last(), DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
        FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT | (directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE), source->value);
    if (status < 0) { const auto code = RtlNtStatusToDosError(status); return {WorkspaceMoveStatus::Failed, nativeError(code), code}; }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(source->value, FileAttributeTagInfo, &attributes, sizeof(attributes))
        || (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || nativeIdentity(source->value) != identity)
        return {WorkspaceMoveStatus::Failed, WorkspaceReadError::Changed};
    struct RenameRequest { IO_STATUS_BLOCK io{}; std::vector<quint64> bytes; std::shared_ptr<NativeHandle> source; std::vector<std::shared_ptr<NativeHandle>> anchors; };
    auto pending = std::make_unique<RenameRequest>(); pending->source = source; pending->anchors = anchors;
    const auto name = targetParts.last(); const auto size = offsetof(FILE_RENAME_INFO, FileName) + size_t(name.size()) * sizeof(wchar_t);
    pending->bytes.resize((size + sizeof(quint64) - 1) / sizeof(quint64));
    auto *rename = reinterpret_cast<FILE_RENAME_INFO *>(pending->bytes.data());
    rename->ReplaceIfExists = FALSE; rename->RootDirectory = newParent; rename->FileNameLength = DWORD(name.size() * sizeof(wchar_t));
    std::memcpy(rename->FileName, name.utf16(), rename->FileNameLength);
    {
        std::lock_guard<std::mutex> lock(m_state->commitMutex);
        if (!active()) return {WorkspaceMoveStatus::Failed, WorkspaceReadError::Revoked};
        status = NtSetInformationFile(source->value, &pending->io, rename, ULONG(size), static_cast<FILE_INFORMATION_CLASS>(10));
        if (status == 0x103) {
            if (WaitForSingleObject(source->value, 5000) != WAIT_OBJECT_0) {
                pending.release(); return {WorkspaceMoveStatus::Uncertain, WorkspaceReadError::Io, 0, {}, to};
            }
            status = pending->io.Status;
        }
    }
    if (status < 0) { const auto code = RtlNtStatusToDosError(status); return {WorkspaceMoveStatus::Failed, nativeError(code), code}; }
    auto lease = std::make_shared<NativeHandle>();
    const auto verification = openRelative(newParent, targetParts.last(), FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
        FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT | (directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE), lease->value);
    if (verification < 0 || nativeIdentity(lease->value) != identity || !active()) return {WorkspaceMoveStatus::Uncertain, WorkspaceReadError::Changed, 0, {}, to};
    return {WorkspaceMoveStatus::Moved, WorkspaceReadError::None, 0, to, {}, lease};
#else
    QJsonObject response;
    {
        std::lock_guard<std::mutex> lock(m_state->commitMutex);
        if (!active()) return {WorkspaceMoveStatus::Failed, WorkspaceReadError::Revoked};
        const auto rootIdentity = QStringLiteral("posix:%1:%2").arg(qulonglong(m_state->device), 0, 16).arg(qulonglong(m_state->inode), 0, 16);
        response = helperRequest(m_state->path, rootIdentity, "move", from, 0,
            {{"to", to}, {"expected", identity}, {"directory", directory}});
    }
    const auto state = WorkspaceMoveStatus(response.value("moveStatus").toInt(int(WorkspaceMoveStatus::Uncertain)));
    const auto error = WorkspaceReadError(response.value("error").toInt(int(WorkspaceReadError::Io)));
    if (state == WorkspaceMoveStatus::Moved && !active()) return {WorkspaceMoveStatus::Uncertain, WorkspaceReadError::Revoked, 0, to, to};
    return {state, error, response.value("native").toInteger(), response.value("actual").toString(), response.value("recovery").toString()};
#endif
}
WorkspaceReadResult WorkspaceResourceReader::readImpl(const QString &path, qsizetype maxBytes, bool metadataOnly) const {
    if (!m_state->active.load(std::memory_order_acquire)) return {WorkspaceReadError::Revoked};
    const auto parts = components(path);
    if (parts.isEmpty() || maxBytes < 0 || maxBytes > MaxReadBytes) return {WorkspaceReadError::InvalidPath};
#ifdef Q_OS_WIN
    NativeHandle lockedRoot;
    lockedRoot.value = openRoot(m_state->path, FILE_SHARE_READ | FILE_SHARE_WRITE);
    if (lockedRoot.value == INVALID_HANDLE_VALUE) { const DWORD code = GetLastError(); return {nativeError(code), {}, code}; }
    FILE_ID_INFO actualRoot{};
    if (!directoryInfo(lockedRoot.value, actualRoot) || !sameIdentity(actualRoot, m_state->identity)) return {WorkspaceReadError::RootChanged};
    std::vector<std::unique_ptr<NativeHandle>> opened;
    HANDLE parent = lockedRoot.value;
    for (qsizetype index = 0; index < parts.size(); ++index) {
        const bool directory = index + 1 < parts.size();
        auto handle = std::make_unique<NativeHandle>();
        const auto status = metadataOnly && !directory
            ? openRelative(parent, parts.at(index), FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT | FILE_NON_DIRECTORY_FILE, handle->value)
            : openChild(parent, parts.at(index), directory, handle->value);
        if (status < 0) { const DWORD code = RtlNtStatusToDosError(status); return {nativeError(code), {}, code}; }
        FILE_ATTRIBUTE_TAG_INFO attrs{}; FILE_STANDARD_INFO standard{};
        if (!GetFileInformationByHandleEx(handle->value, FileAttributeTagInfo, &attrs, sizeof(attrs))
            || !GetFileInformationByHandleEx(handle->value, FileStandardInfo, &standard, sizeof(standard))) return {WorkspaceReadError::Io, {}, GetLastError()};
        if ((attrs.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || bool(standard.Directory) != directory
            || GetFileType(handle->value) != FILE_TYPE_DISK) return {WorkspaceReadError::OutsideOrLink};
        parent = handle->value;
        opened.push_back(std::move(handle));
    }
    FILE_STANDARD_INFO info{};
    if (!GetFileInformationByHandleEx(parent, FileStandardInfo, &info, sizeof(info))) return {WorkspaceReadError::Io, {}, GetLastError()};
    FILE_ID_INFO fileIdentity{};
    if (!GetFileInformationByHandleEx(parent, FileIdInfo, &fileIdentity, sizeof(fileIdentity))) return {WorkspaceReadError::Io, {}, GetLastError()};
    const auto identity = QStringLiteral("win:%1:%2").arg(fileIdentity.VolumeSerialNumber, 0, 16).arg(
        QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(fileIdentity.FileId.Identifier), 16).toHex()));
    if (metadataOnly) {
        if (!active()) return {WorkspaceReadError::Revoked};
        std::shared_ptr<NativeHandle> lease(std::move(opened.back()));
        FILE_ID_DESCRIPTOR description{}; description.dwSize = sizeof(description); description.Type = ExtendedFileIdType; description.ExtendedFileId = fileIdentity.FileId;
        auto identityLease = std::make_shared<NativeHandle>();
        identityLease->value = OpenFileById(lockedRoot.value, &description, FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT);
        if (identityLease->value != INVALID_HANDLE_VALUE && nativeIdentity(identityLease->value) == identity) lease = std::move(identityLease);
        return {WorkspaceReadError::None, {}, 0, identity, std::move(lease)};
    }
    if (info.EndOfFile.QuadPart < 0 || info.EndOfFile.QuadPart > maxBytes) return {WorkspaceReadError::TooLarge};
    QByteArray bytes(qsizetype(info.EndOfFile.QuadPart), Qt::Uninitialized);
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        DWORD count = 0;
        const auto chunk = DWORD(qMin<qsizetype>(bytes.size() - offset, 64 * 1024));
        if (!ReadFile(parent, bytes.data() + offset, chunk, &count, nullptr)) return {WorkspaceReadError::Io, {}, GetLastError()};
        if (count == 0) return {WorkspaceReadError::Changed};
        offset += count;
    }
#elif defined(Q_OS_LINUX) && defined(SYS_openat2)
    NativeHandle currentRoot;
    const auto rootPath = QFile::encodeName(m_state->path);
    currentRoot.value = ::open(rootPath.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (currentRoot.value < 0) { const int code = errno; return {nativeError(code), {}, code}; }
    struct stat rootInfo{};
    if (::fstat(currentRoot.value, &rootInfo) != 0 || rootInfo.st_dev != m_state->device || rootInfo.st_ino != m_state->inode)
        return {WorkspaceReadError::RootChanged};
    const auto nativePath = QFile::encodeName(path);
    open_how how{};
    how.flags = (metadataOnly ? O_PATH : O_RDONLY | O_NONBLOCK) | O_CLOEXEC | O_NOFOLLOW;
    // Bind mounts are not symlinks: without NO_XDEV they could expose an
    // unrelated directory under the authorized root.
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_XDEV;
    NativeHandle file;
    file.value = int(::syscall(SYS_openat2, currentRoot.value, nativePath.constData(), &how, sizeof(how)));
    if (file.value < 0) { const int code = errno; return {nativeError(code), {}, code}; }
    struct stat before{}, after{};
    if (::fstat(file.value, &before) != 0) return {WorkspaceReadError::Io, {}, errno};
    if (!S_ISREG(before.st_mode)) return {WorkspaceReadError::OutsideOrLink};
    const auto identity = QStringLiteral("posix:%1:%2").arg(qulonglong(before.st_dev), 0, 16).arg(qulonglong(before.st_ino), 0, 16);
    if (metadataOnly) {
        NativeHandle verifyRoot;
        verifyRoot.value = ::open(rootPath.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        struct stat verified{};
        if (verifyRoot.value < 0 || ::fstat(verifyRoot.value, &verified) != 0
            || verified.st_dev != m_state->device || verified.st_ino != m_state->inode) return {WorkspaceReadError::RootChanged};
        if (!active()) return {WorkspaceReadError::Revoked};
        auto lease = std::make_shared<NativeHandle>(); lease->value = file.value; file.value = -1;
        return {WorkspaceReadError::None, {}, 0, identity, std::move(lease)};
    }
    if (before.st_size < 0 || before.st_size > maxBytes) return {WorkspaceReadError::TooLarge};
    QByteArray bytes(qsizetype(before.st_size), Qt::Uninitialized);
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::read(file.value, bytes.data() + offset, size_t(bytes.size() - offset));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return {WorkspaceReadError::Io, {}, errno};
        if (count == 0) return {WorkspaceReadError::Changed};
        offset += count;
    }
    if (::fstat(file.value, &after) != 0) return {WorkspaceReadError::Io, {}, errno};
    if (before.st_size != after.st_size || before.st_mtim.tv_sec != after.st_mtim.tv_sec
        || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec || before.st_ctim.tv_sec != after.st_ctim.tv_sec
        || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) return {WorkspaceReadError::Changed};
    NativeHandle verifyRoot;
    verifyRoot.value = ::open(rootPath.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat verified{};
    if (verifyRoot.value < 0 || ::fstat(verifyRoot.value, &verified) != 0
        || verified.st_dev != m_state->device || verified.st_ino != m_state->inode) return {WorkspaceReadError::RootChanged};
#elif defined(Q_OS_MACOS)
    const auto rootIdentity = QStringLiteral("posix:%1:%2").arg(qulonglong(m_state->device), 0, 16).arg(qulonglong(m_state->inode), 0, 16);
    const auto response = helperRequest(m_state->path, rootIdentity, metadataOnly ? "inspect" : "read", path, int(maxBytes));
    if (!active()) return {WorkspaceReadError::Revoked};
    const auto error = WorkspaceReadError(response.value("error").toInt(int(WorkspaceReadError::Io)));
    if (error != WorkspaceReadError::None) return {error, {}, response.value("native").toInteger()};
    WorkspaceReadResult result; result.identity = response.value("identity").toString();
    if (metadataOnly) {
        // Hold the already authorized object's identity against inode reuse.
        // These parent-process opens request events/metadata, never file data;
        // all content reads stay inside the restricted helper.
        NativeHandle parent; parent.value = ::dup(m_state->root.value);
        for (qsizetype i = 0; i < parts.size(); ++i) {
            const bool directory = i + 1 < parts.size();
            const int child = ::openat(parent.value, parts.at(i).toUtf8().constData(), O_CLOEXEC | O_NOFOLLOW
                | (directory ? O_RDONLY | O_DIRECTORY : O_EVTONLY));
            if (child < 0) return {nativeError(errno), {}, errno};
            ::close(parent.value); parent.value = child;
        }
        struct stat actual{};
        if (::fstat(parent.value, &actual) != 0 || QStringLiteral("posix:%1:%2").arg(qulonglong(actual.st_dev), 0, 16)
            .arg(qulonglong(actual.st_ino), 0, 16) != result.identity) return {WorkspaceReadError::Changed};
        auto lease = std::make_shared<NativeHandle>(); lease->value = parent.value; parent.value = -1; result.lease = std::move(lease);
    } else result.bytes = QByteArray::fromBase64(response.value("bytes").toString().toLatin1());
    return result;
#else
    return {WorkspaceReadError::Unsupported};
#endif
#if defined(Q_OS_WIN) || (defined(Q_OS_LINUX) && defined(SYS_openat2))
    if (!m_state->active.load(std::memory_order_acquire)) return {WorkspaceReadError::Revoked};
    return {WorkspaceReadError::None, std::move(bytes), 0, identity};
#endif
}

WorkspaceWriteResult WorkspaceResourceReader::publishImage(const QString &documentPath, const QString &documentIdentity,
    const QByteArray &bytes, const QString &suggestedName, const QString &extension,
    const std::function<void(WorkspaceWriteStage)> &observer, const QString &destination,
    const std::shared_ptr<WorkspaceResourceReader> &documentRoot) const {
    const auto &sourceReader = documentRoot ? *documentRoot : *this;
    if (bytes.isEmpty() || bytes.size() > 8 * 1024 * 1024) return {WorkspaceWriteStatus::Failed, WorkspaceReadError::TooLarge};
    if ((!destination.isEmpty() && destination != "assets/images") || !active() || !sourceReader.active())
        return {WorkspaceWriteStatus::Failed, WorkspaceReadError::Revoked};
#ifndef Q_OS_WIN
    const auto source = sourceReader.inspect(documentPath);
    if (!source.ok() || source.identity != documentIdentity) return {WorkspaceWriteStatus::Failed, WorkspaceReadError::Changed};
    if (observer) { try { observer(WorkspaceWriteStage::BeforeCommit); } catch (...) { return {WorkspaceWriteStatus::Failed, WorkspaceReadError::Io}; } }
    QJsonObject response;
    {
        std::lock_guard<std::mutex> lock(m_state->commitMutex);
        if (!active() || !sourceReader.active()) return {WorkspaceWriteStatus::Failed, WorkspaceReadError::Revoked};
        const auto current = sourceReader.inspect(documentPath);
        if (!current.ok() || current.identity != documentIdentity) return {WorkspaceWriteStatus::Failed, WorkspaceReadError::Changed};
        const auto rootIdentity = QStringLiteral("posix:%1:%2").arg(qulonglong(m_state->device), 0, 16).arg(qulonglong(m_state->inode), 0, 16);
        response = helperRequest(m_state->path, rootIdentity, "publish", destination, 0, {{"bytes", QString::fromLatin1(bytes.toBase64())}, {"stem", safeStem(suggestedName)}, {"extension", extension.toLower()}});
    }
    const auto status = WorkspaceWriteStatus(response.value("status").toInt(int(WorkspaceWriteStatus::Failed)));
    const auto path = response.value("path").toString();
    if (observer && (status == WorkspaceWriteStatus::Committed || status == WorkspaceWriteStatus::Reused)) {
        try { observer(WorkspaceWriteStage::AfterCommit); } catch (...) { return {WorkspaceWriteStatus::PublishedButUnverified, WorkspaceReadError::Io, 0, path}; }
    }
    if (status == WorkspaceWriteStatus::Committed || status == WorkspaceWriteStatus::Reused) {
        const auto checked = sourceReader.inspect(documentPath);
        if (!active() || !sourceReader.active() || !checked.ok() || checked.identity != documentIdentity)
            return {WorkspaceWriteStatus::PublishedButUnverified, WorkspaceReadError::Changed, 0, path};
    }
    return {status, WorkspaceReadError(response.value("error").toInt(int(WorkspaceReadError::Io))), response.value("native").toInteger(), path, response.value("recovery").toString(), {}, response.value("identity").toString()};
#else
    constexpr qsizetype MaxImageBytes = 8 * 1024 * 1024;
    if (!m_state->active.load(std::memory_order_acquire)) return {WorkspaceWriteStatus::Failed, WorkspaceReadError::Revoked};
    const auto suffix = extension.toLower();
    if (documentIdentity.isEmpty() || bytes.isEmpty() || bytes.size() > MaxImageBytes
        || !(suffix == "png" || suffix == "jpg" || suffix == "jpeg" || suffix == "gif" || suffix == "bmp" || suffix == "webp"))
        return {WorkspaceWriteStatus::Failed, bytes.size() > MaxImageBytes ? WorkspaceReadError::TooLarge : WorkspaceReadError::InvalidPath};
    auto document = sourceReader.read(documentPath, FileService::DiskByteLimit);
    if (!document.ok()) return {WorkspaceWriteStatus::Failed, document.error, document.nativeCode};
    if (document.identity != documentIdentity) return {WorkspaceWriteStatus::Failed, WorkspaceReadError::Changed};

    auto lease = std::make_shared<PublicationLease>();
    auto &lockedRoot = lease->root;
    lockedRoot.value = openRoot(m_state->path, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                FILE_LIST_DIRECTORY | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_READ_ATTRIBUTES);
    if (lockedRoot.value == INVALID_HANDLE_VALUE) { const auto code = GetLastError(); return {WorkspaceWriteStatus::Failed, nativeError(code), code}; }
    FILE_ID_INFO rootIdentity{};
    if (!directoryInfo(lockedRoot.value, rootIdentity) || !sameIdentity(rootIdentity, m_state->identity))
        return {WorkspaceWriteStatus::Failed, WorkspaceReadError::RootChanged};

    if (documentRoot && documentRoot.get() != this) {
        lease->documentRoot.value = openRoot(documentRoot->m_state->path, FILE_SHARE_READ | FILE_SHARE_WRITE);
        FILE_ID_INFO actual{};
        if (lease->documentRoot.value == INVALID_HANDLE_VALUE || !directoryInfo(lease->documentRoot.value, actual)
            || !sameIdentity(actual, documentRoot->m_state->identity)) return {WorkspaceWriteStatus::Failed, WorkspaceReadError::RootChanged};
    }
    const ULONG dirOptions = FILE_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT;
    auto &assets = lease->assets, &images = lease->images;
    NTSTATUS status = 0;
    {
        // Directory creation is a write too: once revoke() returns, it must
        // not start a new creation under the previous authorization.
        std::lock_guard<std::mutex> lock(m_state->commitMutex);
        if (!m_state->active.load(std::memory_order_acquire)) return {WorkspaceWriteStatus::Failed, WorkspaceReadError::Revoked};
        if (destination.isEmpty()) {
            if (!DuplicateHandle(GetCurrentProcess(), lockedRoot.value, GetCurrentProcess(), &images.value, 0, FALSE, DUPLICATE_SAME_ACCESS))
                return {WorkspaceWriteStatus::Failed, WorkspaceReadError::Io, GetLastError()};
        } else {
        status = openRelative(lockedRoot.value, QStringLiteral("assets"), FILE_LIST_DIRECTORY | FILE_ADD_SUBDIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF, dirOptions, assets.value);
        if (status < 0) { const auto code = RtlNtStatusToDosError(status); return {WorkspaceWriteStatus::Failed, nativeError(code), code}; }
        if (!directoryHandle(assets.value)) return {WorkspaceWriteStatus::Failed, WorkspaceReadError::OutsideOrLink};
        status = openRelative(assets.value, QStringLiteral("images"), FILE_LIST_DIRECTORY | FILE_ADD_FILE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF, dirOptions, images.value);
        if (status < 0) { const auto code = RtlNtStatusToDosError(status); return {WorkspaceWriteStatus::Failed, nativeError(code), code}; }
        if (!directoryHandle(images.value)) return {WorkspaceWriteStatus::Failed, WorkspaceReadError::OutsideOrLink};
        }
    }

    const auto stem = safeStem(suggestedName);
    const auto digest = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().left(12));
    const auto candidate = [&](int number) {
        return number == 0 ? stem + '.' + suffix
            : stem + '-' + digest + (number == 1 ? QString() : '-' + QString::number(number)) + '.' + suffix;
    };
    auto contextValid = [&]() {
        if (!m_state->active.load(std::memory_order_acquire)) return false;
        const auto checked = sourceReader.read(documentPath, FileService::DiskByteLimit);
        return checked.ok() && checked.identity == documentIdentity;
    };
    auto relative = [&destination](const QString &name) { return destination.isEmpty() ? name : destination + '/' + name; };
    int selected = 0;
    for (; selected < 128; ++selected) {
        const auto existing = existingBytes(images.value, candidate(selected), bytes);
        if (existing.state == Existing::Missing) break;
        if (existing.state == Existing::Same) {
            if (!lockDocument(*lease, documentPath, documentIdentity) || !contextValid()) return {WorkspaceWriteStatus::Failed, WorkspaceReadError::Changed};
            lease->resource = existing.file;
            return {WorkspaceWriteStatus::Reused, WorkspaceReadError::None, 0, relative(candidate(selected)), {}, lease, nativeIdentity(lease->resource->value)};
        }
    }
    if (selected == 128) return {WorkspaceWriteStatus::Failed, WorkspaceReadError::Io};
    if (!contextValid()) return {WorkspaceWriteStatus::Failed, WorkspaceReadError::Changed};

    auto stage = std::make_shared<NativeHandle>(); QString stageName;
    for (int attempt = 0; attempt < 16; ++attempt) {
        stageName = QStringLiteral(".xdnote-%1.tmp").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        {
            std::lock_guard<std::mutex> lock(m_state->commitMutex);
            if (!m_state->active.load(std::memory_order_acquire)) return {WorkspaceWriteStatus::Failed, WorkspaceReadError::Revoked};
            status = openRelative(images.value, stageName, GENERIC_READ | GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                  0, FILE_CREATE, FILE_NON_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT, stage->value);
        }
        if (status >= 0) break;
        const auto code = RtlNtStatusToDosError(status);
        if (code != ERROR_ALREADY_EXISTS && code != ERROR_FILE_EXISTS) return {WorkspaceWriteStatus::Failed, nativeError(code), code};
    }
    if (stage->value == INVALID_HANDLE_VALUE || stage->value == nullptr) return {WorkspaceWriteStatus::Failed, WorkspaceReadError::Io};
    auto failStage = [&](WorkspaceReadError error, qint64 code = 0) {
        const auto recovery = deleteOwnedTemporary(*stage) ? QString() : relative(stageName);
        return WorkspaceWriteResult{WorkspaceWriteStatus::Failed, error, code, {}, recovery};
    };
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        if (!m_state->active.load(std::memory_order_acquire)) return failStage(WorkspaceReadError::Revoked);
        DWORD count = 0; const auto chunk = DWORD(qMin<qsizetype>(bytes.size() - offset, 64 * 1024));
        if (!WriteFile(stage->value, bytes.constData() + offset, chunk, &count, nullptr) || count == 0) {
            const auto code = GetLastError(); return failStage(nativeError(code), code);
        }
        offset += count;
    }
    if (!FlushFileBuffers(stage->value)) { const auto code = GetLastError(); return failStage(nativeError(code), code); }
    if (!lockDocument(*lease, documentPath, documentIdentity) || !contextValid()) return failStage(WorkspaceReadError::Changed);
    if (observer) {
        try { observer(WorkspaceWriteStage::BeforeCommit); }
        catch (...) { return failStage(WorkspaceReadError::Io); }
    }

    for (; selected < 128; ++selected) {
        const auto name = candidate(selected);
        const auto existing = existingBytes(images.value, name, bytes);
        if (existing.state == Existing::Same) {
            if (!deleteOwnedTemporary(*stage)) return {WorkspaceWriteStatus::Failed, WorkspaceReadError::Io, GetLastError(), {}, relative(stageName)};
            if (!contextValid()) return {WorkspaceWriteStatus::Failed, WorkspaceReadError::Changed};
            lease->resource = existing.file;
            return {WorkspaceWriteStatus::Reused, WorkspaceReadError::None, 0, relative(name), {}, lease, nativeIdentity(lease->resource->value)};
        }
        if (existing.state == Existing::Occupied) continue;
        const auto byteLength = size_t(name.size()) * sizeof(wchar_t);
        // The kernel can complete a native file operation asynchronously. Its
        // status block, name and handles must then outlive the submission call.
        struct PendingRename {
            QByteArray storage;
            IO_STATUS_BLOCK io{};
            std::shared_ptr<NativeHandle> stage;
            std::shared_ptr<PublicationLease> lease;
        };
        auto pending = std::make_unique<PendingRename>();
        pending->storage = QByteArray(qsizetype(offsetof(FILE_RENAME_INFO, FileName) + byteLength), '\0');
        pending->stage = stage;
        pending->lease = lease;
        auto *rename = reinterpret_cast<FILE_RENAME_INFO *>(pending->storage.data());
        rename->ReplaceIfExists = FALSE; rename->RootDirectory = images.value; rename->FileNameLength = DWORD(byteLength);
        std::memcpy(rename->FileName, name.utf16(), byteLength);
        DWORD renameError = ERROR_SUCCESS;
        NTSTATUS renameStatus = 0;
        {
            std::lock_guard<std::mutex> lock(m_state->commitMutex);
            if (!m_state->active.load(std::memory_order_acquire)) renameError = ERROR_OPERATION_ABORTED;
            else {
                constexpr auto FileRenameInformationClass = static_cast<FILE_INFORMATION_CLASS>(10);
                renameStatus = NtSetInformationFile(stage->value, &pending->io, rename, ULONG(pending->storage.size()), FileRenameInformationClass);
            }
        }
        if (renameStatus == 0x103) {
            // Keep the IO status block and buffer alive until the kernel has
            // finished, but do not hold the authorization lock while waiting.
            if (WaitForSingleObject(stage->value, INFINITE) != WAIT_OBJECT_0) {
                const auto code = GetLastError();
                // An unsuccessful wait gives us no proof that the kernel has
                // stopped using the buffers. Retain them until process exit and
                // report both possible locations; never delete either entry.
                pending.release();
                return {WorkspaceWriteStatus::PublishedButUnverified, WorkspaceReadError::Io, code,
                        relative(name), relative(stageName)};
            }
            renameStatus = pending->io.Status;
        }
        if (renameStatus < 0 && renameError == ERROR_SUCCESS) renameError = RtlNtStatusToDosError(renameStatus);
        if (renameError != ERROR_SUCCESS) {
            const auto code = renameError;
            if (code == ERROR_OPERATION_ABORTED) return failStage(WorkspaceReadError::Revoked, code);
            if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) continue;
            return failStage(nativeError(code), code);
        }
        // After publication the resource is never automatically removed. Any
        // uncertain result includes its location so the caller can report it.
        const auto published = relative(name);
        if (observer) {
            try { observer(WorkspaceWriteStage::AfterCommit); }
            catch (...) { return {WorkspaceWriteStatus::PublishedButUnverified, WorkspaceReadError::Io, 0, published}; }
        }
        FILE_STANDARD_INFO finalInfo{};
        if (!GetFileInformationByHandleEx(stage->value, FileStandardInfo, &finalInfo, sizeof(finalInfo))
            || finalInfo.Directory || finalInfo.EndOfFile.QuadPart != bytes.size())
            return {WorkspaceWriteStatus::PublishedButUnverified, WorkspaceReadError::Changed, 0, published};
        LARGE_INTEGER start{};
        if (!SetFilePointerEx(stage->value, start, nullptr, FILE_BEGIN))
            return {WorkspaceWriteStatus::PublishedButUnverified, WorkspaceReadError::Io, GetLastError(), published};
        QByteArray verified(bytes.size(), Qt::Uninitialized); offset = 0;
        while (offset < verified.size()) {
            DWORD count = 0; const auto chunk = DWORD(qMin<qsizetype>(verified.size() - offset, 64 * 1024));
            if (!ReadFile(stage->value, verified.data() + offset, chunk, &count, nullptr) || count == 0)
                return {WorkspaceWriteStatus::PublishedButUnverified, WorkspaceReadError::Io, GetLastError(), published};
            offset += count;
        }
        if (verified != bytes || !contextValid()) return {WorkspaceWriteStatus::PublishedButUnverified, WorkspaceReadError::Changed, 0, published};
        lease->resource = std::move(stage);
        return {WorkspaceWriteStatus::Committed, WorkspaceReadError::None, 0, published, {}, lease, nativeIdentity(lease->resource->value)};
    }
    return failStage(WorkspaceReadError::Io);
#endif
}
}
