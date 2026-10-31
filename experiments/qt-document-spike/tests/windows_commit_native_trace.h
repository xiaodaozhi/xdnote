#pragma once

#include <QString>
#include <QtGlobal>

#ifdef Q_OS_WIN
#include <string>

struct FixtureNativeRenameObservation
{
    bool installed = false;
    bool captureStarted = false;
    bool restored = false;
    unsigned calls = 0;
    unsigned failures = 0;
    quint32 lastFailureError = 0;
    bool ntInstalled = false;
    bool ntRestored = false;
    unsigned ntCalls = 0;
    quint32 ntReturnStatus = 0;
    bool sourceAccessQueried = false;
    quint32 sourceAccessStatus = 0;
    bool sourceHasDeleteAccess = false;
    bool targetAttributesAvailable = false;
    bool targetReadOnly = false;
    bool requestBoundsValid = false;
    bool requestTerminated = false;
    bool requestTargetMatches = false;
    bool sourceInfoAvailable = false;
    bool sourceDeletePending = false;
    bool targetDeleteAttempted = false;
    bool targetDeleteOpened = false;
    quint32 targetDeleteError = 0;
    bool usersRequested = false;
    quint32 usersRegistrationError = 0;
    bool usersQueryAttempted = false;
    quint32 usersQueryError = 0;
    quint32 userCount = 0;
    quint32 otherUserCount = 0;
    bool currentProcessListed = false;
    bool expectedProcessListed = false;
    bool usersSessionEnded = false;
    quint32 usersSessionEndError = 0;
    bool sourcePathValidated = false;
    bool sourceUsersRegistrationAttempted = false;
    quint32 sourceUsersRegistrationError = 0;
    bool sourceUsersQueryAttempted = false;
    quint32 sourceUsersQueryError = 0;
    quint32 sourceUserCount = 0;
    quint32 sourceOtherUserCount = 0;
    bool sourceCurrentProcessListed = false;
    bool sourceUsersSessionEnded = false;
    quint32 sourceUsersSessionEndError = 0;
};

QString describeFixtureNativeRename(const FixtureNativeRenameObservation &observation);

// A diagnostic for this test process and verified self-created fixtures only.
// It records the result, not the path or contents, of Qt Core's native rename.
class FixtureCommitNativeTrace
{
public:
    FixtureCommitNativeTrace(const QString &fixtureRoot, const QString &targetName, bool enabled,
                             bool queryFixtureUsers = false, quint32 expectedFixturePid = 0);
    ~FixtureCommitNativeTrace();
    FixtureCommitNativeTrace(const FixtureCommitNativeTrace &) = delete;
    FixtureCommitNativeTrace &operator=(const FixtureCommitNativeTrace &) = delete;

    bool installed() const { return m_observation.installed; }
    bool usersRegistered() const { return m_usersRegistered; }
    bool start();
    FixtureNativeRenameObservation finish();

private:
    FixtureNativeRenameObservation m_observation;
    void **m_slot = nullptr;
    void *m_original = nullptr;
    void **m_ntSlot = nullptr;
    void *m_ntOriginal = nullptr;
    bool m_finished = false;
    std::wstring m_targetPath;
    std::wstring m_fixtureRoot;
    quint32 m_usersSession = 0;
    quint32 m_expectedFixturePid = 0;
    bool m_usersSessionStarted = false;
    bool m_usersRegistered = false;
};
#endif
