#pragma once

#include <QDir>
#include <QFile>
#include <cstring>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fileFixtures {
inline bool write(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush();
}
inline bool hardLink(const QString &source, const QString &alias) {
#ifdef Q_OS_WIN
    return CreateHardLinkW(reinterpret_cast<LPCWSTR>(alias.utf16()), reinterpret_cast<LPCWSTR>(source.utf16()), nullptr);
#else
    return ::link(source.toUtf8().constData(), alias.toUtf8().constData()) == 0;
#endif
}

#ifdef Q_OS_WIN
// Only removes the owned directory entry, never traverses the junction target.
class Junction final {
public:
    ~Junction() { if (!m_path.isEmpty()) RemoveDirectoryW(reinterpret_cast<LPCWSTR>(m_path.utf16())); }
    bool create(const QString &path, const QString &target) {
        if (!QDir().mkdir(path)) return false;
        m_path = QDir::toNativeSeparators(path);
        const auto display = QDir::toNativeSeparators(target);
        const auto substitute = QStringLiteral("\\??\\") + display;
        QByteArray data(16 + (substitute.size() + display.size() + 2) * 2, '\0');
        struct Header { DWORD tag; WORD length; WORD reserved; WORD subOffset; WORD subLength; WORD printOffset; WORD printLength; };
        static_assert(sizeof(Header) == 16);
        auto *header = reinterpret_cast<Header *>(data.data());
        header->tag = IO_REPARSE_TAG_MOUNT_POINT;
        header->length = WORD(data.size() - 8);
        header->subLength = WORD(substitute.size() * 2);
        header->printOffset = WORD((substitute.size() + 1) * 2);
        header->printLength = WORD(display.size() * 2);
        memcpy(data.data() + 16, substitute.utf16(), size_t(header->subLength));
        memcpy(data.data() + 16 + header->printOffset, display.utf16(), size_t(header->printLength));
        HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(m_path.utf16()), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return false;
        DWORD count = 0;
        const bool ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, data.data(), DWORD(data.size()), nullptr, 0, &count, nullptr);
        CloseHandle(handle);
        return ok;
    }
private:
    QString m_path;
};

class ReadBlock final {
public:
    explicit ReadBlock(const QString &path) {
        m_handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_WRITE,
            FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    ~ReadBlock() { if (valid()) CloseHandle(m_handle); }
    bool valid() const { return m_handle != INVALID_HANDLE_VALUE; }
private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
};
#endif
}
