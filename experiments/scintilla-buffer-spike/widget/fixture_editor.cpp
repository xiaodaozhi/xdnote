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
    ScintillaEditBase::inputMethodEvent(event);
}
