#include "windows_commit_native_trace.h"

#ifdef Q_OS_WIN
#include "windows_commit_diagnostic.h"
#include <QDir>

#define NOMINMAX
#include <windows.h>
#include <RestartManager.h>

#include <array>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <iterator>

namespace {
using SetFileInfo = decltype(&SetFileInformationByHandle);
std::atomic<SetFileInfo> originalSetFileInfo{nullptr};
std::atomic_flag traceInUse = ATOMIC_FLAG_INIT;
thread_local FixtureNativeRenameObservation *currentObservation = nullptr;
thread_local const wchar_t *currentTarget = nullptr;
thread_local const wchar_t *currentFixtureRoot = nullptr;
thread_local DWORD currentUsersSession = 0;
thread_local DWORD currentExpectedFixturePid = 0;
thread_local bool currentUsersRegistered = false;

struct FixtureUsers
{
    DWORD error = ERROR_SUCCESS;
    UINT count = 0;
    UINT others = 0;
    bool current = false;
    bool expected = false;
};

FixtureUsers queryFixtureUsers(DWORD session, DWORD expectedPid)
{
    FixtureUsers result;
    std::array<RM_PROCESS_INFO, 64> entries{};
    UINT needed = 0;
    UINT count = UINT(entries.size());
    DWORD reason = 0;
    result.error = RmGetList(session, &needed, &count, entries.data(), &reason);
    if (result.error != ERROR_SUCCESS)
        return result; // An incomplete or unsupported snapshot is not an empty one.
    result.count = count;
    for (UINT i = 0; i < count; ++i) {
        const DWORD id = entries[i].Process.dwProcessId;
        if (id == GetCurrentProcessId())
            result.current = true;
        else
            ++result.others;
        if (expectedPid && id == expectedPid)
            result.expected = true;
    }
    return result;
}

void observeFixtureUsers(FixtureNativeRenameObservation *observation)
{
    if (!currentUsersRegistered)
        return;
    observation->usersQueryAttempted = true;
    const auto users = queryFixtureUsers(currentUsersSession, currentExpectedFixturePid);
    observation->usersQueryError = users.error;
    observation->userCount = users.count;
    observation->otherUserCount = users.others;
    observation->currentProcessListed = users.current;
    observation->expectedProcessListed = users.expected;
}

void observeSourceFixtureUsers(HANDLE file, FixtureNativeRenameObservation *observation)
{
    if (!currentUsersRegistered || !currentFixtureRoot || !currentTarget || GetFileType(file) != FILE_TYPE_DISK)
        return;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info)
        || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        return;
    wchar_t path[32768]{};
    const DWORD length = GetFinalPathNameByHandleW(file, path, DWORD(std::size(path)),
                                                   FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!length || length >= std::size(path) || std::wcsncmp(path, L"\\\\?\\", 4) != 0)
        return;
    const wchar_t *source = path + 4;
    const size_t rootLength = std::wcslen(currentFixtureRoot);
    if (std::wcslen(source) <= rootLength + 1
        || CompareStringOrdinal(source, int(rootLength), currentFixtureRoot, int(rootLength), TRUE) != CSTR_EQUAL
        || (source[rootLength] != L'\\' && source[rootLength] != L'/'))
        return;
    const wchar_t *name = source + rootLength + 1;
    if (std::wcschr(name, L'\\') || std::wcschr(name, L'/') || _wcsicmp(source, currentTarget) == 0)
        return;
    observation->sourcePathValidated = true;
    observation->sourceUsersRegistrationAttempted = true;
    DWORD session = 0;
    wchar_t key[CCH_RM_SESSION_KEY + 1]{};
    DWORD status = RmStartSession(&session, 0, key);
    if (status == ERROR_SUCCESS) {
        status = RmRegisterResources(session, 1, &source, 0, nullptr, 0, nullptr);
        if (status == ERROR_SUCCESS) {
            observation->sourceUsersQueryAttempted = true;
            const auto users = queryFixtureUsers(session, 0);
            observation->sourceUsersQueryError = users.error;
            observation->sourceUserCount = users.count;
            observation->sourceOtherUserCount = users.others;
            observation->sourceCurrentProcessListed = users.current;
        }
        observation->sourceUsersSessionEndError = RmEndSession(session);
        observation->sourceUsersSessionEnded = observation->sourceUsersSessionEndError == ERROR_SUCCESS;
    }
    observation->sourceUsersRegistrationError = status;
}

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
            currentObservation->usersQueryAttempted = false;
            currentObservation->usersQueryError = 0;
            currentObservation->userCount = 0;
            currentObservation->otherUserCount = 0;
            currentObservation->currentProcessListed = false;
            currentObservation->expectedProcessListed = false;
            currentObservation->sourcePathValidated = false;
            currentObservation->sourceUsersRegistrationAttempted = false;
            currentObservation->sourceUsersRegistrationError = 0;
            currentObservation->sourceUsersQueryAttempted = false;
            currentObservation->sourceUsersQueryError = 0;
            currentObservation->sourceUserCount = 0;
            currentObservation->sourceOtherUserCount = 0;
            currentObservation->sourceCurrentProcessListed = false;
            currentObservation->sourceUsersSessionEnded = false;
            currentObservation->sourceUsersSessionEndError = 0;
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
            observeFixtureUsers(currentObservation);
            observeSourceFixtureUsers(file, currentObservation);
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
                                                   bool enabled, bool queryFixtureUsers,
                                                   quint32 expectedFixturePid)
{
    if (!enabled || fixtureOtherEntryCount(fixtureRoot, targetName) < 0 || traceInUse.test_and_set())
        return;
    const QString root = QDir(fixtureRoot).canonicalPath();
    if (root.isEmpty() || fixtureOtherEntryCount(root, targetName) < 0) {
        traceInUse.clear();
        return;
    }
    m_fixtureRoot = QDir::toNativeSeparators(root).toStdWString();
    m_targetPath = QDir::toNativeSeparators(root + QLatin1Char('/') + targetName).toStdWString();
    m_observation.usersRequested = queryFixtureUsers;
    m_expectedFixturePid = expectedFixturePid;
    if (queryFixtureUsers) {
        wchar_t key[CCH_RM_SESSION_KEY + 1]{};
        DWORD session = 0;
        DWORD status = RmStartSession(&session, 0, key);
        if (status == ERROR_SUCCESS) {
            m_usersSessionStarted = true;
            m_usersSession = session;
            const wchar_t *target = m_targetPath.c_str();
            status = RmRegisterResources(session, 1, &target, 0, nullptr, 0, nullptr);
            m_usersRegistered = status == ERROR_SUCCESS;
        }
        m_observation.usersRegistrationError = status;
    }
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
    currentFixtureRoot = m_fixtureRoot.c_str();
    currentUsersSession = m_usersSession;
    currentUsersRegistered = m_usersRegistered;
    currentExpectedFixturePid = m_expectedFixturePid;
    m_observation.captureStarted = true;
    return true;
}

FixtureNativeRenameObservation FixtureCommitNativeTrace::finish()
{
    if (!m_finished && m_observation.installed) {
        if (currentObservation == &m_observation) {
            currentObservation = nullptr;
            currentTarget = nullptr;
            currentFixtureRoot = nullptr;
            currentUsersRegistered = false;
            currentUsersSession = 0;
            currentExpectedFixturePid = 0;
        }
        m_observation.restored = exchangeImport(m_slot, reinterpret_cast<void *>(&tracedSetFileInformationByHandle),
                                                m_original);
        if (m_observation.restored)
            traceInUse.clear(); // Otherwise never mistake the wrapper for the original import.
    }
    if (!m_finished && m_usersSessionStarted) {
        m_observation.usersSessionEndError = RmEndSession(m_usersSession);
        m_observation.usersSessionEnded = m_observation.usersSessionEndError == ERROR_SUCCESS;
    }
    m_finished = true;
    return m_observation;
}

QString describeFixtureNativeRename(const FixtureNativeRenameObservation &observation)
{
    const QString result = QStringLiteral("fixture native rename trace: installed=%1; capture started=%2; restored=%3; calls=%4; failures=%5; last failure error=%6; "
                          "source info=%7; source delete pending=%8; immediate target DELETE probe attempted=%9; opened=%10; error=%11")
        .arg(observation.installed).arg(observation.captureStarted).arg(observation.restored).arg(observation.calls)
        .arg(observation.failures).arg(observation.lastFailureError)
        .arg(observation.sourceInfoAvailable).arg(observation.sourceDeletePending)
        .arg(observation.targetDeleteAttempted).arg(observation.targetDeleteOpened).arg(observation.targetDeleteError);
    if (!observation.usersRequested)
        return result;
    return result + QStringLiteral("; fixture users registration error=%1; queried=%2; query error=%3; "
                                   "count=%4; other count=%5; current process listed=%6; expected fixture process listed=%7; "
                                   "session ended=%8; end error=%9")
                        .arg(observation.usersRegistrationError).arg(observation.usersQueryAttempted)
                        .arg(observation.usersQueryError).arg(observation.userCount).arg(observation.otherUserCount)
                        .arg(observation.currentProcessListed).arg(observation.expectedProcessListed)
                        .arg(observation.usersSessionEnded).arg(observation.usersSessionEndError)
           + QStringLiteral("; source path validated=%1; registration attempted=%2; registration error=%3; queried=%4; "
                            "query error=%5; count=%6; other count=%7; current process listed=%8; session ended=%9; end error=%10")
                 .arg(observation.sourcePathValidated).arg(observation.sourceUsersRegistrationAttempted)
                 .arg(observation.sourceUsersRegistrationError).arg(observation.sourceUsersQueryAttempted)
                 .arg(observation.sourceUsersQueryError).arg(observation.sourceUserCount)
                 .arg(observation.sourceOtherUserCount).arg(observation.sourceCurrentProcessListed)
                 .arg(observation.sourceUsersSessionEnded).arg(observation.sourceUsersSessionEndError);
}
#endif
