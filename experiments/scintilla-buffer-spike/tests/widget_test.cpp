#include "handoff.h"
#include "ScintillaEditBase.h"

#include <QApplication>
#include <QByteArray>
#include <QInputMethodEvent>
#include <QDebug>

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
    ScintillaEditBase first;
    ScintillaEditBase second;
    first.send(SCI_SETCODEPAGE, SC_CP_UTF8);
    const QByteArray original = QStringLiteral("甲\u2029乙\n丙🙂").toUtf8();
    first.send(SCI_ADDTEXT, original.size(), reinterpret_cast<sptr_t>(original.constData()));
    first.send(SCI_EMPTYUNDOBUFFER);
    const sptr_t document = first.send(SCI_GETDOCPOINTER);
    second.send(SCI_SETDOCPOINTER, 0, document);
    second.setEnabled(false);
    if (text(first) != original || text(second) != original)
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

    second.send(SCI_GOTOPOS, static_cast<uptr_t>(second.send(SCI_GETTEXTLENGTH)));
    QInputMethodEvent preedit(QStringLiteral("pin"), {});
    QCoreApplication::sendEvent(&second, &preedit);
    // This candidate exposes preedit in the shared buffer; do not snapshot or
    // hand off until the composition is resolved. This event is synthetic.
    if (second.send(SCI_GETUNDOTENTATIVE) < 0 || text(first) != original + edit + "pin"
        || transferFixture(second, first) != FixtureHandoff::Composing
        || !second.isEnabled() || first.isEnabled())
        return false;
    QInputMethodEvent commit;
    commit.setCommitString(QStringLiteral("拼"));
    QCoreApplication::sendEvent(&second, &commit);
    const QByteArray committed = original + edit + QStringLiteral("拼").toUtf8();
    if (second.send(SCI_GETUNDOTENTATIVE) >= 0 || text(second) != committed
        || transferFixture(second, first) != FixtureHandoff::Completed
        || text(first) != committed)
        return false;
    first.send(SCI_UNDO);
    if (text(first) != original + edit)
        return false;
    first.send(SCI_REDO);
    if (text(first) != committed)
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
    qInfo("Scintilla Qt widget shared document and synthetic IME experiment passed");
    return 0;
}
