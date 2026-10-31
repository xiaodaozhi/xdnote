#pragma once

#include "ScintillaEditBase.h"

class QInputMethodEvent;

// Experiment-only wrapper: tracks the last input-method event and prevents
// snapshots and handoffs while that event is still being dispatched.
class FixtureScintillaEdit final : public ScintillaEditBase
{
public:
    using ScintillaEditBase::ScintillaEditBase;
    bool compositionPending() const;

protected:
    void inputMethodEvent(QInputMethodEvent *event) override;

private:
    bool m_dispatchingInputMethod = false;
    bool m_preeditActive = false;
};
