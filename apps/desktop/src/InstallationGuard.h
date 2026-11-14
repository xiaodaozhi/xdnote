#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace xdnote {
// Keep an app-running marker throughout startup, forwarding and the GUI loop.
// Setup never terminates windows which may contain unsaved documents.
class InstallationGuard final {
public:
    InstallationGuard(const wchar_t *runningName = L"Local\\xdnote.desktop.running",
                      const wchar_t *installingName = L"Local\\xdnote.desktop.installing") {
#ifdef _WIN32
        m_running = CreateMutexW(nullptr, FALSE, runningName);
        const auto installing = OpenMutexW(SYNCHRONIZE, FALSE, installingName);
        m_allowed = m_running && !installing && GetLastError() == ERROR_FILE_NOT_FOUND;
        if (installing) CloseHandle(installing);
#else
        (void)runningName; (void)installingName;
#endif
    }
    ~InstallationGuard() {
#ifdef _WIN32
        if (m_running) CloseHandle(m_running);
#endif
    }
    InstallationGuard(const InstallationGuard &) = delete;
    InstallationGuard &operator=(const InstallationGuard &) = delete;
    bool allowed() const { return m_allowed; }
private:
    bool m_allowed = true;
#ifdef _WIN32
    HANDLE m_running = nullptr;
#endif
};
}
