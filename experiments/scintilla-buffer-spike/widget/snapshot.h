#pragma once

#include <QByteArray>

class FixtureScintillaEdit;

enum class FixtureSnapshotStatus { Ready, Composing, Invalid };

struct FixtureSnapshot
{
    FixtureSnapshotStatus status = FixtureSnapshotStatus::Invalid;
    QByteArray bytes;
};

// Experiment only: called synchronously on the widgets' owning thread for
// one active and one disabled view of the same document. It does not save.
FixtureSnapshot snapshotForSaveFixture(FixtureScintillaEdit &active, FixtureScintillaEdit &inactive);
