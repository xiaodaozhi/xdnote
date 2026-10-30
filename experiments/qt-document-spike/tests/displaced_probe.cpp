#include "displaced_probe.h"

#include <QDir>
#include <QFile>
#include <QtGlobal>

#ifdef Q_OS_WIN
#  define NOMINMAX
#  include <windows.h>
#elif defined(Q_OS_MACOS)
#  include <cerrno>
#  include <cstdio>
#  include <cstring>
#elif defined(Q_OS_LINUX)
#  include <cerrno>
#  include <cstring>
#  include <fcntl.h>
#  include <linux/fs.h>
#  include <sys/syscall.h>
#  include <unistd.h>
#endif

bool replaceAndKeepDisplaced(const QString &target, const QString &prepared,
                             const QString &windowsBackup, QString *displaced,
                             QString *error)
{
#ifdef Q_OS_WIN
    const QString nativeTarget = QDir::toNativeSeparators(target);
    const QString nativePrepared = QDir::toNativeSeparators(prepared);
    const QString nativeBackup = QDir::toNativeSeparators(windowsBackup);
    if (!ReplaceFileW(reinterpret_cast<LPCWSTR>(nativeTarget.utf16()),
                      reinterpret_cast<LPCWSTR>(nativePrepared.utf16()),
                      reinterpret_cast<LPCWSTR>(nativeBackup.utf16()), 0, nullptr, nullptr)) {
        *error = QStringLiteral("ReplaceFileW failed: %1").arg(GetLastError());
        return false;
    }
    *displaced = windowsBackup;
#elif defined(Q_OS_MACOS)
    Q_UNUSED(windowsBackup);
    const QByteArray source = QFile::encodeName(prepared);
    const QByteArray destination = QFile::encodeName(target);
    if (::renamex_np(source.constData(), destination.constData(), RENAME_SWAP) != 0) {
        *error = QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
    *displaced = prepared;
#elif defined(Q_OS_LINUX)
    Q_UNUSED(windowsBackup);
    const QByteArray source = QFile::encodeName(prepared);
    const QByteArray destination = QFile::encodeName(target);
    if (::syscall(SYS_renameat2, AT_FDCWD, source.constData(), AT_FDCWD,
                  destination.constData(), RENAME_EXCHANGE) != 0) {
        *error = QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
    *displaced = prepared;
#else
    Q_UNUSED(target);
    Q_UNUSED(prepared);
    Q_UNUSED(windowsBackup);
    Q_UNUSED(displaced);
    *error = QStringLiteral("No candidate primitive for this platform");
    return false;
#endif
    error->clear();
    return true;
}
