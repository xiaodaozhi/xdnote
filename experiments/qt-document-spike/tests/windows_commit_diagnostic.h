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

// Only accepts a test-owned spike-test-data/fixture-XXXXXX directory.
int fixtureOtherEntryCount(const QString &fixtureRoot);
FixtureCommitObservation observeFixtureCommitFailure(const QString &fixtureRoot,
                                                     const QByteArray *baseline = nullptr,
                                                     const QByteArray *attempted = nullptr);
QString describeFixtureCommitFailure(const FixtureCommitObservation &observation,
                                     int beforeOpenEntries, int afterOpenEntries);
#endif
