#include "MarkdownPreview.h"
#include "Appearance.h"
#include "DocumentSession.h"
#include <QScrollBar>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <cmath>
#include <limits>

namespace xdnote {
namespace {
bool currentRevision(DocumentSession *session, quint64 revision) {
    return session && !session->isClosing() && session->revision() == revision;
}
RenderResult renderSnapshot(DocumentSnapshot snapshot, NativePreviewStamp stamp,
    const std::shared_ptr<WorkspaceResourceReader> &root, const QString &path,
    const QString &identity, const AuthorizedPreviewImages &authorized,
    const PreviewCancellation &cancel, const std::shared_ptr<PreviewImageCache> &imageCache, const std::shared_ptr<PreviewAnalysisStore> &analysisStore,
    const std::shared_ptr<const NativePreviewContent> &previous, const QByteArray &previousStates, double queueMs) {
    RenderResult result; result.stamp = stamp; result.generation = stamp.request; result.revision = stamp.revision;
    result.metrics = {{"queueMs", queueMs}, {"semanticMs", 0.0}, {"referencesMs", 0.0}, {"decodeMs", 0.0}};
    QElapsedTimer total, timer; total.start();
    auto finish = [&]() {
        result.cancelled = previewCancelled(cancel);
        result.metrics["workerMs"] = total.nsecsElapsed() / 1e6;
        if (result.cancelled) { result.content.reset(); result.images.clear(); result.error.clear(); }
        return result;
    };
    auto analysis = PreviewAnalysis::get(snapshot, cancel, result.metrics, &result.error, analysisStore);
    if (!analysis || previewCancelled(cancel)) return finish();
    timer.start();
    auto resources = preparePreviewImages(snapshot, analysis->references, root, path, identity, authorized, cancel, imageCache);
    result.metrics["resourcesMs"] = timer.nsecsElapsed() / 1e6;
    result.metrics["resourceReadMs"] = resources.readMs; result.metrics["imageDecodeMs"] = resources.decodeMs;
    result.metrics["imagesDecoded"] = resources.decodedImages; result.metrics["imagesReused"] = resources.reusedImages;
    if (previewCancelled(cancel)) return finish();
    timer.restart();
    const bool reuse = previous && previous->model.sourceDigest() == analysis->model.sourceDigest()
        && previousStates == resources.states && previous->navigationAvailable == resources.analyzed;
    result.metrics["projectionCacheHit"] = reuse;
    if (reuse) result.content = previous;
    else {
        auto content = NativePreviewContent::build(analysis->model, resources.states, &result.error);
        if (!content) return finish();
        content->navigationAvailable = resources.analyzed;
        result.content = std::make_shared<const NativePreviewContent>(std::move(*content));
    }
    result.metrics["projectionMs"] = timer.nsecsElapsed() / 1e6;
    result.imageStates = resources.states;
    result.images = std::move(resources.png); result.imageWatchPaths = std::move(resources.watchedPaths);
    result.linkTargets = std::move(resources.linkTargets);
    result.localDocumentsAdmitted = resources.analyzed && root && root->active();
    return finish();
}
}
MarkdownPreview::MarkdownPreview(DocumentSession *session, QWidget *parent, int padding, Appearance *appearance)
    : QWidget(parent), m_session(session), m_appearance(appearance ? appearance : new Appearance(this)) {
    m_documentId = session->documentId();
    m_view = new NativePreviewView(m_appearance, this, padding);
    m_view->setObjectName(QStringLiteral("markdownPreview"));
    m_view->setActionValidator([this](const NativePreviewStamp &stamp) {
        return !m_closed && m_pageReady && stamp.request == m_generation && currentRevision(m_session, stamp.revision);
    });
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->addWidget(m_view);
    connect(m_view, &NativePreviewView::copyTextRequested, this, [this](const QString &text) {
        if (!m_closed && m_pageReady && currentRevision(m_session, m_displayedRevision)
            && m_presentingGeneration == m_generation) emit copyTextRequested(text);
    });
    connect(m_view, &NativePreviewView::documentLinkRequested, this, [this](const QString &target, const NativePreviewStamp &stamp) {
        if (!m_closed && m_pageReady && m_session && stamp.document == m_session->documentId()
            && stamp.request == m_generation && stamp.path == m_contextGeneration && stamp.resources == m_resourceGeneration
            && currentRevision(m_session, stamp.revision) && m_resourceRoot && m_resourceRoot->active()
            && m_presentingLinkTargets.contains(target)) emit documentLinkRequested(target, stamp.revision);
    });
    connect(m_view, &NativePreviewView::fragmentNavigated, this, [this](const QString &fragment) {
        const QPointer<MarkdownPreview> alive(this); emit fragmentNavigationMessage({});
        if (alive) emit fragmentNavigated(fragment);
    });
    connect(m_view, &NativePreviewView::layoutStarted, this, [this] { m_restoring = true; m_layoutAt.start(); });
    connect(m_view, &NativePreviewView::navigationMessage, this, &MarkdownPreview::fragmentNavigationMessage);
    connect(m_view, &NativePreviewView::layoutFailed, this, [this](const QString &message) {
        m_restoring = false; m_pageReady = false; if (!m_closed) emit previewError(message);
    });
    connect(m_view, &NativePreviewView::layoutReady, this, [this](const NativePreviewStamp &stamp) {
        if (m_closed || stamp.request != m_generation || !currentRevision(m_session, stamp.revision)) return;
        m_displayedRevision = stamp.revision; m_pageReady = true;
        if (m_awaitingLayout) {
            m_metrics["layoutMs"] = m_layoutAt.isValid() ? m_layoutAt.nsecsElapsed() / 1e6 : 0;
            m_metrics["readyMs"] = m_requestedAt.nsecsElapsed() / 1e6; m_awaitingLayout = false;
        }
        restoreReadingPosition(m_scroll);
        const QPointer<MarkdownPreview> alive(this); emit previewReady(stamp.revision);
        if (alive && m_navigationPending) navigateToHeading(m_pendingFragment, m_navigationRevision);
    });
    connect(m_view->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        if (!m_restoring && m_pageReady) m_scroll = m_view->readingPosition();
    });
    connect(session, &DocumentSession::changed, this, &MarkdownPreview::refresh, Qt::QueuedConnection);
    connect(session, &DocumentSession::stateChanged, this, &MarkdownPreview::retryDeferred, Qt::QueuedConnection);
    m_imageDelay.setSingleShot(true); m_imageDelay.setInterval(180);
    const auto imageChanged = [this] {
        if (m_closed || !m_session) return;
        if (m_cancel) m_cancel->store(true);
        ++m_resourceGeneration; m_pageReady = false;
        m_view->expect({m_session->documentId(), m_session->revision(), m_generation, m_contextGeneration, m_resourceGeneration});
        m_imageDelay.start();
    };
    connect(&m_imageWatch, &QFileSystemWatcher::fileChanged, this, imageChanged);
    connect(&m_imageWatch, &QFileSystemWatcher::directoryChanged, this, imageChanged);
    connect(&m_imageDelay, &QTimer::timeout, this, &MarkdownPreview::refresh);
    const auto closed = [this] {
        if (m_cancel) m_cancel->store(true);
        PreviewAnalysis::forget(m_documentId); m_previousContent.reset(); m_previousImageStates.clear(); m_imageCache.reset();
        m_closed = true; ++m_generation; ++m_contextGeneration; ++m_resourceGeneration;
        m_hasPending = m_pageReady = m_navigationPending = m_refreshDeferred = false; m_imageDelay.stop();
        m_presentingLinkTargets.clear(); m_view->expect({});
    };
    connect(session, &DocumentSession::closed, this, closed);
    connect(session, &QObject::destroyed, this, closed);
    connect(&m_worker, &QFutureWatcher<RenderResult>::finished, this, [this] {
        const QPointer<MarkdownPreview> alive(this);
        auto result = m_worker.result(); m_running = false; ++m_completedWork;
        if (result.cancelled) ++m_cancelledWork;
        if (!result.cancelled && !m_closed && currentRevision(m_session, result.revision) && result.generation == m_generation
            && result.stamp.path == m_contextGeneration && result.stamp.resources == m_resourceGeneration) {
            if (!result.error.isEmpty()) { m_view->showError(result.stamp, result.error); }
            else {
                m_metrics = result.metrics;
                m_previousContent = result.content; m_previousImageStates = result.imageStates;
                m_presentingGeneration = result.generation;
                m_presentingLinkTargets = result.linkTargets;
                const auto previous = m_imageWatch.files() + m_imageWatch.directories();
                if (!previous.isEmpty()) m_imageWatch.removePaths(previous);
                if (!result.imageWatchPaths.isEmpty()) m_imageWatch.addPaths(result.imageWatchPaths);
                emit previewPrepared(result.revision);
                if (!alive) return;
                if (m_closed || result.generation != m_generation || !currentRevision(m_session, result.revision)) { startPending(); return; }
                if (m_resourceRoot && !m_resourceRoot->active()) { result.images.clear(); result.localDocumentsAdmitted = false; }
                m_restoring = true; m_awaitingLayout = true; m_layoutAt.start();
                m_view->presentAsync(result.stamp, std::move(result.content), std::move(result.images), result.localDocumentsAdmitted);
            }
        }
        if (alive) startPending();
    });
    // Let the owner configure its resource root before the initial parse.
    QTimer::singleShot(0, this, [this] { if (m_generation == 0) refresh(); });
}
MarkdownPreview::~MarkdownPreview() {
    m_closed = true; if (m_cancel) m_cancel->store(true);
    PreviewAnalysis::forget(m_documentId); disconnect(&m_worker, nullptr, this, nullptr);
}
QJsonObject MarkdownPreview::workStatistics() const {
    auto result = m_metrics;
    result["completedWork"] = m_completedWork; result["cancelledWork"] = m_cancelledWork;
    result["running"] = m_running; result["pending"] = m_hasPending;
    result["analysisCache"] = PreviewAnalysis::cacheStatistics();
    result["imageCacheBytes"] = m_imageCache ? m_imageCache->retainedBytes() : 0;
    return result;
}
bool MarkdownPreview::mappingCurrent() {
    if (m_closed || m_restoring || !m_pageReady || !m_session || m_session->isClosing()
        || m_session->isBusy() || m_session->isComposing() || !m_view->ready()) return false;
    const QPointer<MarkdownPreview> alive(this);
    const auto revision = m_session->revision(); // Flush pending edits before trusting the old map.
    if (!alive) return false;
    const auto stamp = m_view->displayedStamp();
    return !m_closed && !m_restoring && m_pageReady && m_session && m_view->ready()
        && stamp.document == m_session->documentId() && stamp.revision == revision
        && stamp.request == m_generation && m_presentingGeneration == m_generation
        && stamp.path == m_contextGeneration && stamp.resources == m_resourceGeneration;
}
void MarkdownPreview::restoreReadingPosition(const QPointF &position) {
    if (m_closed || !std::isfinite(position.x()) || !std::isfinite(position.y())) return;
    m_scroll = QPointF(qBound(0.0, position.x(), 100000000.0), qBound(0.0, position.y(), 100000000.0));
    m_restoring = true; m_view->restoreReadingPosition(m_scroll); m_restoring = false;
}
void MarkdownPreview::navigateToHeading(const QString &fragment, quint64 revision) {
    if (m_closed || !currentRevision(m_session, revision)) {
        m_navigationPending = false; emit fragmentNavigationMessage(QStringLiteral("文档已变化，请重新定位标题。")); return;
    }
    if (!m_pageReady || !m_view->ready() || m_displayedRevision != revision || m_presentingGeneration != m_generation) {
        m_pendingFragment = fragment; m_navigationRevision = revision; m_navigationPending = true; return;
    }
    m_navigationPending = false;
    m_view->navigateToHeading(fragment, m_view->displayedStamp());
}
void MarkdownPreview::refresh() {
    if (m_closed || !m_session) return;
    if (m_cancel) m_cancel->store(true);
    m_requestedAt.start(); m_queuedAt.start(); m_awaitingLayout = false;
    if (m_generation == std::numeric_limits<quint64>::max()) { m_closed = true; return; }
    const auto snapshot = m_session->snapshot();
    if (snapshot.result != EditResult::Applied) {
        if (snapshot.result == EditResult::Busy || snapshot.result == EditResult::Composing) {
            // Keep the latest intent, not an obsolete frozen snapshot. A save
            // can rebind the resource context inside a state notification,
            // where snapshot() intentionally refuses reentrant reads.
            m_refreshDeferred = true; m_pageReady = m_hasPending = false; m_pending.clear();
            ++m_generation; m_view->expect({snapshot.documentId, snapshot.revision, m_generation, m_contextGeneration, m_resourceGeneration});
            retryDeferred();
        } else m_refreshDeferred = false;
        if (snapshot.result == EditResult::InvalidText) {
            ++m_generation; const NativePreviewStamp stamp{snapshot.documentId, snapshot.revision, m_generation, m_contextGeneration, m_resourceGeneration};
            m_view->expect(stamp); m_view->showError(stamp, resultDescription(snapshot.result));
        }
        return;
    }
    m_refreshDeferred = false;
    ++m_generation; m_pageReady = false;
    if (m_navigationPending && snapshot.revision != m_navigationRevision) {
        m_navigationPending = false; emit fragmentNavigationMessage(QStringLiteral("文档已变化，请重新定位标题。"));
    }
    m_view->expect({snapshot.documentId, snapshot.revision, m_generation, m_contextGeneration, m_resourceGeneration});
    m_pending = snapshot.utf8; m_pendingDocumentId = snapshot.documentId; m_pendingRevision = snapshot.revision;
    m_hasPending = true; startPending();
}
void MarkdownPreview::retryDeferred() {
    if (m_closed || !m_refreshDeferred || m_refreshQueued) return;
    m_refreshQueued = true;
    QTimer::singleShot(0, this, [this] {
        m_refreshQueued = false;
        if (!m_closed && m_refreshDeferred && m_session && !m_session->isBusy() && !m_session->isComposing()) refresh();
    });
}
void MarkdownPreview::setResourceContext(std::shared_ptr<WorkspaceResourceReader> root, const QString &path, const QString &identity, const AuthorizedPreviewImages &authorized) {
    if (m_closed || (m_resourceRoot == root && m_resourceDocumentPath == path && m_resourceDocumentIdentity == identity && m_authorizedImages == authorized)) return;
    const bool cancelled = m_navigationPending; m_navigationPending = false;
    m_imageCache = std::make_shared<PreviewImageCache>();
    m_resourceRoot = std::move(root); m_resourceDocumentPath = path; m_resourceDocumentIdentity = identity; m_authorizedImages = authorized;
    ++m_contextGeneration; ++m_resourceGeneration; m_imageDelay.stop();
    const auto previous = m_imageWatch.files() + m_imageWatch.directories(); if (!previous.isEmpty()) m_imageWatch.removePaths(previous);
    refresh(); if (cancelled) emit fragmentNavigationMessage(QStringLiteral("文档位置已变化，请重新定位标题。"));
}
void MarkdownPreview::startPending() {
    if (m_closed || m_running || !m_hasPending) return;
    m_running = true; m_hasPending = false;
    DocumentSnapshot snapshot{EditResult::Applied, std::move(m_pending), m_pendingRevision, m_pendingDocumentId};
    NativePreviewStamp stamp{m_pendingDocumentId, m_pendingRevision, m_generation, m_contextGeneration, m_resourceGeneration};
    const auto root = m_resourceRoot; const auto path = m_resourceDocumentPath, identity = m_resourceDocumentIdentity; const auto authorized = m_authorizedImages;
    m_cancel = std::make_shared<std::atomic_bool>(false);
    const auto cancel = m_cancel; const auto imageCache = m_imageCache; const auto analysisStore = m_analysisStore;
    const auto previous = m_previousContent; const auto previousStates = m_previousImageStates;
    const auto queueMs = m_queuedAt.nsecsElapsed() / 1e6;
    m_worker.setFuture(QtConcurrent::run([snapshot = std::move(snapshot), stamp, root, path, identity, authorized, cancel, imageCache, analysisStore, previous, previousStates, queueMs]() mutable {
        return renderSnapshot(std::move(snapshot), stamp, root, path, identity, authorized, cancel, imageCache, analysisStore, previous, previousStates, queueMs);
    }));
}
}
