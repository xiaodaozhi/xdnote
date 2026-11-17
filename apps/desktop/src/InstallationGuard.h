#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <appmodel.h>
#include <cwchar>
#endif
#ifdef __linux__
#include <string>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace xdnote {
// Keep an app-running marker throughout startup, forwarding and the GUI loop.
// Setup never terminates windows which may contain unsaved documents.
class InstallationGuard final {
public:
    InstallationGuard(const wchar_t *runningName = L"Local\\xdnote.desktop.running",
                      const wchar_t *installingName = L"Local\\xdnote.desktop.installing") {
#ifdef _WIN32
        UINT32 packageLength = 0;
        if (GetCurrentPackageFullName(&packageLength, nullptr) != APPMODEL_ERROR_NO_PACKAGE && std::wcscmp(runningName, L"Local\\xdnote.desktop.running") == 0) {
            runningName = L"Local\\xdnote.store.running"; installingName = L"Local\\xdnote.store.installing";
        }
        m_running = CreateMutexW(nullptr, FALSE, runningName);
        const auto updating = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\xdnote.desktop.updating");
        const auto updateError = GetLastError();
        const bool clearUpdate = (GetCurrentPackageFullName(&packageLength, nullptr) != APPMODEL_ERROR_NO_PACKAGE) || (!updating && updateError == ERROR_FILE_NOT_FOUND);
        if (updating) CloseHandle(updating);
        const auto installing = OpenMutexW(SYNCHRONIZE, FALSE, installingName);
        m_allowed = m_running && clearUpdate && !installing && GetLastError() == ERROR_FILE_NOT_FOUND;
        if (installing) CloseHandle(installing);
#elif defined(__linux__)
        (void)runningName; (void)installingName;
        char path[4096]; const auto length = readlink("/proc/self/exe", path, sizeof(path) - 1);
        if (length > 0) { path[length] = 0;
            if (std::string(path).rfind("/opt/xdnote/versions/", 0) == 0) {
                m_usage = open("/run/xdnote/usage.lock", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
                struct stat metadata{};
                m_allowed = m_usage >= 0 && fstat(m_usage, &metadata) == 0 && S_ISREG(metadata.st_mode) && metadata.st_uid == 0 && !(metadata.st_mode & 0022) &&
                    flock(m_usage, LOCK_SH | LOCK_NB) == 0 && access("/run/xdnote/installing", F_OK) != 0 && access("/run/xdnote/updating", F_OK) != 0;
            }
        }
#else
        (void)runningName; (void)installingName;
#endif
    }
    ~InstallationGuard() {
#ifdef _WIN32
        if (m_running) CloseHandle(m_running);
#elif defined(__linux__)
        if (m_usage >= 0) close(m_usage);
#endif
    }
    InstallationGuard(const InstallationGuard &) = delete;
    InstallationGuard &operator=(const InstallationGuard &) = delete;
    bool allowed() const { return m_allowed; }
private:
    bool m_allowed = true;
#ifdef _WIN32
    HANDLE m_running = nullptr;
#elif defined(__linux__)
    int m_usage = -1;
#endif
};
}
