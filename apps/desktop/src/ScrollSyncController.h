#pragma once
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUuid>

namespace xdnote {
class SourceEditor;
class MarkdownPreview;
class NativePreviewView;
class DocumentSession;
class ScrollSyncController final : public QObject {
    Q_OBJECT
public:
    ScrollSyncController(SourceEditor *source, MarkdownPreview *preview, QObject *parent);
    void setEnabled(bool enabled, bool preferPreview = false);
protected:
    bool eventFilter(QObject *, QEvent *) override;
private:
    enum class Side { None, Source, Preview, Restore };
    void queue(Side side);
    void reflow();
    void synchronize();
    void sourceScrolled();
    void requestSource();
    void requestReader(int pixel, bool entering = false);
    QPointer<SourceEditor> m_source;
    QPointer<MarkdownPreview> m_preview;
    QPointer<NativePreviewView> m_view;
    QPointer<DocumentSession> m_session;
    QTimer m_scroll, m_resize;
    Side m_pending = Side::None;
    Side m_driver = Side::Source;
    bool m_enabled = false, m_applying = false, m_layoutPending = false, m_resizing = false;
    QUuid m_anchorDocument;
    quint64 m_anchorRevision = 0;
    quint32 m_anchorStart = 0, m_anchorEnd = 0;
    qreal m_anchorFraction = 0;
    bool m_readerAnchor = false, m_anchorAtStart = false, m_anchorAtEnd = false, m_sourceBookmark = false;
};
}
