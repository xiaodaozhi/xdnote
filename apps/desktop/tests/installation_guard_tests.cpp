#include "InstallationGuard.h"
#include <string>
#include <cstdio>

int main() {
    const auto prefix = L"Local\\xdnote.test.installation." + std::to_wstring(GetCurrentProcessId());
    const auto running = prefix + L".running", installing = prefix + L".installing";
    {
        xdnote::InstallationGuard application(running.c_str(), installing.c_str());
        if (!application.allowed()) return 1;
        auto marker = OpenMutexW(SYNCHRONIZE, FALSE, running.c_str());
        if (!marker) return 2;
        CloseHandle(marker);
        // A forwarded process keeps the same marker alive until it exits.
        xdnote::InstallationGuard forwarded(running.c_str(), installing.c_str());
        if (!forwarded.allowed()) return 3;
    }
    auto marker = OpenMutexW(SYNCHRONIZE, FALSE, running.c_str());
    if (marker) { CloseHandle(marker); return 4; }
    auto setup = CreateMutexW(nullptr, FALSE, installing.c_str());
    if (!setup) return 5;
    {
        xdnote::InstallationGuard blocked(running.c_str(), installing.c_str());
        if (blocked.allowed()) { CloseHandle(setup); return 6; }
    }
    CloseHandle(setup);
    xdnote::InstallationGuard retry(running.c_str(), installing.c_str());
    if (!retry.allowed()) return 7;
    std::puts("installation guard: native marker lifetime, forwarding, install blocking and retry passed");
    return 0;
}
