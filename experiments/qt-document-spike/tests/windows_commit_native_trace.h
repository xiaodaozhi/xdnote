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
    bool sourceInfoAvailable = false;
    bool sourceDeletePending = false;
    bool targetDeleteAttempted = false;
    bool targetDeleteOpened = false;
    quint32 targetDeleteError = 0;
};

QString describeFixtureNativeRename(const FixtureNativeRenameObservation &observation);

// A diagnostic for this test process and verified self-created fixtures only.
// It records the result, not the path or contents, of Qt Core's native rename.
class FixtureCommitNativeTrace
{
public:
    FixtureCommitNativeTrace(const QString &fixtureRoot, const QString &targetName, bool enabled);
    ~FixtureCommitNativeTrace();
    FixtureCommitNativeTrace(const FixtureCommitNativeTrace &) = delete;
    FixtureCommitNativeTrace &operator=(const FixtureCommitNativeTrace &) = delete;

    bool installed() const { return m_observation.installed; }
    bool start();
    FixtureNativeRenameObservation finish();

private:
    FixtureNativeRenameObservation m_observation;
    void **m_slot = nullptr;
    void *m_original = nullptr;
    bool m_finished = false;
    std::wstring m_targetPath;
};
#endif
