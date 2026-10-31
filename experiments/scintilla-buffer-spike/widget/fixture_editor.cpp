#include "fixture_editor.h"

#include <QInputMethodEvent>
#include <QScopedValueRollback>

bool FixtureScintillaEdit::compositionPending() const
{
    return m_dispatchingInputMethod || m_preeditActive || send(SCI_GETUNDOTENTATIVE) >= 0;
}

void FixtureScintillaEdit::inputMethodEvent(QInputMethodEvent *event)
{
    QScopedValueRollback<bool> dispatching(m_dispatchingInputMethod, true);
    m_preeditActive = !event->preeditString().isEmpty();
    if (!event->commitString().isEmpty() && !event->preeditString().isEmpty()) {
        // The tested Scintilla Qt port handles commit and preedit as exclusive
        // branches. Keep both operations within the same guarded dispatch.
        QInputMethodEvent commit;
        commit.setCommitString(event->commitString(), event->replacementStart(), event->replacementLength());
        ScintillaEditBase::inputMethodEvent(&commit);
        QInputMethodEvent preedit(event->preeditString(), event->attributes());
        ScintillaEditBase::inputMethodEvent(&preedit);
        return;
    }
    ScintillaEditBase::inputMethodEvent(event);
}
