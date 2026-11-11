#pragma once

#include "ScintillaEditBase.h"
#include <QImage>
#include <QPointer>

namespace xdnote {
class DocumentSession;
class Appearance;

class SourceEditor final : public ScintillaEditBase {
    Q_OBJECT
public:
    explicit SourceEditor(DocumentSession *session, QWidget *parent = nullptr, Appearance *appearance = nullptr);
    DocumentSession *session() const;
    bool compositionPending() const;
    bool dispatching() const { return m_inputDepth != 0; }
    static void installAccessibility();
    void setTextZoom(int percent);
    void acknowledgeNavigation();

signals:
    void imagePasteRequested(QImage image);
    void verticalNavigationRequested();

protected:
    void inputMethodEvent(QInputMethodEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

private:
    void applyAppearance();
    void restyle();
    QPointer<Appearance> m_appearance;
    QPointer<DocumentSession> m_session;
    int m_inputDepth = 0;
    bool m_preedit = false;
    bool m_inIme = false;
    int m_textZoom = 100;
    qintptr m_lastAnchor = 0, m_lastCursor = 0;
};
}
