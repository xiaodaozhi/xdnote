#pragma once

#include <QByteArray>
#include <QFutureWatcher>
#include <QPointer>
#include <QPointF>
#include <QWidget>

class QWebEngineView;

namespace xdnote {
class DocumentSession;
class Appearance;
struct RenderResult { QByteArray html; QString error; quint64 generation = 0; quint64 revision = 0; };

class MarkdownPreview final : public QWidget {
    Q_OBJECT
public:
    explicit MarkdownPreview(DocumentSession *session, QWidget *parent = nullptr, int padding = 24, Appearance *appearance = nullptr);
    ~MarkdownPreview() override;
    QWebEngineView *view() const { return m_view; }
    quint64 requestedGeneration() const { return m_generation; }
    quint64 displayedRevision() const { return m_displayedRevision; }
    void refresh();
    QPointF readingPosition() const { return m_scroll; }
    void restoreReadingPosition(const QPointF &position);
signals:
    void previewReady(quint64 revision);
    void previewError(const QString &message);
    void copyTextRequested(const QString &text);
private:
    void startPending();
    QPointer<DocumentSession> m_session;
    QWebEngineView *m_view = nullptr;
    QFutureWatcher<RenderResult> m_worker;
    QByteArray m_pending;
    quint64 m_pendingRevision = 0;
    quint64 m_generation = 0;
    quint64 m_presentingGeneration = 0;
    quint64 m_presentingRevision = 0;
    quint64 m_displayedRevision = 0;
    bool m_hasPending = false;
    bool m_running = false;
    bool m_closed = false;
    int m_padding = 24;
    QPointer<Appearance> m_appearance;
    QPointF m_scroll;
    bool m_restoring = false;
};
}
