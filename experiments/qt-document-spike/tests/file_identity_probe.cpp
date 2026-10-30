#include "file_identity_probe.h"

#include <QDir>
#include <QFile>

#ifdef Q_OS_WIN
#  define NOMINMAX
#  include <windows.h>
#else
#  include <cerrno>
#  include <cstring>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

FixtureIdentityHandle::~FixtureIdentityHandle()
{
#ifdef Q_OS_WIN
    if (m_handle)
        CloseHandle(static_cast<HANDLE>(m_handle));
#else
    if (m_descriptor >= 0)
        ::close(m_descriptor);
#endif
}

bool FixtureIdentityHandle::open(const QString &path, QString *error)
{
    if (path.contains(QChar::Null) || !QDir::isAbsolutePath(path)) {
        *error = QStringLiteral("Expected an absolute path without NUL");
        return false;
    }
#ifdef Q_OS_WIN
    if (m_handle) {
        *error = QStringLiteral("Handle already open");
        return false;
    }
    const QString nativePath = QDir::toNativeSeparators(path);
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(nativePath.utf16()),
        FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        *error = QStringLiteral("CreateFileW failed: %1").arg(GetLastError());
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)) {
        const DWORD code = GetLastError();
        CloseHandle(handle);
        *error = QStringLiteral("GetFileInformationByHandle failed: %1").arg(code);
        return false;
    }
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        || GetFileType(handle) != FILE_TYPE_DISK) {
        CloseHandle(handle);
        *error = QStringLiteral("Expected a regular non-reparse file");
        return false;
    }
    m_handle = handle;
#else
    if (m_descriptor >= 0) {
        *error = QStringLiteral("Handle already open");
        return false;
    }
    const QByteArray nativePath = QFile::encodeName(path);
    const int descriptor = ::open(nativePath.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) {
        *error = QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
    struct stat info{};
    if (::fstat(descriptor, &info) != 0) {
        const int code = errno;
        ::close(descriptor);
        *error = QString::fromLocal8Bit(std::strerror(code));
        return false;
    }
    if (!S_ISREG(info.st_mode)) {
        ::close(descriptor);
        *error = QStringLiteral("Expected a regular non-symlink file");
        return false;
    }
    m_descriptor = descriptor;
#endif
    error->clear();
    return true;
}

bool FixtureIdentityHandle::identity(FixtureFileIdentity *value, QString *error) const
{
#ifdef Q_OS_WIN
    if (!m_handle) {
        *error = QStringLiteral("Handle not open");
        return false;
    }
    FILE_ID_INFO info{};
    if (!GetFileInformationByHandleEx(static_cast<HANDLE>(m_handle), FileIdInfo,
                                       &info, sizeof(info))) {
        *error = QStringLiteral("FileIdInfo unavailable: %1").arg(GetLastError());
        return false;
    }
    const QByteArray object(reinterpret_cast<const char *>(info.FileId.Identifier),
                            sizeof(info.FileId.Identifier));
    if (object == QByteArray(object.size(), '\0')) {
        *error = QStringLiteral("FileIdInfo returned an unusable zero identifier");
        return false;
    }
    value->volume = info.VolumeSerialNumber;
    value->object = object;
#else
    if (m_descriptor < 0) {
        *error = QStringLiteral("Handle not open");
        return false;
    }
    struct stat info{};
    if (::fstat(m_descriptor, &info) != 0) {
        *error = QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
    if (info.st_ino == 0) {
        *error = QStringLiteral("Filesystem returned an unusable inode");
        return false;
    }
    value->volume = static_cast<quint64>(info.st_dev);
    value->object = QByteArray::number(static_cast<qulonglong>(info.st_ino));
#endif
    error->clear();
    return true;
}
