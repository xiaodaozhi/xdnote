#include "WorkspaceResourceReader.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef Q_OS_LINUX
#include <linux/landlock.h>
#include <linux/fs.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#else
#include <sandbox.h>
#endif

// One bounded request per process. Only this process receives the kernel
// filesystem restriction; the editor and its other open documents are untouched.
namespace {
using Error = xdnote::WorkspaceReadError;
struct Fd { int value = -1; ~Fd() { if (value >= 0) ::close(value); } };
QString identity(const struct stat &s) { return QStringLiteral("posix:%1:%2").arg(qulonglong(s.st_dev), 0, 16).arg(qulonglong(s.st_ino), 0, 16); }
Error errorFor(int e) {
    if (e == ENOENT || e == ENOTDIR) return Error::Missing;
    if (e == EACCES || e == EPERM) return Error::Denied;
    if (e == ELOOP || e == EXDEV) return Error::OutsideOrLink;
    return Error::Io;
}
QJsonObject failed(Error e, int native = 0) { return {{"error", int(e)}, {"native", native}}; }
QStringList components(const QString &path) {
    if (path.isEmpty() || path.size() > 8192 || path.startsWith('/') || path.contains('\\')) return {};
    const auto parts = path.split('/');
    for (const auto &part : parts) {
        if (part.isEmpty() || part == "." || part == ".." || part.toUtf8().size() > 255) return {};
        for (const auto ch : part) if (ch.isNull() || ch.category() == QChar::Other_Control) return {};
    }
    return parts;
}
int openWithin(int root, const QString &path, bool directory, dev_t device, bool metadata = false) {
    if (path.isEmpty() && directory) return ::dup(root);
    const auto parts = components(path); if (parts.isEmpty()) { errno = EINVAL; return -1; }
    Fd parent; parent.value = ::dup(root); if (parent.value < 0) return -1;
    for (qsizetype i = 0; i < parts.size(); ++i) {
        const bool dir = directory || i + 1 < parts.size();
        const auto name = parts.at(i).toUtf8();
        int access = O_RDONLY;
#ifdef Q_OS_MACOS
        if (!dir && metadata) access = O_EVTONLY;
#else
        Q_UNUSED(metadata);
#endif
        const int child = ::openat(parent.value, name.constData(), access | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | (dir ? O_DIRECTORY : 0));
        if (child < 0) return -1;
        struct stat info{};
        if (::fstat(child, &info) != 0 || info.st_dev != device || (dir ? !S_ISDIR(info.st_mode) : !S_ISREG(info.st_mode))) {
            ::close(child); errno = EXDEV; return -1;
        }
        ::close(parent.value); parent.value = child;
    }
    const int result = parent.value; parent.value = -1; return result;
}
bool restrictFilesystem(int root, const QString &path, bool writing) {
#ifdef Q_OS_LINUX
    Q_UNUSED(path);
    const int abi = int(::syscall(SYS_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION));
    if (abi < 1) return false;
    quint64 rights = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_FILE
        | LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE
        | LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG
        | LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_SYM;
    // ABI 1 denies cross-directory reparenting. No copy/delete fallback is used.
    landlock_ruleset_attr attributes{}; attributes.handled_access_fs = rights;
    Fd rules; rules.value = int(::syscall(SYS_landlock_create_ruleset, &attributes, sizeof(attributes), 0));
    if (rules.value < 0) return false;
    landlock_path_beneath_attr rule{}; rule.parent_fd = root;
    rule.allowed_access = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
    if (writing) rule.allowed_access |= LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_MAKE_DIR
        | LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_REMOVE_FILE;
    return ::syscall(SYS_landlock_add_rule, rules.value, LANDLOCK_RULE_PATH_BENEATH, &rule, 0) == 0
        && ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 && ::syscall(SYS_landlock_restrict_self, rules.value, 0) == 0;
#else
    Q_UNUSED(root);
    auto quoted = QJsonDocument(QJsonArray{path}).toJson(QJsonDocument::Compact); quoted = quoted.mid(1, quoted.size() - 2);
    QByteArray profile = "(version 1)(deny default)(allow file-read* (subpath " + quoted + "))";
    if (writing) profile += "(allow file-write* (subpath " + quoted + "))";
    char *message = nullptr;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    const int status = sandbox_init(profile.constData(), 0, &message);
#pragma clang diagnostic pop
    if (message) sandbox_free_error(message);
    return status == 0;
#endif
}
bool readBytes(int fd, qsizetype limit, QByteArray &bytes) {
    struct stat before{}, after{};
    if (::fstat(fd, &before) != 0 || !S_ISREG(before.st_mode)) return false;
    if (before.st_size < 0 || before.st_size > limit) { errno = EFBIG; return false; }
    bytes.resize(qsizetype(before.st_size)); qsizetype offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::pread(fd, bytes.data() + offset, size_t(bytes.size() - offset), offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { errno = EIO; return false; } offset += count;
    }
    if (::fstat(fd, &after) != 0) return false;
#ifdef Q_OS_MACOS
    const bool sameTimes = before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec && before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec
        && before.st_ctimespec.tv_sec == after.st_ctimespec.tv_sec && before.st_ctimespec.tv_nsec == after.st_ctimespec.tv_nsec;
#else
    const bool sameTimes = before.st_mtim.tv_sec == after.st_mtim.tv_sec && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec
        && before.st_ctim.tv_sec == after.st_ctim.tv_sec && before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
#endif
    if (before.st_size != after.st_size || !sameTimes) { errno = ESTALE; return false; } return true;
}
QJsonObject request(const QJsonObject &input) {
    const auto rootPath = input.value("root").toString(), expected = input.value("identity").toString();
    const auto path = input.value("path").toString(), operation = input.value("operation").toString();
    if (!QDir::isAbsolutePath(rootPath) || rootPath.contains(QChar::Null) || expected.isEmpty()) return failed(Error::InvalidPath);
    Fd root; root.value = ::open(QFile::encodeName(rootPath).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat rootInfo{};
    if (root.value < 0 || ::fstat(root.value, &rootInfo) != 0) return failed(errorFor(errno), errno);
    if (identity(rootInfo) != expected) return failed(Error::RootChanged);
    const auto canonical = QDir(rootPath).canonicalPath();
    const auto nonce = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (canonical.isEmpty() || !restrictFilesystem(root.value, canonical, operation == "publish")) return failed(Error::Unsupported);
    auto rootCurrent = [&] {
        Fd check; check.value = ::open(QFile::encodeName(rootPath).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        struct stat info{}; return check.value >= 0 && ::fstat(check.value, &info) == 0 && identity(info) == expected;
    };
    if (!rootCurrent()) return failed(Error::RootChanged);
    if (operation == "publish") {
        const auto bytes = QByteArray::fromBase64(input.value("bytes").toString().toLatin1());
        if (bytes.isEmpty() || bytes.size() > 8 * 1024 * 1024) return failed(Error::TooLarge);
        const auto suffix = input.value("extension").toString();
        if (!QStringList{"png", "jpg", "jpeg", "gif", "bmp", "webp"}.contains(suffix)) return failed(Error::InvalidPath);
        if (!path.isEmpty() && components(path).isEmpty()) return failed(Error::InvalidPath);
        QString prefix;
        for (const auto &part : path.isEmpty() ? QStringList{} : path.split('/')) {
            const auto parentPath = prefix; prefix += (prefix.isEmpty() ? "" : "/") + part;
            Fd parent; parent.value = openWithin(root.value, parentPath, true, rootInfo.st_dev);
            if (parent.value < 0) return failed(errorFor(errno), errno);
            if (::mkdirat(parent.value, part.toUtf8().constData(), 0700) != 0 && errno != EEXIST) return failed(errorFor(errno), errno);
            Fd verified; verified.value = openWithin(root.value, prefix, true, rootInfo.st_dev);
            if (verified.value < 0) return failed(errorFor(errno), errno);
        }
        Fd directory; directory.value = openWithin(root.value, path, true, rootInfo.st_dev);
        if (directory.value < 0) return failed(errorFor(errno), errno);
        const auto stem = input.value("stem").toString();
        if (components(stem).size() != 1) return failed(Error::InvalidPath);
        const auto digest = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().left(12));
        const auto relative = [&](const QString &name) { return path.isEmpty() ? name : path + '/' + name; };
        for (int number = 0; number < 128; ++number) {
            const auto name = stem + (number == 0 ? QString() : '-' + digest + (number == 1 ? QString() : '-' + QString::number(number))) + '.' + suffix;
            Fd existing; existing.value = openWithin(root.value, relative(name), false, rootInfo.st_dev);
            if (existing.value >= 0) {
                QByteArray current;
                if (readBytes(existing.value, bytes.size(), current) && current == bytes && rootCurrent())
                {
                    struct stat actual{}; if (::fstat(existing.value, &actual) != 0) return failed(Error::Io, errno);
                    return {{"status", int(xdnote::WorkspaceWriteStatus::Reused)}, {"path", relative(name)}, {"error", 0}, {"identity", identity(actual)}};
                }
                continue;
            }
            if (errno != ENOENT && errno != ELOOP && errno != EXDEV) return failed(errorFor(errno), errno);
            const auto temporary = QStringLiteral(".xdnote-%1-%2.tmp").arg(nonce).arg(number);
            Fd stage; stage.value = ::openat(directory.value, temporary.toUtf8().constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (stage.value < 0) return failed(errorFor(errno), errno);
            struct stat stageInfo{};
            const auto failedStage = [&](Error error, int code) {
                auto result = failed(error, code); result["recovery"] = relative(temporary); return result;
            };
            if (::fstat(stage.value, &stageInfo) != 0) return failedStage(Error::Io, errno);
            qsizetype offset = 0; bool written = true;
            while (offset < bytes.size()) {
                const auto count = ::write(stage.value, bytes.constData() + offset, size_t(bytes.size() - offset));
                if (count < 0 && errno == EINTR) continue;
                if (count <= 0) { written = false; break; } offset += count;
            }
            if (!written || ::fsync(stage.value) != 0 || !rootCurrent()) {
                return failedStage(Error::Io, errno);
            }
            struct stat pending{};
            if (::fstatat(directory.value, temporary.toUtf8().constData(), &pending, AT_SYMLINK_NOFOLLOW) != 0
                || identity(pending) != identity(stageInfo)) return failedStage(Error::Changed, errno);
            // No-replace rename leaves no temporary name on success. On a
            // failure we preserve and report the entry: unlink-by-name after
            // an identity check would have a destructive replacement race.
#ifdef Q_OS_LINUX
            const int renamed = int(::syscall(SYS_renameat2, directory.value, temporary.toUtf8().constData(), directory.value, name.toUtf8().constData(), RENAME_NOREPLACE));
#else
            const int renamed = ::renameatx_np(directory.value, temporary.toUtf8().constData(), directory.value, name.toUtf8().constData(), RENAME_EXCL);
#endif
            if (renamed != 0) return failedStage(errorFor(errno), errno);
            const bool synced = ::fsync(directory.value) == 0;
            Fd final; final.value = openWithin(root.value, relative(name), false, rootInfo.st_dev); QByteArray verified; struct stat info{};
            const bool matches = final.value >= 0 && ::fstat(final.value, &info) == 0 && identity(info) == identity(stageInfo)
                && readBytes(final.value, bytes.size(), verified) && verified == bytes && rootCurrent();
            return {{"status", int(matches && synced ? xdnote::WorkspaceWriteStatus::Committed : xdnote::WorkspaceWriteStatus::PublishedButUnverified)},
                    {"path", relative(name)}, {"error", matches ? 0 : int(Error::Changed)}, {"identity", identity(stageInfo)}};
        }
        return failed(Error::Changed);
    }
    if (operation != "read" && operation != "inspect" && operation != "list") return failed(Error::InvalidPath);
    Fd file; file.value = openWithin(root.value, path, operation == "list", rootInfo.st_dev, operation == "inspect");
    if (file.value < 0) return failed(errorFor(errno), errno);
    struct stat info{}; if (::fstat(file.value, &info) != 0) return failed(Error::Io, errno);
    QJsonObject result{{"error", 0}, {"identity", identity(info)}};
    if (operation == "read") {
        const int limit = input.value("limit").toInt(-1);
        if (limit < 0 || limit > 32 * 1024 * 1024) return failed(Error::InvalidPath);
        QByteArray bytes; if (!readBytes(file.value, limit, bytes)) return failed(errno == EFBIG ? Error::TooLarge : errorFor(errno), errno);
        result["bytes"] = QString::fromLatin1(bytes.toBase64());
    } else if (operation == "list") {
        const int limit = input.value("limit").toInt(-1); if (limit < 1 || limit > 10000) return failed(Error::InvalidPath);
        DIR *directory = ::fdopendir(::dup(file.value)); if (!directory) return failed(Error::Io, errno);
        QJsonArray entries; bool complete = true; errno = 0;
        while (const auto *entry = ::readdir(directory)) {
            const QByteArray raw(entry->d_name); if (raw == "." || raw == "..") continue;
            if (entries.size() == limit) { complete = false; break; }
            const auto name = QString::fromUtf8(raw); if (name.toUtf8() != raw) { complete = false; continue; }
            struct stat child{}; int kind = 2;
            if (::fstatat(file.value, raw.constData(), &child, AT_SYMLINK_NOFOLLOW) == 0 && child.st_dev == rootInfo.st_dev)
                kind = S_ISDIR(child.st_mode) ? 1 : S_ISREG(child.st_mode) ? 0 : 2;
            entries.append(QJsonObject{{"name", name}, {"kind", kind}}); errno = 0;
        }
        const int code = errno; ::closedir(directory); if (code) return failed(errorFor(code), code);
        result["entries"] = entries; result["complete"] = complete;
    }
    return rootCurrent() ? result : failed(Error::RootChanged);
}
}
int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    QFile input; input.open(stdin, QIODevice::ReadOnly); QByteArray bytes;
    while (bytes.size() <= 16 * 1024 * 1024) {
        const auto chunk = input.read(65536); if (chunk.isEmpty()) break; bytes += chunk;
    }
    QJsonObject result;
    if (bytes.size() > 16 * 1024 * 1024) result = failed(Error::TooLarge);
    else {
        QJsonParseError error{}; const auto document = QJsonDocument::fromJson(bytes, &error);
        result = error.error == QJsonParseError::NoError && document.isObject() ? request(document.object()) : failed(Error::InvalidPath);
    }
    QFile output; output.open(stdout, QIODevice::WriteOnly); const auto response = QJsonDocument(result).toJson(QJsonDocument::Compact);
    return output.write(response) == response.size() && output.flush() ? 0 : 1;
}
