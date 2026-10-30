#include "handoff.h"
#include "ScintillaEditBase.h"

FixtureHandoff transferFixture(ScintillaEditBase &from, ScintillaEditBase &to)
{
    if (&from == &to || !from.isEnabled() || to.isEnabled()
        || from.send(SCI_GETDOCPOINTER) != to.send(SCI_GETDOCPOINTER))
        return FixtureHandoff::Failed;
    // The observed Qt port exposes tentative IME text in the document buffer.
    if (from.send(SCI_GETUNDOTENTATIVE) >= 0)
        return FixtureHandoff::Composing;

    const sptr_t anchor = from.send(SCI_GETANCHOR);
    const sptr_t cursor = from.send(SCI_GETCURRENTPOS);
    if (anchor < 0 || cursor < 0)
        return FixtureHandoff::Failed;
    from.setEnabled(false);
    to.setEnabled(true);
    to.setFocus(Qt::OtherFocusReason);
    to.send(SCI_SETSEL, static_cast<uptr_t>(anchor), cursor);
    if (to.send(SCI_GETANCHOR) == anchor && to.send(SCI_GETCURRENTPOS) == cursor)
        return FixtureHandoff::Completed;

    to.setEnabled(false);
    from.setEnabled(true);
    from.setFocus(Qt::OtherFocusReason);
    from.send(SCI_SETSEL, static_cast<uptr_t>(anchor), cursor);
    return FixtureHandoff::Failed;
}
