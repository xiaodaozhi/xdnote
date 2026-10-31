#include "snapshot.h"
#include "fixture_editor.h"

#include <QThread>

FixtureSnapshot snapshotForSaveFixture(FixtureScintillaEdit &active, FixtureScintillaEdit &inactive)
{
    if (&active == &inactive || QThread::currentThread() != active.thread()
        || active.thread() != inactive.thread() || !active.isEnabled() || inactive.isEnabled())
        return {};
    const sptr_t document = active.send(SCI_GETDOCPOINTER);
    if (!document || document != inactive.send(SCI_GETDOCPOINTER))
        return {};

    // Both checks are conservative; neither proves all input methods safe.
    if (active.compositionPending() || inactive.compositionPending())
        return {FixtureSnapshotStatus::Composing, {}};

    const sptr_t count = active.send(SCI_GETTEXTLENGTH);
    constexpr sptr_t fixtureLimit = 1024 * 1024;
    if (count < 0 || count > fixtureLimit)
        return {};
    QByteArray bytes(static_cast<qsizetype>(count + 1), '\0');
    const sptr_t copied = active.send(SCI_GETTEXT, static_cast<uptr_t>(count + 1),
                                      reinterpret_cast<sptr_t>(bytes.data()));
    if (copied != count || bytes.at(static_cast<qsizetype>(count)) != '\0'
        || active.send(SCI_GETDOCPOINTER) != document
        || inactive.send(SCI_GETDOCPOINTER) != document
        || active.send(SCI_GETTEXTLENGTH) != count)
        return {};
    if (active.compositionPending() || inactive.compositionPending())
        return {FixtureSnapshotStatus::Composing, {}};
    bytes.resize(static_cast<qsizetype>(count));
    return {FixtureSnapshotStatus::Ready, bytes};
}
