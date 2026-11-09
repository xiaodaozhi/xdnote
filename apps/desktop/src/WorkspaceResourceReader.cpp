#include "WorkspaceResourceReader.h"

#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QUuid>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

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

bool directoryHandle(HANDLE handle) {
    FILE_ATTRIBUTE_TAG_INFO attrs{}; FILE_STANDARD_INFO standard{};
    return GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attrs, sizeof(attrs))
        && GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard))
        && !(attrs.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && standard.Directory
        && GetFileType(handle) == FILE_TYPE_DISK;
}

enum class Existing { Missing, Same, Occupied };
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
    NativeHandle root, assets, images;
    std::shared_ptr<NativeHandle> resource;
    std::vector<std::shared_ptr<NativeHandle>> documentPath;
};

bool lockDocument(PublicationLease &lease, const QString &path, const QString &identity) {
    const auto parts = components(path);
    if (parts.isEmpty()) return false;
    HANDLE parent = lease.root.value;
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
            if (found != identity || info.EndOfFile.QuadPart < 0 || info.EndOfFile.QuadPart > 2 * 1024 * 1024) return false;
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
#if !defined(Q_OS_WIN) && !defined(Q_OS_LINUX)
    // A lexical or canonical-path check cannot make concurrent directory
    // replacement safe. Unsupported platforms must fail closed.
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

WorkspaceReadResult WorkspaceResourceReader::read(const QString &path, qsizetype maxBytes) const {
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
        const auto status = openChild(parent, parts.at(index), directory, handle->value);
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
    how.flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
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
#else
    Q_UNUSED(maxBytes);
    return {WorkspaceReadError::Unsupported};
#endif
#if defined(Q_OS_WIN) || (defined(Q_OS_LINUX) && defined(SYS_openat2))
    if (!m_state->active.load(std::memory_order_acquire)) return {WorkspaceReadError::Revoked};
    return {WorkspaceReadError::None, std::move(bytes), 0, identity};
#endif
}

WorkspaceWriteResult WorkspaceResourceReader::publishImage(const QString &documentPath, const QString &documentIdentity,
    const QByteArray &bytes, const QString &suggestedName, const QString &extension,
    const std::function<void(WorkspaceWriteStage)> &observer) const {
#ifndef Q_OS_WIN
    Q_UNUSED(documentPath); Q_UNUSED(documentIdentity); Q_UNUSED(bytes); Q_UNUSED(suggestedName); Q_UNUSED(extension); Q_UNUSED(observer);
    return {WorkspaceWriteStatus::Failed, WorkspaceReadError::Unsupported};
#else
    constexpr qsizetype MaxImageBytes = 8 * 1024 * 1024;
    if (!m_state->active.load(std::memory_order_acquire)) return {WorkspaceWriteStatus::Failed, WorkspaceReadError::Revoked};
    const auto suffix = extension.toLower();
    if (documentIdentity.isEmpty() || bytes.isEmpty() || bytes.size() > MaxImageBytes
        || !(suffix == "png" || suffix == "jpg" || suffix == "jpeg" || suffix == "gif" || suffix == "bmp" || suffix == "webp"))
        return {WorkspaceWriteStatus::Failed, bytes.size() > MaxImageBytes ? WorkspaceReadError::TooLarge : WorkspaceReadError::InvalidPath};
    auto document = read(documentPath, 2 * 1024 * 1024);
    if (!document.ok()) return {WorkspaceWriteStatus::Failed, document.error, document.nativeCode};
    if (document.identity != documentIdentity) return {WorkspaceWriteStatus::Failed, WorkspaceReadError::Changed};

    auto lease = std::make_shared<PublicationLease>();
    auto &lockedRoot = lease->root;
    lockedRoot.value = openRoot(m_state->path, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                FILE_LIST_DIRECTORY | FILE_ADD_SUBDIRECTORY | FILE_READ_ATTRIBUTES);
    if (lockedRoot.value == INVALID_HANDLE_VALUE) { const auto code = GetLastError(); return {WorkspaceWriteStatus::Failed, nativeError(code), code}; }
    FILE_ID_INFO rootIdentity{};
    if (!directoryInfo(lockedRoot.value, rootIdentity) || !sameIdentity(rootIdentity, m_state->identity))
        return {WorkspaceWriteStatus::Failed, WorkspaceReadError::RootChanged};

    const ULONG dirOptions = FILE_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT;
    auto &assets = lease->assets, &images = lease->images;
    NTSTATUS status = 0;
    {
        // Directory creation is a write too: once revoke() returns, it must
        // not start a new creation under the previous authorization.
        std::lock_guard<std::mutex> lock(m_state->commitMutex);
        if (!m_state->active.load(std::memory_order_acquire)) return {WorkspaceWriteStatus::Failed, WorkspaceReadError::Revoked};
        status = openRelative(lockedRoot.value, QStringLiteral("assets"), FILE_LIST_DIRECTORY | FILE_ADD_SUBDIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF, dirOptions, assets.value);
        if (status < 0) { const auto code = RtlNtStatusToDosError(status); return {WorkspaceWriteStatus::Failed, nativeError(code), code}; }
        if (!directoryHandle(assets.value)) return {WorkspaceWriteStatus::Failed, WorkspaceReadError::OutsideOrLink};
        status = openRelative(assets.value, QStringLiteral("images"), FILE_LIST_DIRECTORY | FILE_ADD_FILE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF, dirOptions, images.value);
        if (status < 0) { const auto code = RtlNtStatusToDosError(status); return {WorkspaceWriteStatus::Failed, nativeError(code), code}; }
        if (!directoryHandle(images.value)) return {WorkspaceWriteStatus::Failed, WorkspaceReadError::OutsideOrLink};
    }

    const auto stem = safeStem(suggestedName);
    const auto digest = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().left(12));
    const auto candidate = [&](int number) {
        return number == 0 ? stem + '.' + suffix
            : stem + '-' + digest + (number == 1 ? QString() : '-' + QString::number(number)) + '.' + suffix;
    };
    auto contextValid = [&]() {
        if (!m_state->active.load(std::memory_order_acquire)) return false;
        const auto checked = read(documentPath, 2 * 1024 * 1024);
        return checked.ok() && checked.identity == documentIdentity;
    };
    auto relative = [](const QString &name) { return QStringLiteral("assets/images/") + name; };
    int selected = 0;
    for (; selected < 128; ++selected) {
        const auto existing = existingBytes(images.value, candidate(selected), bytes);
        if (existing.state == Existing::Missing) break;
        if (existing.state == Existing::Same) {
            if (!lockDocument(*lease, documentPath, documentIdentity) || !contextValid()) return {WorkspaceWriteStatus::Failed, WorkspaceReadError::Changed};
            lease->resource = existing.file;
            return {WorkspaceWriteStatus::Reused, WorkspaceReadError::None, 0, relative(candidate(selected)), {}, lease};
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
            return {WorkspaceWriteStatus::Reused, WorkspaceReadError::None, 0, relative(name), {}, lease};
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
        return {WorkspaceWriteStatus::Committed, WorkspaceReadError::None, 0, published, {}, lease};
    }
    return failStage(WorkspaceReadError::Io);
#endif
}
}
