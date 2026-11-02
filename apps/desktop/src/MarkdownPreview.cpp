#include "MarkdownPreview.h"
#include "DocumentSession.h"
#include "VisualTokens.h"

#include <QJsonObject>
#include <QScopeGuard>
#include <QVBoxLayout>
#include <QWebEnginePage>
#include <QWebEngineLoadingInfo>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineView>
#include <QtConcurrentRun>
#include <cstddef>
#include <cstdint>
#include <limits>

extern "C" {
struct XdnoteRenderBuffer { unsigned char *data; std::size_t length; std::uint32_t status; };
XdnoteRenderBuffer xdnote_markdown_render(const unsigned char *data, std::size_t length);
void xdnote_markdown_free(XdnoteRenderBuffer *buffer);
}

namespace xdnote {
namespace {
bool currentRevision(DocumentSession *session, quint64 revision) {
    if (!session || session->isClosing() || session->isBusy() || session->isComposing()) return false;
    const auto latest = session->revision();
    // Flushing pending edits may notify an observer that requests closure.
    return !session->isClosing() && latest == revision;
}

class BlockResourceRequests final : public QWebEngineUrlRequestInterceptor {
public:
    using QWebEngineUrlRequestInterceptor::QWebEngineUrlRequestInterceptor;
    void interceptRequest(QWebEngineUrlRequestInfo &request) override {
        // Only the generated main document may use a data URL. No file, network,
        // subframe, image, font, stylesheet or other document-controlled request.
        const bool bootstrap = request.resourceType() == QWebEngineUrlRequestInfo::ResourceTypeMainFrame
            && (request.requestUrl().scheme() == QStringLiteral("data")
                || request.requestUrl() == QUrl(QStringLiteral("about:blank")));
        request.block(!bootstrap);
    }
};

class RestrictedPage final : public QWebEnginePage {
public:
    RestrictedPage(QWebEngineProfile *profile, QObject *parent) : QWebEnginePage(profile, parent) {}
    void present(const QString &html, quint64 generation) {
        m_expectedData = QUrl(QStringLiteral("data:text/html;charset=UTF-8,")
            + QString::fromLatin1(html.toUtf8().toPercentEncoding()));
        m_expectedBase = QUrl(QStringLiteral("about:blank#xdnote-%1").arg(generation));
        setHtml(html, m_expectedBase);
    }
    QUrl expectedBase() const { return m_expectedBase; }
protected:
    bool acceptNavigationRequest(const QUrl &url, NavigationType type, bool mainFrame) override {
        // Qt setHtml is a Typed navigation from its API. Authorize only the
        // exact generated document, rather than trusting a navigation type.
        Q_UNUSED(type);
        return mainFrame && !m_expectedData.isEmpty() && (url == m_expectedData || url == m_expectedBase);
    }
    QWebEnginePage *createWindow(WebWindowType) override { return nullptr; }
private:
    QUrl m_expectedData;
    QUrl m_expectedBase;
};

RenderResult renderSnapshot(QByteArray source, quint64 generation, quint64 revision) {
    const auto pointer = reinterpret_cast<const unsigned char *>(source.constData());
    auto buffer = xdnote_markdown_render(pointer, std::size_t(source.size()));
    const auto release = qScopeGuard([&] { xdnote_markdown_free(&buffer); });
    if (buffer.status != 0 || !buffer.data || buffer.length > 8 * 1024 * 1024)
        return {{}, QStringLiteral("预览失败：无效源文或超过准入样本额度"), generation, revision};
    return {QByteArray(reinterpret_cast<const char *>(buffer.data), qsizetype(buffer.length)), {}, generation, revision};
}

QString pageFor(const QByteArray &body, quint64 generation) {
    const auto typography = visualTokens().value(QStringLiteral("typography")).toObject();
    const auto reader = typography.value(QStringLiteral("reader")).toObject();
    const auto colors = visualTokens().value(QStringLiteral("themes")).toObject().value(QStringLiteral("light")).toObject();
    // All markup and CSS around the body are application-owned. Raw source HTML
    // and URLs have already been converted to inert text by the Rust service.
    const QString head = QStringLiteral(
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"xdnote-generation\" content=\"%1\">"
        "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; script-src 'none'; "
        "style-src 'unsafe-inline'; img-src 'none'; connect-src 'none'; object-src 'none'; "
        "frame-src 'none'; form-action 'none'; base-uri 'none'\">"
        "<style>body{margin:24px auto;padding:0 24px;max-width:%2px;font:%3px/%4 system-ui,sans-serif;"
        "color:%5;background:%6;overflow-wrap:anywhere}pre{white-space:pre-wrap;background:%7;padding:16px}"
        "code{font-family:monospace}table{border-collapse:collapse}td,th{border:1px solid %8;padding:6px 12px}"
        ".link-placeholder{color:%9;text-decoration:underline}.image-placeholder{color:%9}"
        "blockquote{border-left:3px solid %8;margin-left:0;padding-left:16px}</style></head><body>")
        .arg(generation).arg(reader.value(QStringLiteral("maxContentWidth")).toInt())
        .arg(reader.value(QStringLiteral("size")).toInt()).arg(reader.value(QStringLiteral("lineHeight")).toDouble())
        .arg(colors.value(QStringLiteral("text")).toString(), colors.value(QStringLiteral("background")).toString(),
             colors.value(QStringLiteral("codeSurface")).toString(), colors.value(QStringLiteral("border")).toString(),
             colors.value(QStringLiteral("textSecondary")).toString());
    return head + QString::fromUtf8(body) + QStringLiteral("</body></html>");
}
}

MarkdownPreview::MarkdownPreview(DocumentSession *session, QWidget *parent) : QWidget(parent), m_session(session) {
    m_view = new QWebEngineView(this);
    m_view->setObjectName(QStringLiteral("markdownPreview"));
    auto *profile = new QWebEngineProfile(this);
    profile->setHttpCacheType(QWebEngineProfile::NoCache);
    profile->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
    profile->setUrlRequestInterceptor(new BlockResourceRequests(profile));
    auto *page = new RestrictedPage(profile, m_view);
    m_view->setPage(page);
    auto *settings = page->settings();
    for (auto option : {QWebEngineSettings::JavascriptEnabled, QWebEngineSettings::AutoLoadImages,
                       QWebEngineSettings::LocalStorageEnabled, QWebEngineSettings::PluginsEnabled,
                       QWebEngineSettings::LocalContentCanAccessFileUrls,
                       QWebEngineSettings::LocalContentCanAccessRemoteUrls,
                       QWebEngineSettings::JavascriptCanAccessClipboard, QWebEngineSettings::WebGLEnabled})
        settings->setAttribute(option, false);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_view);
    connect(session, &DocumentSession::changed, this, &MarkdownPreview::refresh, Qt::QueuedConnection);
    connect(session, &DocumentSession::closed, this, [this] {
        m_closed = true; ++m_generation; m_hasPending = false; m_view->stop();
    });
    connect(session, &QObject::destroyed, this, [this] { m_closed = true; ++m_generation; m_hasPending = false; });
    connect(&m_worker, &QFutureWatcher<RenderResult>::finished, this, [this, page] {
        const auto result = m_worker.result();
        m_running = false;
        if (!m_closed && currentRevision(m_session, result.revision) && result.generation == m_generation) {
            if (!result.error.isEmpty()) emit previewError(result.error);
            else {
                m_presentingGeneration = result.generation;
                m_presentingRevision = result.revision;
                const auto document = pageFor(result.html, result.generation);
                // setHtml uses a data URL with Chromium's 2 MiB encoded limit.
                // Fail explicitly rather than leaving a silent blank preview.
                if (QUrl::toPercentEncoding(document).size() > 2 * 1024 * 1024 - 1024)
                    emit previewError(QStringLiteral("预览超过当前页面加载额度，请缩小准入样本"));
                else page->present(document, result.generation);
            }
        }
        startPending();
    });
    connect(page, &QWebEnginePage::loadFinished, this, [this, page](bool success) {
        if (!success || m_closed || !m_session || m_session->isClosing()) return;
        const auto generation = m_presentingGeneration;
        const auto revision = m_presentingRevision;
        const auto marker = QStringLiteral("<meta name=\"xdnote-generation\" content=\"%1\">").arg(generation);
        const QPointer<MarkdownPreview> guard(this);
        page->toHtml([guard, generation, revision, marker](const QString &html) {
            if (!guard || guard->m_closed || !guard->m_session || guard->m_session->isClosing()
                || generation != guard->m_generation || !currentRevision(guard->m_session, revision) || !html.contains(marker)) return;
            guard->m_displayedRevision = revision;
            emit guard->previewReady(revision);
        });
    });
    connect(page, &QWebEnginePage::loadingChanged, this, [this, page](const QWebEngineLoadingInfo &info) {
        if (m_closed || !m_session || m_session->isClosing() || m_presentingGeneration != m_generation) return;
        if (info.status() == QWebEngineLoadingInfo::LoadFailedStatus && info.url() == page->expectedBase())
            emit previewError(QStringLiteral("预览页面加载失败：%1").arg(info.errorString()));
    });
    connect(page, &QWebEnginePage::renderProcessTerminated, this, [this] {
        if (!m_closed && m_session && !m_session->isClosing()) emit previewError(QStringLiteral("预览进程已停止，请重新生成预览"));
    });
    refresh();
}

MarkdownPreview::~MarkdownPreview() {
    m_closed = true;
    // The worker owns only an immutable byte snapshot and frees Rust output in
    // its own scope. It never captures this widget or the active document.
    disconnect(&m_worker, nullptr, this, nullptr);
    delete m_view;
    m_view = nullptr;
}

void MarkdownPreview::refresh() {
    if (m_closed || !m_session) return;
    const auto snapshot = m_session->snapshot();
    if (snapshot.result != EditResult::Applied) {
        if (snapshot.result == EditResult::InvalidText) emit previewError(resultDescription(snapshot.result));
        return;
    }
    if (m_generation == std::numeric_limits<quint64>::max()) { m_closed = true; return; }
    ++m_generation;
    m_pending = snapshot.utf8;
    m_pendingRevision = snapshot.revision;
    m_hasPending = true;
    startPending();
}

void MarkdownPreview::startPending() {
    if (m_closed || m_running || !m_hasPending) return;
    m_running = true;
    m_hasPending = false;
    auto snapshot = std::move(m_pending);
    const auto generation = m_generation;
    const auto revision = m_pendingRevision;
    m_worker.setFuture(QtConcurrent::run([snapshot = std::move(snapshot), generation, revision] {
        return renderSnapshot(snapshot, generation, revision);
    }));
}
}
