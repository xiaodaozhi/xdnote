#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <cstdio>

#ifdef Q_OS_WIN
#  define NOMINMAX
#  include <windows.h>
#else
#  include <cerrno>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

// Deliberately restricted to this experiment's own uniquely named fixture.
// It waits with an open handle, then writes a fixed eight-byte test payload.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() != 2)
        return 2;
    const QString executableDir = QDir(QCoreApplication::applicationDirPath()).canonicalPath();
    const QString requestedRoot = executableDir + QStringLiteral("/spike-test-data");
    const QString root = QDir(requestedRoot).canonicalPath();
    const QFileInfo target(args.at(1));
    const QString parent = target.dir().canonicalPath();
    const QString canonical = target.canonicalFilePath();
    const QRegularExpression fixtureName(QStringLiteral("^fixture-[A-Za-z0-9]{6}$"));
    if (executableDir.isEmpty() || root.isEmpty() || !root.startsWith(executableDir + QLatin1Char('/'))
        || QFileInfo(requestedRoot).isSymLink() || QFileInfo(target.absolutePath()).isSymLink()
        || parent != root + QLatin1Char('/') + QFileInfo(parent).fileName()
        || !fixtureName.match(QFileInfo(parent).fileName()).hasMatch()
        || target.fileName() != QStringLiteral("note.md") || !target.isFile() || target.isSymLink()
        || canonical != parent + QStringLiteral("/note.md"))
        return 3;

#ifdef Q_OS_WIN
    const QString native = QDir::toNativeSeparators(canonical);
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return 4;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)
        || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        CloseHandle(handle);
        return 5;
    }
#else
    const QByteArray native = QFile::encodeName(canonical);
    const int handle = ::open(native.constData(), O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
    if (handle < 0)
        return 4;
    struct stat info{};
    if (::fstat(handle, &info) != 0 || !S_ISREG(info.st_mode)) {
        ::close(handle);
        return 5;
    }
#endif

    std::fputs("READY\n", stdout);
    std::fflush(stdout);
    const int command = std::getchar();
    bool success = command == 'W';
    constexpr char payload[] = "external";
#ifdef Q_OS_WIN
    LARGE_INTEGER position{};
    DWORD written = 0;
    success = success && SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)
        && WriteFile(handle, payload, sizeof(payload) - 1, &written, nullptr)
        && written == sizeof(payload) - 1 && FlushFileBuffers(handle);
    CloseHandle(handle);
#else
    success = success && ::lseek(handle, 0, SEEK_SET) == 0;
    size_t offset = 0;
    while (success && offset < sizeof(payload) - 1) {
        const ssize_t written = ::write(handle, payload + offset, sizeof(payload) - 1 - offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            success = false;
        } else {
            offset += static_cast<size_t>(written);
        }
    }
    success = success && ::fsync(handle) == 0;
    ::close(handle);
#endif
    return success ? 0 : 6;
}
