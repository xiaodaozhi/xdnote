#include "handoff.h"
#include "fixture_editor.h"
#include "snapshot.h"

#include <QApplication>
#include <QByteArray>
#include <QInputMethodEvent>
#include <QDebug>

#include <thread>

namespace {
QByteArray text(ScintillaEditBase &view)
{
    const sptr_t count = view.send(SCI_GETTEXTLENGTH);
    QByteArray result(static_cast<qsizetype>(count + 1), '\0');
    view.send(SCI_GETTEXT, static_cast<uptr_t>(count + 1), reinterpret_cast<sptr_t>(result.data()));
    result.resize(static_cast<qsizetype>(count));
    return result;
}

bool run()
{
    FixtureScintillaEdit first;
    FixtureScintillaEdit second;
    first.send(SCI_SETCODEPAGE, SC_CP_UTF8);
    const QByteArray original = QStringLiteral("甲\u2029乙\n丙🙂").toUtf8();
    first.send(SCI_ADDTEXT, original.size(), reinterpret_cast<sptr_t>(original.constData()));
    first.send(SCI_EMPTYUNDOBUFFER);
    const sptr_t document = first.send(SCI_GETDOCPOINTER);
    second.send(SCI_SETDOCPOINTER, 0, document);
    second.setEnabled(false);
    if (text(first) != original || text(second) != original)
        return false;
    const auto initialSnapshot = snapshotForSaveFixture(first, second);
    const auto sameView = snapshotForSaveFixture(first, first);
    const auto inactiveView = snapshotForSaveFixture(second, first);
    if (initialSnapshot.status != FixtureSnapshotStatus::Ready || initialSnapshot.bytes != original
        || sameView.status != FixtureSnapshotStatus::Invalid || !sameView.bytes.isEmpty()
        || inactiveView.status != FixtureSnapshotStatus::Invalid || !inactiveView.bytes.isEmpty())
        return false;
    FixtureSnapshot wrongThread;
    FixtureHandoff wrongThreadHandoff = FixtureHandoff::Completed;
    std::thread worker([&] {
        wrongThread = snapshotForSaveFixture(first, second);
        wrongThreadHandoff = transferFixture(first, second);
    });
    worker.join();
    if (wrongThread.status != FixtureSnapshotStatus::Invalid || !wrongThread.bytes.isEmpty()
        || wrongThreadHandoff != FixtureHandoff::Failed)
        return false;

    const QByteArray edit = QStringLiteral("丁").toUtf8();
    first.send(SCI_APPENDTEXT, edit.size(), reinterpret_cast<sptr_t>(edit.constData()));
    const sptr_t anchor = QStringLiteral("甲\u2029").toUtf8().size();
    first.send(SCI_SETSEL, static_cast<uptr_t>(anchor), 0);
    if (transferFixture(first, second) != FixtureHandoff::Completed
        || second.send(SCI_GETANCHOR) != anchor || second.send(SCI_GETCURRENTPOS) != 0
        || text(second) != original + edit)
        return false;
    second.send(SCI_UNDO);
    if (text(first) != original)
        return false;
    second.send(SCI_REDO);
    if (text(first) != original + edit)
        return false;

    // Scintilla's read-only flag belongs to the shared document, not a view.
    second.send(SCI_SETREADONLY, 1);
    const bool sharedReadOnly = first.send(SCI_GETREADONLY) != 0;
    second.send(SCI_SETREADONLY, 0);
    if (!sharedReadOnly)
        return false;

    const auto beforeComposition = snapshotForSaveFixture(second, first);
    if (beforeComposition.status != FixtureSnapshotStatus::Ready || beforeComposition.bytes != original + edit)
        return false;
    second.send(SCI_GOTOPOS, static_cast<uptr_t>(second.send(SCI_GETTEXTLENGTH)));
    QInputMethodEvent preedit(QStringLiteral("pin"), {});
    QCoreApplication::sendEvent(&second, &preedit);
    // This candidate exposes preedit in the shared buffer; do not snapshot or
    // hand off until the composition is resolved. This event is synthetic.
    const auto duringComposition = snapshotForSaveFixture(second, first);
    if (second.send(SCI_GETUNDOTENTATIVE) < 0 || text(first) != original + edit + "pin"
        || duringComposition.status != FixtureSnapshotStatus::Composing || !duringComposition.bytes.isEmpty()
        || transferFixture(second, first) != FixtureHandoff::Composing
        || !second.isEnabled() || first.isEnabled())
        return false;
    QInputMethodEvent update(QStringLiteral("pinyin"), {});
    QCoreApplication::sendEvent(&second, &update);
    const auto duringUpdate = snapshotForSaveFixture(second, first);
    if (text(first) != original + edit + "pinyin"
        || duringUpdate.status != FixtureSnapshotStatus::Composing || !duringUpdate.bytes.isEmpty())
        return false;
    QInputMethodEvent commit;
    commit.setCommitString(QStringLiteral("拼"));
    QCoreApplication::sendEvent(&second, &commit);
    const QByteArray committed = original + edit + QStringLiteral("拼").toUtf8();
    const auto afterCommit = snapshotForSaveFixture(second, first);
    if (second.send(SCI_GETUNDOTENTATIVE) >= 0 || text(second) != committed
        || afterCommit.status != FixtureSnapshotStatus::Ready || afterCommit.bytes != committed
        || transferFixture(second, first) != FixtureHandoff::Completed
        || text(first) != committed)
        return false;
    const auto afterTransfer = snapshotForSaveFixture(first, second);
    if (afterTransfer.status != FixtureSnapshotStatus::Ready || afterTransfer.bytes != committed)
        return false;
    first.send(SCI_UNDO);
    if (text(first) != original + edit)
        return false;
    first.send(SCI_REDO);
    if (text(first) != committed)
        return false;

    first.send(SCI_GOTOPOS, static_cast<uptr_t>(first.send(SCI_GETTEXTLENGTH)));
    QInputMethodEvent cancelledPreedit(QStringLiteral("zhong"), {});
    QCoreApplication::sendEvent(&first, &cancelledPreedit);
    const auto beforeCancel = snapshotForSaveFixture(first, second);
    if (beforeCancel.status != FixtureSnapshotStatus::Composing || !beforeCancel.bytes.isEmpty())
        return false;
    QInputMethodEvent cancel;
    QCoreApplication::sendEvent(&first, &cancel);
    const auto afterCancel = snapshotForSaveFixture(first, second);
    if (afterCancel.status != FixtureSnapshotStatus::Ready || afterCancel.bytes != committed
        || text(first) != committed)
        return false;

    FixtureScintillaEdit binaryActive;
    FixtureScintillaEdit binaryInactive;
    const QByteArray binary("a\0b", 3);
    binaryActive.send(SCI_ADDTEXT, binary.size(), reinterpret_cast<sptr_t>(binary.constData()));
    binaryInactive.send(SCI_SETDOCPOINTER, 0, binaryActive.send(SCI_GETDOCPOINTER));
    binaryInactive.setEnabled(false);
    const auto binarySnapshot = snapshotForSaveFixture(binaryActive, binaryInactive);
    if (binarySnapshot.status != FixtureSnapshotStatus::Ready || binarySnapshot.bytes != binary)
        return false;
    const auto unrelatedDocument = snapshotForSaveFixture(first, binaryInactive);
    if (unrelatedDocument.status != FixtureSnapshotStatus::Invalid || !unrelatedDocument.bytes.isEmpty())
        return false;
    const QByteArray oversized(1024 * 1024, 'x');
    binaryActive.send(SCI_APPENDTEXT, oversized.size(), reinterpret_cast<sptr_t>(oversized.constData()));
    const auto tooLarge = snapshotForSaveFixture(binaryActive, binaryInactive);
    if (tooLarge.status != FixtureSnapshotStatus::Invalid || !tooLarge.bytes.isEmpty())
        return false;

    FixtureScintillaEdit mixedActive;
    FixtureScintillaEdit mixedInactive;
    mixedInactive.send(SCI_SETDOCPOINTER, 0, mixedActive.send(SCI_GETDOCPOINTER));
    mixedInactive.setEnabled(false);
    int callbacksDuringInput = 0;
    bool unsafeCallback = false;
    QObject::connect(&mixedActive, &ScintillaEditBase::notifyChange, &mixedActive, [&] {
        ++callbacksDuringInput;
        const auto reentrantSnapshot = snapshotForSaveFixture(mixedActive, mixedInactive);
        if (reentrantSnapshot.status != FixtureSnapshotStatus::Composing || !reentrantSnapshot.bytes.isEmpty()
            || transferFixture(mixedActive, mixedInactive) != FixtureHandoff::Composing)
            unsafeCallback = true;
    });
    QInputMethodEvent mixedEvent(QStringLiteral("next"), {});
    mixedEvent.setCommitString(QStringLiteral("先"));
    QCoreApplication::sendEvent(&mixedActive, &mixedEvent);
    const auto mixedSnapshot = snapshotForSaveFixture(mixedActive, mixedInactive);
    if (callbacksDuringInput == 0 || unsafeCallback
        || mixedSnapshot.status != FixtureSnapshotStatus::Composing || !mixedSnapshot.bytes.isEmpty()
        || transferFixture(mixedActive, mixedInactive) != FixtureHandoff::Composing
        || !mixedActive.isEnabled() || mixedInactive.isEnabled()) {
        qCritical() << "Snapshot allowed after an IME event with both commit and preedit; tentative marker:"
                    << mixedActive.send(SCI_GETUNDOTENTATIVE);
        return false;
    }
    QInputMethodEvent continued(QStringLiteral("next"), {});
    QCoreApplication::sendEvent(&mixedActive, &continued);
    if (snapshotForSaveFixture(mixedActive, mixedInactive).status != FixtureSnapshotStatus::Composing)
        return false;
    QInputMethodEvent finishMixed;
    finishMixed.setCommitString(QStringLiteral("后"));
    QCoreApplication::sendEvent(&mixedActive, &finishMixed);
    const auto mixedCommitted = snapshotForSaveFixture(mixedActive, mixedInactive);
    if (unsafeCallback || mixedCommitted.status != FixtureSnapshotStatus::Ready
        || mixedCommitted.bytes != QStringLiteral("先后").toUtf8()
        || transferFixture(mixedActive, mixedInactive) != FixtureHandoff::Completed)
        return false;
    return true;
}
} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    if (!run()) {
        qCritical("Scintilla Qt widget experiment failed");
        return 1;
    }
    qInfo("Scintilla Qt widget shared document, synthetic IME and guarded snapshot experiment passed");
    return 0;
}
