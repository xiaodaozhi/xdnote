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
// Writer mode sends a fixed payload; reader modes retain and check the original.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    const bool qtReader = args.size() == 3 && args.at(1) == QStringLiteral("--hold-qt-read");
    const bool sharedReader = args.size() == 3 && args.at(1) == QStringLiteral("--hold-shared-read");
    if (args.size() != 2 && !qtReader && !sharedReader)
        return 2;
    const QString executableDir = QDir(QCoreApplication::applicationDirPath()).canonicalPath();
    const QString requestedRoot = executableDir + QStringLiteral("/spike-test-data");
    const QString root = QDir(requestedRoot).canonicalPath();
    const QFileInfo target(args.constLast());
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

    if (qtReader) {
        QFile reader(canonical);
        if (!reader.open(QIODevice::ReadOnly))
            return 4;
        std::fputs("READY\n", stdout);
        std::fflush(stdout);
        if (std::getchar() != 'R')
            return 6;
        const QByteArray original = reader.readAll();
        if (reader.error() != QFileDevice::NoError || original != QByteArray("original"))
            return 7;
        std::fputs("HELD_ORIGINAL\n", stdout);
        std::fflush(stdout);
        return 0;
    }

    if (sharedReader) {
#ifdef Q_OS_WIN
        const QString native = QDir::toNativeSeparators(canonical);
        HANDLE reader = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (reader == INVALID_HANDLE_VALUE)
            return 4;
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(reader, &info)
            || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
            CloseHandle(reader);
            return 5;
        }
        std::fputs("READY\n", stdout);
        std::fflush(stdout);
        const int command = std::getchar();
        char bytes[32]{};
        DWORD count = 0;
        const bool success = command == 'R' && ReadFile(reader, bytes, sizeof(bytes), &count, nullptr)
            && QByteArray(bytes, int(count)) == QByteArray("original");
        CloseHandle(reader);
#else
        const QByteArray native = QFile::encodeName(canonical);
        const int reader = ::open(native.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (reader < 0)
            return 4;
        struct stat info{};
        if (::fstat(reader, &info) != 0 || !S_ISREG(info.st_mode)) {
            ::close(reader);
            return 5;
        }
        std::fputs("READY\n", stdout);
        std::fflush(stdout);
        const int command = std::getchar();
        QByteArray bytes;
        char chunk[32]{};
        bool readOk = true;
        while (command == 'R' && bytes.size() <= 8) {
            const ssize_t count = ::read(reader, chunk, sizeof(chunk));
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0) {
                readOk = false;
                break;
            }
            if (count == 0)
                break;
            bytes.append(chunk, int(count));
        }
        const bool success = command == 'R' && readOk && bytes == QByteArray("original");
        ::close(reader);
#endif
        if (!success)
            return 7;
        std::fputs("HELD_ORIGINAL\n", stdout);
        std::fflush(stdout);
        return 0;
    }

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
