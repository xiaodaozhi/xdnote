#pragma once

#include "ScintillaEditBase.h"
#include <QPointer>

namespace xdnote {
class DocumentSession;

class SourceEditor final : public ScintillaEditBase {
    Q_OBJECT
public:
    explicit SourceEditor(DocumentSession *session, QWidget *parent = nullptr);
    DocumentSession *session() const { return m_session; }
    bool compositionPending() const;
    bool dispatching() const { return m_inputDepth != 0; }
    static void installAccessibility();

protected:
    void inputMethodEvent(QInputMethodEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    QPointer<DocumentSession> m_session;
    int m_inputDepth = 0;
    bool m_preedit = false;
    bool m_inIme = false;
};
}
