#include "windows_commit_diagnostic.h"

#ifdef Q_OS_WIN
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#define NOMINMAX
#include <windows.h>

namespace {
bool isPlainDirectory(const QString &path)
{
    const QString native = QDir::toNativeSeparators(path);
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(native.utf16()));
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)
        && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

QString validatedFixtureRoot(const QString &requested)
{
    const QString executable = QDir(QCoreApplication::applicationDirPath()).canonicalPath();
    const QString dataRoot = QDir(executable + QStringLiteral("/spike-test-data")).canonicalPath();
    const QString root = QDir(requested).canonicalPath();
    const QFileInfo info(root);
    const QRegularExpression name(QStringLiteral("^fixture-[A-Za-z0-9]{6}$"));
    if (dataRoot.isEmpty() || root.isEmpty() || info.dir().canonicalPath() != dataRoot
        || !name.match(info.fileName()).hasMatch() || info.isSymLink()
        || !isPlainDirectory(dataRoot) || !isPlainDirectory(root))
        return {};
    return root;
}

QString classifyFixtureBytes(const QByteArray &bytes, const QByteArray *baseline, const QByteArray *attempted)
{
    if (baseline && attempted) {
        const bool matchesBaseline = bytes == *baseline;
        const bool matchesAttempted = bytes == *attempted;
        if (matchesBaseline && matchesAttempted)
            return QStringLiteral("baseline-and-attempted");
        if (matchesBaseline)
            return QStringLiteral("baseline");
        if (matchesAttempted)
            return QStringLiteral("attempted");
    }
    if (bytes == QByteArray("original"))
        return QStringLiteral("original");
    if (bytes == QByteArray("external"))
        return QStringLiteral("external");
    if (bytes == QByteArray("local"))
        return QStringLiteral("local");
    return QStringLiteral("other");
}
} // namespace

int fixtureOtherEntryCount(const QString &fixtureRoot)
{
    const QString root = validatedFixtureRoot(fixtureRoot);
    if (root.isEmpty())
        return -1;
    QStringList entries = QDir(root).entryList(QDir::AllEntries | QDir::Hidden | QDir::System
                                              | QDir::NoDotAndDotDot);
    entries.removeAll(QStringLiteral("note.md"));
    return entries.size();
}

FixtureCommitObservation observeFixtureCommitFailure(const QString &fixtureRoot,
                                                     const QByteArray *baseline, const QByteArray *attempted)
{
    FixtureCommitObservation observation;
    const QString root = validatedFixtureRoot(fixtureRoot);
    if (root.isEmpty())
        return observation;
    observation.valid = true;
    observation.otherEntries = fixtureOtherEntryCount(root);
    const QString native = QDir::toNativeSeparators(root + QStringLiteral("/note.md"));
    const auto path = reinterpret_cast<LPCWSTR>(native.utf16());
    const DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        observation.targetNativeError = GetLastError();
        observation.targetState = observation.targetNativeError == ERROR_FILE_NOT_FOUND
                || observation.targetNativeError == ERROR_PATH_NOT_FOUND
            ? QStringLiteral("missing") : QStringLiteral("attributes-error");
        return observation;
    }
    if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
        observation.targetState = QStringLiteral("unexpected-type");
        return observation;
    }

    HANDLE reader = CreateFileW(path, GENERIC_READ | FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (reader == INVALID_HANDLE_VALUE) {
        observation.targetNativeError = GetLastError();
        observation.targetState = QStringLiteral("read-open-error");
    } else {
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(reader, &info)) {
            observation.targetNativeError = GetLastError();
            observation.targetState = QStringLiteral("handle-info-error");
        } else if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
                   || GetFileType(reader) != FILE_TYPE_DISK) {
            observation.targetState = QStringLiteral("unexpected-type");
        } else {
            char bytes[64]{};
            DWORD count = 0;
            LARGE_INTEGER before{};
            LARGE_INTEGER after{};
            if (!GetFileSizeEx(reader, &before)) {
                observation.targetNativeError = GetLastError();
                observation.targetState = QStringLiteral("size-error");
            } else if (before.QuadPart < 0 || before.QuadPart > qint64(sizeof(bytes))) {
                observation.targetState = QStringLiteral("too-large-to-classify");
            } else if (!ReadFile(reader, bytes, sizeof(bytes), &count, nullptr)) {
                observation.targetNativeError = GetLastError();
                observation.targetState = QStringLiteral("read-error");
            } else if (!GetFileSizeEx(reader, &after)) {
                observation.targetNativeError = GetLastError();
                observation.targetState = QStringLiteral("size-error");
            } else if (before.QuadPart != after.QuadPart || before.QuadPart != qint64(count)) {
                observation.targetState = QStringLiteral("changed-during-read");
            } else {
                observation.targetState = classifyFixtureBytes(QByteArray(bytes, int(count)), baseline, attempted);
            }
        }
        CloseHandle(reader);
    }

    // This is a separate post-failure observation. Requesting DELETE access
    // does not delete or rename the fixture and does not recover Qt's error.
    observation.deleteOpenAttempted = true;
    HANDLE probe = CreateFileW(path, DELETE | FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (probe == INVALID_HANDLE_VALUE) {
        observation.deleteOpenNativeError = GetLastError();
    } else {
        observation.deleteOpenSucceeded = true;
        CloseHandle(probe);
    }
    return observation;
}

QString describeFixtureCommitFailure(const FixtureCommitObservation &observation,
                                     int beforeOpenEntries, int afterOpenEntries)
{
    if (!observation.valid)
        return QStringLiteral("fixture observation rejected");
    const QString deleteStatus = !observation.deleteOpenAttempted ? QStringLiteral("not-attempted")
        : observation.deleteOpenSucceeded ? QStringLiteral("opened") : QStringLiteral("failed");
    return QStringLiteral("fixture target=%1; target native error=%2; other entries before/after open/after commit=%3/%4/%5; "
                          "separate post-failure DELETE-access open=%6; its native error=%7")
        .arg(observation.targetState).arg(observation.targetNativeError)
        .arg(beforeOpenEntries).arg(afterOpenEntries).arg(observation.otherEntries)
        .arg(deleteStatus).arg(observation.deleteOpenNativeError);
}
#endif
