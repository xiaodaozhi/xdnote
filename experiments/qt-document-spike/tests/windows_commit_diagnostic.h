#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#ifdef Q_OS_WIN
struct FixtureCommitObservation
{
    bool valid = false;
    QString targetState;
    quint32 targetNativeError = 0;
    int otherEntries = -1;
    bool deleteOpenAttempted = false;
    bool deleteOpenSucceeded = false;
    quint32 deleteOpenNativeError = 0;
};

// Only accepts a test-owned spike-test-data/fixture-XXXXXX directory and a
// known fixture target name; it never accepts arbitrary relative paths.
int fixtureOtherEntryCount(const QString &fixtureRoot, const QString &targetName = QStringLiteral("note.md"));
FixtureCommitObservation observeFixtureCommitFailure(const QString &fixtureRoot,
                                                     const QByteArray *baseline = nullptr,
                                                     const QByteArray *attempted = nullptr,
                                                     const QString &targetName = QStringLiteral("note.md"));
QString describeFixtureCommitFailure(const FixtureCommitObservation &observation,
                                     int beforeOpenEntries, int afterOpenEntries);
#endif
