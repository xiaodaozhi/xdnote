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
    const QString &identity, const AuthorizedPreviewImages &authorized) {
    RenderResult result; result.stamp = stamp; result.generation = stamp.request; result.revision = stamp.revision;
    auto resources = preparePreviewImages(snapshot, root, path, identity, authorized);
    auto model = NativePreviewModel::parse(snapshot.utf8, &result.error);
    if (!model) return result;
    auto content = NativePreviewContent::build(std::move(*model), resources.states, &result.error);
    if (!content) return result;
    content->navigationAvailable = resources.analyzed;
    result.content = std::make_shared<const NativePreviewContent>(std::move(*content));
    result.images = std::move(resources.png); result.imageWatchPaths = std::move(resources.watchedPaths);
    result.linkTargets = std::move(resources.linkTargets);
    result.localDocumentsAdmitted = resources.analyzed && root && root->active();
    return result;
}
}
MarkdownPreview::MarkdownPreview(DocumentSession *session, QWidget *parent, int padding, Appearance *appearance)
    : QWidget(parent), m_session(session), m_appearance(appearance ? appearance : new Appearance(this)) {
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
    connect(m_view, &NativePreviewView::layoutStarted, this, [this] { m_restoring = true; });
    connect(m_view, &NativePreviewView::navigationMessage, this, &MarkdownPreview::fragmentNavigationMessage);
    connect(m_view, &NativePreviewView::layoutFailed, this, [this](const QString &message) {
        m_restoring = false; m_pageReady = false; if (!m_closed) emit previewError(message);
    });
    connect(m_view, &NativePreviewView::layoutReady, this, [this](const NativePreviewStamp &stamp) {
        if (m_closed || stamp.request != m_generation || !currentRevision(m_session, stamp.revision)) return;
        m_displayedRevision = stamp.revision; m_pageReady = true;
        restoreReadingPosition(m_scroll);
        const QPointer<MarkdownPreview> alive(this); emit previewReady(stamp.revision);
        if (alive && m_navigationPending) navigateToHeading(m_pendingFragment, m_navigationRevision);
    });
    connect(m_view->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        if (!m_restoring && m_pageReady) m_scroll = m_view->readingPosition();
    });
    connect(session, &DocumentSession::changed, this, &MarkdownPreview::refresh, Qt::QueuedConnection);
    m_imageDelay.setSingleShot(true); m_imageDelay.setInterval(180);
    const auto imageChanged = [this] {
        if (m_closed || !m_session) return;
        ++m_resourceGeneration; m_pageReady = false;
        m_view->expect({m_session->documentId(), m_session->revision(), m_generation, m_contextGeneration, m_resourceGeneration});
        m_imageDelay.start();
    };
    connect(&m_imageWatch, &QFileSystemWatcher::fileChanged, this, imageChanged);
    connect(&m_imageWatch, &QFileSystemWatcher::directoryChanged, this, imageChanged);
    connect(&m_imageDelay, &QTimer::timeout, this, &MarkdownPreview::refresh);
    const auto closed = [this] {
        m_closed = true; ++m_generation; ++m_contextGeneration; ++m_resourceGeneration;
        m_hasPending = m_pageReady = m_navigationPending = false; m_imageDelay.stop();
        m_presentingLinkTargets.clear(); m_view->expect({});
    };
    connect(session, &DocumentSession::closed, this, closed);
    connect(session, &QObject::destroyed, this, closed);
    connect(&m_worker, &QFutureWatcher<RenderResult>::finished, this, [this] {
        const QPointer<MarkdownPreview> alive(this);
        auto result = m_worker.result(); m_running = false;
        if (!m_closed && currentRevision(m_session, result.revision) && result.generation == m_generation
            && result.stamp.path == m_contextGeneration && result.stamp.resources == m_resourceGeneration) {
            if (!result.error.isEmpty()) { m_view->showError(result.stamp, result.error); }
            else {
                m_presentingGeneration = result.generation;
                m_presentingLinkTargets = result.linkTargets;
                const auto previous = m_imageWatch.files() + m_imageWatch.directories();
                if (!previous.isEmpty()) m_imageWatch.removePaths(previous);
                if (!result.imageWatchPaths.isEmpty()) m_imageWatch.addPaths(result.imageWatchPaths);
                if (m_resourceRoot && !m_resourceRoot->active()) { result.images.clear(); result.localDocumentsAdmitted = false; }
                m_restoring = true;
                m_view->presentAsync(result.stamp, std::move(result.content), std::move(result.images), result.localDocumentsAdmitted);
            }
        }
        if (alive) startPending();
    });
    refresh();
}
MarkdownPreview::~MarkdownPreview() { m_closed = true; disconnect(&m_worker, nullptr, this, nullptr); }
void MarkdownPreview::restoreReadingPosition(const QPointF &position) {
    if (m_closed || !std::isfinite(position.x()) || !std::isfinite(position.y())) return;
    m_scroll = QPointF(qBound(0.0, position.x(), 100000000.0), qBound(0.0, position.y(), 100000000.0));
    m_restoring = true; m_view->restoreReadingPosition(m_scroll); m_restoring = false;
}
void MarkdownPreview::navigateToHeading(const QString &fragment, quint64 revision) {
    if (m_closed || !currentRevision(m_session, revision)) {
        m_navigationPending = false; emit fragmentNavigationMessage(QStringLiteral("文档已变化，请重新定位标题。")); return;
    }
    if (!m_pageReady || m_displayedRevision != revision || m_presentingGeneration != m_generation) {
        m_pendingFragment = fragment; m_navigationRevision = revision; m_navigationPending = true; return;
    }
    m_navigationPending = false;
    m_view->navigateToHeading(fragment, m_view->displayedStamp());
}
void MarkdownPreview::refresh() {
    if (m_closed || !m_session) return;
    const auto snapshot = m_session->snapshot();
    if (snapshot.result != EditResult::Applied) {
        if (snapshot.result == EditResult::InvalidText) {
            ++m_generation; const NativePreviewStamp stamp{snapshot.documentId, snapshot.revision, m_generation, m_contextGeneration, m_resourceGeneration};
            m_view->expect(stamp); m_view->showError(stamp, resultDescription(snapshot.result));
        }
        return;
    }
    if (m_generation == std::numeric_limits<quint64>::max()) { m_closed = true; return; }
    ++m_generation; m_pageReady = false;
    if (m_navigationPending && snapshot.revision != m_navigationRevision) {
        m_navigationPending = false; emit fragmentNavigationMessage(QStringLiteral("文档已变化，请重新定位标题。"));
    }
    m_view->expect({snapshot.documentId, snapshot.revision, m_generation, m_contextGeneration, m_resourceGeneration});
    m_pending = snapshot.utf8; m_pendingDocumentId = snapshot.documentId; m_pendingRevision = snapshot.revision;
    m_hasPending = true; startPending();
}
void MarkdownPreview::setResourceContext(std::shared_ptr<WorkspaceResourceReader> root, const QString &path, const QString &identity, const AuthorizedPreviewImages &authorized) {
    if (m_closed || (m_resourceRoot == root && m_resourceDocumentPath == path && m_resourceDocumentIdentity == identity && m_authorizedImages == authorized)) return;
    const bool cancelled = m_navigationPending; m_navigationPending = false;
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
    m_worker.setFuture(QtConcurrent::run([snapshot = std::move(snapshot), stamp, root, path, identity, authorized]() mutable {
        return renderSnapshot(std::move(snapshot), stamp, root, path, identity, authorized);
    }));
}
}
