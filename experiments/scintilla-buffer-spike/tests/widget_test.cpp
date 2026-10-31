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
    QInputMethodEvent previousPreedit(QStringLiteral("previous"), {});
    QCoreApplication::sendEvent(&mixedActive, &previousPreedit);
    if (text(mixedActive) != QByteArray("previous")
        || snapshotForSaveFixture(mixedActive, mixedInactive).status != FixtureSnapshotStatus::Composing)
        return false;
    int callbacksDuringInput = 0;
    bool unsafeCallback = false;
    QObject::connect(&mixedActive, &ScintillaEditBase::notifyChange, &mixedActive, [&] {
        ++callbacksDuringInput;
        const auto reentrantSnapshot = snapshotForSaveFixture(mixedActive, mixedInactive);
        if (reentrantSnapshot.status != FixtureSnapshotStatus::Composing || !reentrantSnapshot.bytes.isEmpty()
            || transferFixture(mixedActive, mixedInactive) != FixtureHandoff::Composing)
            unsafeCallback = true;
    });
    QInputMethodEvent mixedEvent(QStringLiteral("next"),
                                 {QInputMethodEvent::Attribute(QInputMethodEvent::Cursor, 2, 1, QVariant())});
    mixedEvent.setCommitString(QStringLiteral("先"));
    mixedEvent.ignore(); // The fixture must not invent acceptance for the original event.
    QCoreApplication::sendEvent(&mixedActive, &mixedEvent);
    const auto mixedSnapshot = snapshotForSaveFixture(mixedActive, mixedInactive);
    if (mixedEvent.isAccepted() || callbacksDuringInput == 0 || unsafeCallback
        || mixedSnapshot.status != FixtureSnapshotStatus::Composing || !mixedSnapshot.bytes.isEmpty()
        || transferFixture(mixedActive, mixedInactive) != FixtureHandoff::Composing
        || !mixedActive.isEnabled() || mixedInactive.isEnabled()) {
        qCritical() << "Snapshot allowed after an IME event with both commit and preedit; tentative marker:"
                    << mixedActive.send(SCI_GETUNDOTENTATIVE);
        return false;
    }
    if (text(mixedActive) != QStringLiteral("先next").toUtf8()
        || mixedActive.send(SCI_GETCURRENTPOS) != QStringLiteral("先ne").toUtf8().size()) {
        qCritical("Mixed input event did not retain both committed and preedit text");
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

    FixtureScintillaEdit replacementActive;
    FixtureScintillaEdit replacementInactive;
    replacementActive.send(SCI_SETCODEPAGE, SC_CP_UTF8);
    const QByteArray replacementOriginal = QStringLiteral("甲🙂乙").toUtf8();
    replacementActive.send(SCI_ADDTEXT, replacementOriginal.size(),
                           reinterpret_cast<sptr_t>(replacementOriginal.constData()));
    replacementActive.send(SCI_EMPTYUNDOBUFFER);
    replacementInactive.send(SCI_SETDOCPOINTER, 0, replacementActive.send(SCI_GETDOCPOINTER));
    replacementInactive.setEnabled(false);
    replacementActive.send(SCI_GOTOPOS, QStringLiteral("甲🙂").toUtf8().size());
    QInputMethodEvent previousReplacement(QStringLiteral("temporary"), {});
    QCoreApplication::sendEvent(&replacementActive, &previousReplacement);
    if (text(replacementActive) != QStringLiteral("甲🙂temporary乙").toUtf8()
        || snapshotForSaveFixture(replacementActive, replacementInactive).status != FixtureSnapshotStatus::Composing)
        return false;
    QInputMethodEvent replacement(QStringLiteral("续"), {});
    replacement.setCommitString(QStringLiteral("改"), -2, 2);
    QCoreApplication::sendEvent(&replacementActive, &replacement);
    const auto replacementSnapshot = snapshotForSaveFixture(replacementActive, replacementInactive);
    if (text(replacementActive) != QStringLiteral("甲改续乙").toUtf8()
        || replacementSnapshot.status != FixtureSnapshotStatus::Composing
        || !replacementSnapshot.bytes.isEmpty()
        || transferFixture(replacementActive, replacementInactive) != FixtureHandoff::Composing)
        return false;
    QInputMethodEvent replacementCommit;
    replacementCommit.setCommitString(QStringLiteral("终"));
    QCoreApplication::sendEvent(&replacementActive, &replacementCommit);
    const QByteArray replacementFinal = QStringLiteral("甲改终乙").toUtf8();
    if (text(replacementActive) != replacementFinal)
        return false;
    int undoCount = 0;
    while (text(replacementActive) != replacementOriginal && undoCount < 8
           && replacementActive.send(SCI_CANUNDO)) {
        replacementActive.send(SCI_UNDO);
        ++undoCount;
    }
    if (!undoCount || text(replacementActive) != replacementOriginal)
        return false;
    for (int i = 0; i < undoCount; ++i) {
        if (!replacementActive.send(SCI_CANREDO))
            return false;
        replacementActive.send(SCI_REDO);
    }
    if (text(replacementActive) != replacementFinal)
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
