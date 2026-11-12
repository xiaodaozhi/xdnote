#include "ScrollSyncController.h"
#include "SourceEditor.h"
#include "MarkdownPreview.h"
#include "DocumentSession.h"
#include <QEvent>
#include <QScrollBar>
#include <cmath>

namespace xdnote {
ScrollSyncController::ScrollSyncController(SourceEditor *source, MarkdownPreview *preview, QObject *parent)
    : QObject(parent), m_source(source), m_preview(preview), m_view(preview->view()), m_session(source->session()) {
    setObjectName("splitScrollSync");
    m_scroll.setSingleShot(true); m_resize.setSingleShot(true);
    connect(&m_scroll, &QTimer::timeout, this, &ScrollSyncController::synchronize);
    connect(&m_resize, &QTimer::timeout, this, [this] { m_resizing = false; queue(Side::Restore); });
    connect(source->verticalScrollBar(), &QScrollBar::valueChanged, this, &ScrollSyncController::sourceScrolled);
    connect(m_view->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { queue(m_driver == Side::Source ? Side::Restore : Side::Preview); });
    connect(source, &SourceEditor::verticalNavigationRequested, this, &ScrollSyncController::requestSource);
    connect(source->verticalScrollBar(), &QScrollBar::actionTriggered, this, [this] { requestSource(); });
    connect(m_view, &NativePreviewView::verticalNavigationRequested, this, [this] { requestReader(m_view->verticalScrollBar()->value()); });
    connect(m_view->verticalScrollBar(), &QScrollBar::actionTriggered, this, [this] { requestReader(m_view->verticalScrollBar()->sliderPosition()); });
    connect(source->verticalScrollBar(), &QScrollBar::rangeChanged, this, [this] { reflow(); });
    connect(m_view->verticalScrollBar(), &QScrollBar::rangeChanged, this, [this] { reflow(); });
    connect(m_view, &NativePreviewView::layoutStarted, this, [this] { m_layoutPending = true; m_scroll.stop(); m_pending = Side::None; });
    connect(m_view, &NativePreviewView::layoutFailed, this, [this] { m_layoutPending = true; m_scroll.stop(); });
    connect(m_session, &DocumentSession::changed, this, [this] { m_layoutPending = true; m_driver = Side::Source; m_readerAnchor = false; m_scroll.stop(); m_pending = Side::None; });
    connect(m_session, &DocumentSession::closed, this, [this] { setEnabled(false); });
    connect(m_session, &DocumentSession::stateChanged, this, [this] { queue(Side::Restore); });
    connect(preview, &MarkdownPreview::previewReady, this, [this] { m_layoutPending = false; queue(Side::Restore); });
    connect(preview, &MarkdownPreview::fragmentNavigated, this, [this] { requestReader(m_view->verticalScrollBar()->value()); });
    for (auto *widget : {static_cast<QWidget *>(source), source->viewport(), static_cast<QWidget *>(m_view.data()), m_view->viewport()}) widget->installEventFilter(this);
}
void ScrollSyncController::setEnabled(bool enabled, bool preferPreview) {
    if (m_enabled == enabled) return;
    m_enabled = enabled; m_driver = Side::Source; m_readerAnchor = false; m_scroll.stop(); m_pending = Side::None;
    if (enabled && preferPreview && m_view) requestReader(m_view->verticalScrollBar()->value(), true);
    if (enabled && m_driver != Side::Preview) queue(Side::Source);
}
void ScrollSyncController::sourceScrolled() {
    if (m_sourceBookmark && m_driver == Side::Source && !m_applying && !m_resizing && m_source && m_source->isVisible()) {
        // Once the public wrap bookmark has been used, later native/user
        // scrolling must also advance it, including in source-only mode.
        const QPointer<ScrollSyncController> alive(this); m_applying = true;
        const auto first = m_source->send(SCI_GETFIRSTVISIBLELINE);
        const auto line = m_source->send(SCI_DOCLINEFROMVISIBLE, uptr_t(first));
        const auto subline = first - m_source->send(SCI_VISIBLEFROMDOCLINE, uptr_t(line));
        m_source->send(SCI_SCROLLVERTICAL, uptr_t(line), subline);
        if (!alive) return;
        m_applying = false;
    }
    queue(m_driver == Side::Preview ? Side::Restore : Side::Source);
}
void ScrollSyncController::requestSource() {
    if (!m_enabled || m_applying || !m_source || !m_source->isVisible()) return;
    m_driver = Side::Source; m_readerAnchor = false; m_pending = Side::Source; queue(Side::Source);
}
void ScrollSyncController::requestReader(int pixel, bool entering) {
    if (!m_enabled || m_applying || !m_source || !m_view || !m_preview || (!entering && !m_source->isVisible()) || !m_view->isVisible()) return;
    const QPointer<ScrollSyncController> alive(this); const bool current = m_preview->mappingCurrent();
    if (!alive || !current) return;
    auto *bar = m_view->verticalScrollBar(); pixel = qBound(0, pixel, bar->maximum());
    const auto stamp = m_view->displayedStamp(); const auto span = m_view->scrollSpanAtY(pixel, stamp);
    if (!span) return;
    // Selection notifications from an earlier source interaction can arrive
    // after this newer reader command. Baseline them before changing drivers.
    m_source->acknowledgeNavigation();
    m_driver = Side::Preview; m_readerAnchor = true; m_anchorDocument = stamp.document; m_anchorRevision = stamp.revision;
    m_anchorStart = span->sourceStart; m_anchorEnd = span->sourceEnd;
    m_anchorFraction = qBound(0.0, (pixel - span->top) / (span->bottom - span->top), 1.0);
    m_anchorAtStart = pixel == 0; m_anchorAtEnd = bar->maximum() > 0 && pixel == bar->maximum();
    m_pending = Side::Preview; queue(Side::Preview);
}
void ScrollSyncController::queue(Side side) {
    if (!m_enabled || m_applying || m_layoutPending || m_resizing || !m_source || !m_view
        || !m_source->isVisible() || !m_view->isVisible()) return;
    if (side != Side::Restore && side != m_driver) return;
    if (side != Side::Restore || m_pending == Side::None) m_pending = side;
    if (side == Side::Source) m_readerAnchor = false;
    m_scroll.start(0);
}
void ScrollSyncController::reflow() {
    if (m_applying) return;
    m_resizing = true; m_scroll.stop(); m_pending = Side::None; m_resize.start(0);
}
bool ScrollSyncController::eventFilter(QObject *, QEvent *event) {
    if (event->type() == QEvent::Resize || event->type() == QEvent::Show) reflow();
    else if (event->type() == QEvent::Hide) { m_scroll.stop(); m_pending = Side::None; }
    return false;
}
void ScrollSyncController::synchronize() {
    auto side = m_pending; m_pending = Side::None;
    if (side == Side::None || !m_enabled || m_applying || m_resizing || m_layoutPending || !m_source || !m_preview || !m_view
        || !m_session || m_source->dispatching() || !m_source->isVisible() || !m_view->isVisible()) return;
    // No retained QObject field reference may outlive a scroll notification:
    // an owner is allowed to close the pane while processing that notification.
    const QPointer<ScrollSyncController> alive(this);
    m_applying = true;
    const auto finish = [alive] { if (alive) alive->m_applying = false; };
    const bool current = m_preview->mappingCurrent();
    if (!alive) return;
    if (!current) { if (m_pending == Side::None) m_pending = side; finish(); return; }
    const auto stamp = m_view->displayedStamp();
    const auto bytes = m_view->content()->model.sourceBytes();
    const qreal lineHeight = qMax(sptr_t(1), m_source->send(SCI_TEXTHEIGHT, 0));
    const auto first = m_source->send(SCI_GETFIRSTVISIBLELINE);
    const auto sourceY = [this, bytes, lineHeight](quint32 byte) -> qreal {
        if (byte == bytes) {
            const auto last = qMax(sptr_t(0), m_source->send(SCI_GETLINECOUNT) - 1);
            return (m_source->send(SCI_VISIBLEFROMDOCLINE, uptr_t(last)) + qMax(sptr_t(1), m_source->send(SCI_WRAPCOUNT, uptr_t(last)))) * lineHeight;
        }
        const auto line = m_source->send(SCI_LINEFROMPOSITION, byte);
        return m_source->send(SCI_VISIBLEFROMDOCLINE, uptr_t(line)) * lineHeight;
    };
    const auto fraction = [](qreal value, qreal start, qreal end) { return end > start ? qBound(0.0, (value - start) / (end - start), 1.0) : 0.0; };
    auto *readerBar = m_view->verticalScrollBar(); auto *sourceBar = m_source->verticalScrollBar();
    if (side == Side::Restore) {
        const auto anchor = m_readerAnchor && m_anchorDocument == stamp.document && m_anchorRevision == stamp.revision
            ? m_view->scrollSpanForSource(m_anchorStart, stamp) : std::nullopt;
        if (anchor && anchor->sourceStart == m_anchorStart && anchor->sourceEnd == m_anchorEnd) {
            readerBar->setValue(m_anchorAtStart ? 0 : m_anchorAtEnd ? readerBar->maximum() : qRound(anchor->top + m_anchorFraction * (anchor->bottom - anchor->top)));
            if (!alive) return;
            if (!m_source || !m_preview || !m_view || !m_preview->mappingCurrent()) { finish(); return; }
            side = Side::Preview;
        } else { side = Side::Source; m_driver = Side::Source; }
    }
    if (side == Side::Source) {
        m_readerAnchor = false;
        const auto line = m_source->send(SCI_DOCLINEFROMVISIBLE, uptr_t(first));
        const auto byte = m_source->send(SCI_POSITIONFROMLINE, uptr_t(line));
        const auto span = m_view->scrollSpanForSource(quint32(qMax(sptr_t(0), byte)), stamp);
        if (!span) { finish(); return; }
        const auto part = fraction(first * lineHeight, sourceY(span->sourceStart), sourceY(span->sourceEnd));
        const int target = first == 0 ? 0 : sourceBar->maximum() > 0 && first >= sourceBar->maximum() ? readerBar->maximum()
            : qBound(0, qRound(span->top + part * (span->bottom - span->top)), readerBar->maximum());
        // Source-driven navigation needs the same public wrap bookmark as
        // reader-driven navigation. A pending native wrap may otherwise restore
        // its earlier anchor after the reader has already reached this target.
        const auto subline = first - m_source->send(SCI_VISIBLEFROMDOCLINE, uptr_t(line));
        m_sourceBookmark = true;
        m_source->send(SCI_SCROLLVERTICAL, uptr_t(line), subline);
        if (!alive || !m_source || !m_view) return;
        readerBar->setValue(target);
    } else {
        const auto span = m_view->scrollSpanAtY(readerBar->value(), stamp);
        if (!span) { finish(); return; }
        const auto part = fraction(readerBar->value(), span->top, span->bottom);
        m_readerAnchor = true; m_anchorDocument = stamp.document; m_anchorRevision = stamp.revision;
        m_anchorStart = span->sourceStart; m_anchorEnd = span->sourceEnd; m_anchorFraction = part;
        m_anchorAtStart = readerBar->value() == 0; m_anchorAtEnd = readerBar->maximum() > 0 && readerBar->value() == readerBar->maximum();
        const auto y = sourceY(span->sourceStart) + part * (sourceY(span->sourceEnd) - sourceY(span->sourceStart));
        const int target = readerBar->value() == 0 ? 0 : readerBar->maximum() > 0 && readerBar->value() >= readerBar->maximum() ? sourceBar->maximum()
            : qBound(0, qRound(y / lineHeight), sourceBar->maximum());
        // Persist the document line + wrapped subline through Scintilla's
        // pending wraps. SetFirstVisibleLine alone is overwritten by its old
        // scrollToAfterWrap anchor when an asynchronous wrap finishes.
        const auto documentLine = m_source->send(SCI_DOCLINEFROMVISIBLE, uptr_t(target));
        const auto subline = target - m_source->send(SCI_VISIBLEFROMDOCLINE, uptr_t(documentLine));
        m_sourceBookmark = true;
        m_source->send(SCI_SCROLLVERTICAL, uptr_t(documentLine), subline);
    }
    finish();
}
}
