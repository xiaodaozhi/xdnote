#include "windows_commit_native_trace.h"

#ifdef Q_OS_WIN
#include "windows_commit_diagnostic.h"
#include <QDir>

#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstring>

namespace {
using SetFileInfo = decltype(&SetFileInformationByHandle);
std::atomic<SetFileInfo> originalSetFileInfo{nullptr};
std::atomic_flag traceInUse = ATOMIC_FLAG_INIT;
thread_local FixtureNativeRenameObservation *currentObservation = nullptr;
thread_local const wchar_t *currentTarget = nullptr;

BOOL WINAPI tracedSetFileInformationByHandle(HANDLE file, FILE_INFO_BY_HANDLE_CLASS kind,
                                             LPVOID information, DWORD size)
{
    const SetFileInfo original = originalSetFileInfo.load(std::memory_order_acquire);
    const BOOL result = original(file, kind, information, size);
    const DWORD error = GetLastError();
    if (currentObservation && kind == FileRenameInfo) {
        ++currentObservation->calls;
        if (!result) {
            ++currentObservation->failures;
            currentObservation->lastFailureError = error;
            currentObservation->sourceInfoAvailable = false;
            currentObservation->sourceDeletePending = false;
            currentObservation->targetDeleteAttempted = false;
            currentObservation->targetDeleteOpened = false;
            currentObservation->targetDeleteError = 0;
            FILE_STANDARD_INFO info{};
            if (GetFileInformationByHandleEx(file, FileStandardInfo, &info, sizeof(info))) {
                currentObservation->sourceInfoAvailable = true;
                currentObservation->sourceDeletePending = info.DeletePending != FALSE;
            }
            if (currentTarget) {
                currentObservation->targetDeleteAttempted = true;
                HANDLE target = CreateFileW(currentTarget, DELETE | FILE_READ_ATTRIBUTES,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                if (target == INVALID_HANDLE_VALUE) {
                    currentObservation->targetDeleteError = GetLastError();
                } else {
                    currentObservation->targetDeleteOpened = true;
                    CloseHandle(target);
                }
            }
        }
    }
    SetLastError(error);
    return result;
}

void **qtCoreSetFileInfoImport()
{
    const auto base = reinterpret_cast<unsigned char *>(GetModuleHandleW(L"Qt6Core.dll"));
    if (!base)
        return nullptr;
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
        return nullptr;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return nullptr;
    const auto imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress || !imports.Size)
        return nullptr;
    auto descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR *>(base + imports.VirtualAddress);
    const auto end = reinterpret_cast<const unsigned char *>(descriptor) + imports.Size;
    for (; reinterpret_cast<const unsigned char *>(descriptor + 1) <= end && descriptor->Name; ++descriptor) {
        if (_stricmp(reinterpret_cast<const char *>(base + descriptor->Name), "KERNEL32.dll") != 0
            || !descriptor->OriginalFirstThunk || !descriptor->FirstThunk)
            continue;
        auto names = reinterpret_cast<const IMAGE_THUNK_DATA *>(base + descriptor->OriginalFirstThunk);
        auto importSlot = reinterpret_cast<IMAGE_THUNK_DATA *>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++importSlot) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            const auto name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char *>(name->Name), "SetFileInformationByHandle") == 0)
                return reinterpret_cast<void **>(&importSlot->u1.Function);
        }
    }
    return nullptr;
}

bool exchangeImport(void **slot, void *expected, void *replacement)
{
    DWORD protection = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection))
        return false;
    void *previous = InterlockedCompareExchangePointer(reinterpret_cast<void *volatile *>(slot),
                                                        replacement, expected);
    DWORD ignored = 0;
    const bool protectionRestored = VirtualProtect(slot, sizeof(*slot), protection, &ignored) != 0;
    if (previous == expected && !protectionRestored) {
        // The page is still writable; put the import back before reporting failure.
        InterlockedCompareExchangePointer(reinterpret_cast<void *volatile *>(slot), expected, replacement);
        VirtualProtect(slot, sizeof(*slot), protection, &ignored);
    }
    return previous == expected && protectionRestored;
}
} // namespace

FixtureCommitNativeTrace::FixtureCommitNativeTrace(const QString &fixtureRoot, const QString &targetName,
                                                   bool enabled)
{
    if (!enabled || fixtureOtherEntryCount(fixtureRoot, targetName) < 0 || traceInUse.test_and_set())
        return;
    const QString root = QDir(fixtureRoot).canonicalPath();
    if (root.isEmpty() || fixtureOtherEntryCount(root, targetName) < 0) {
        traceInUse.clear();
        return;
    }
    m_targetPath = QDir::toNativeSeparators(root + QLatin1Char('/') + targetName).toStdWString();
    m_slot = qtCoreSetFileInfoImport();
    if (!m_slot) {
        traceInUse.clear();
        return;
    }
    m_original = *m_slot;
    if (!m_original) {
        m_slot = nullptr;
        traceInUse.clear();
        return;
    }
    originalSetFileInfo.store(reinterpret_cast<SetFileInfo>(m_original), std::memory_order_release);
    if (!exchangeImport(m_slot, m_original, reinterpret_cast<void *>(&tracedSetFileInformationByHandle))) {
        const bool stillWrapped = *m_slot == reinterpret_cast<void *>(&tracedSetFileInformationByHandle);
        m_slot = nullptr;
        if (!stillWrapped)
            traceInUse.clear();
        return;
    }
    m_observation.installed = true;
}

FixtureCommitNativeTrace::~FixtureCommitNativeTrace()
{
    finish();
}

bool FixtureCommitNativeTrace::start()
{
    if (!m_observation.installed || m_finished || currentObservation)
        return false;
    currentObservation = &m_observation;
    currentTarget = m_targetPath.c_str();
    m_observation.captureStarted = true;
    return true;
}

FixtureNativeRenameObservation FixtureCommitNativeTrace::finish()
{
    if (!m_finished && m_observation.installed) {
        if (currentObservation == &m_observation) {
            currentObservation = nullptr;
            currentTarget = nullptr;
        }
        m_observation.restored = exchangeImport(m_slot, reinterpret_cast<void *>(&tracedSetFileInformationByHandle),
                                                m_original);
        if (m_observation.restored)
            traceInUse.clear(); // Otherwise never mistake the wrapper for the original import.
    }
    m_finished = true;
    return m_observation;
}

QString describeFixtureNativeRename(const FixtureNativeRenameObservation &observation)
{
    return QStringLiteral("fixture native rename trace: installed=%1; capture started=%2; restored=%3; calls=%4; failures=%5; last failure error=%6; "
                          "source info=%7; source delete pending=%8; immediate target DELETE probe attempted=%9; opened=%10; error=%11")
        .arg(observation.installed).arg(observation.captureStarted).arg(observation.restored).arg(observation.calls)
        .arg(observation.failures).arg(observation.lastFailureError)
        .arg(observation.sourceInfoAvailable).arg(observation.sourceDeletePending)
        .arg(observation.targetDeleteAttempted).arg(observation.targetDeleteOpened).arg(observation.targetDeleteError);
}
#endif
