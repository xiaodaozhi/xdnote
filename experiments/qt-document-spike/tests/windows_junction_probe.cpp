#include "windows_junction_probe.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#define NOMINMAX
#include <windows.h>
#include <winioctl.h>

namespace {
struct MountPointHeader {
    DWORD tag;
    WORD dataLength;
    WORD reserved;
    WORD substituteOffset;
    WORD substituteLength;
    WORD printOffset;
    WORD printLength;
};
static_assert(sizeof(MountPointHeader) == 16, "unexpected Windows reparse header layout");

bool isPlainDirectory(const QString &path)
{
    const QString native = QDir::toNativeSeparators(path);
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(native.utf16()));
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)
        && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}
} // namespace

quint32 createFixtureJunction(const QString &fixtureRoot)
{
    const QString executable = QDir(QCoreApplication::applicationDirPath()).canonicalPath();
    const QString dataRoot = QDir(executable + QStringLiteral("/spike-test-data")).canonicalPath();
    const QString root = QDir(fixtureRoot).canonicalPath();
    const QFileInfo rootInfo(root);
    const QRegularExpression fixtureName(QStringLiteral("^fixture-[A-Za-z0-9]{6}$"));
    if (dataRoot.isEmpty() || root.isEmpty() || rootInfo.dir().canonicalPath() != dataRoot
        || !fixtureName.match(rootInfo.fileName()).hasMatch() || !isPlainDirectory(root))
        return ERROR_INVALID_PARAMETER;

    const QString workspace = root + QStringLiteral("/workspace");
    const QString target = root + QStringLiteral("/outside");
    if (QDir(workspace).canonicalPath() != workspace || QDir(target).canonicalPath() != target
        || !isPlainDirectory(workspace) || !isPlainDirectory(target))
        return ERROR_INVALID_PARAMETER;
    const QString targetNative = QDir::toNativeSeparators(target);
    if (targetNative.size() < 3 || targetNative.at(1) != QLatin1Char(':'))
        return ERROR_NOT_SUPPORTED; // This fixture does not construct UNC junctions.
    const QString substitute = QStringLiteral("\\??\\") + targetNative;
    const int substituteBytes = int(substitute.size() * sizeof(ushort));
    const int printBytes = int(targetNative.size() * sizeof(ushort));
    const int nameBytes = substituteBytes + int(sizeof(ushort)) + printBytes + int(sizeof(ushort));
    const int dataLength = 8 + nameBytes;
    if (dataLength > 0xffff || 8 + dataLength > MAXIMUM_REPARSE_DATA_BUFFER_SIZE)
        return ERROR_FILENAME_EXCED_RANGE;

    MountPointHeader header{};
    header.tag = IO_REPARSE_TAG_MOUNT_POINT;
    header.dataLength = WORD(dataLength);
    header.substituteLength = WORD(substituteBytes);
    header.printOffset = WORD(substituteBytes + sizeof(ushort));
    header.printLength = WORD(printBytes);
    QByteArray buffer(reinterpret_cast<const char *>(&header), sizeof(header));
    buffer.append(reinterpret_cast<const char *>(substitute.utf16()), substituteBytes);
    buffer.append(int(sizeof(ushort)), '\0');
    buffer.append(reinterpret_cast<const char *>(targetNative.utf16()), printBytes);
    buffer.append(int(sizeof(ushort)), '\0');

    const QString linkNative = QDir::toNativeSeparators(workspace + QStringLiteral("/linked"));
    const auto linkPath = reinterpret_cast<LPCWSTR>(linkNative.utf16());
    if (!CreateDirectoryW(linkPath, nullptr))
        return GetLastError();
    HANDLE handle = CreateFileW(linkPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        RemoveDirectoryW(linkPath);
        return error;
    }
    DWORD ignored = 0;
    const BOOL created = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(), DWORD(buffer.size()),
                                         nullptr, 0, &ignored, nullptr);
    const DWORD error = created ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (!created)
        RemoveDirectoryW(linkPath);
    return error;
}
