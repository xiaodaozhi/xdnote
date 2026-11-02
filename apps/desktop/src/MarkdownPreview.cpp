#include "MarkdownPreview.h"
#include "DocumentSession.h"
#include "VisualTokens.h"

#include <QJsonObject>
#include <QBuffer>
#include <QUuid>
#include <QScopeGuard>
#include <QVBoxLayout>
#include <QWebEnginePage>
#include <QWebEngineLoadingInfo>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineUrlSchemeHandler>
#include <QWebEngineUrlRequestJob>
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
        // Only the generated main document may use the private scheme. No file, network,
        // subframe, image, font, stylesheet or other document-controlled request.
        const bool bootstrap = request.resourceType() == QWebEngineUrlRequestInfo::ResourceTypeMainFrame
            && (request.requestUrl().scheme() == QStringLiteral("xdnote-preview")
                || request.requestUrl() == QUrl(QStringLiteral("about:blank")));
        request.block(!bootstrap);
    }
};

class MemoryPage final : public QWebEngineUrlSchemeHandler {
public:
    using QWebEngineUrlSchemeHandler::QWebEngineUrlSchemeHandler;
    void setPage(const QUrl &url, QByteArray html) { m_url = url; m_html = std::move(html); }
    void requestStarted(QWebEngineUrlRequestJob *request) override {
        // Runs on the owning GUI thread. No path is ever resolved on disk.
        if (request->requestMethod() != "GET" || m_url.isEmpty() || request->requestUrl() != m_url) {
            request->fail(QWebEngineUrlRequestJob::RequestDenied); return;
        }
        auto *buffer = new QBuffer(request);
        buffer->setData(m_html); buffer->open(QIODevice::ReadOnly);
        request->reply("text/html", buffer);
    }
private:
    QUrl m_url;
    QByteArray m_html;
};

class RestrictedPage final : public QWebEnginePage {
public:
    RestrictedPage(QWebEngineProfile *profile, QObject *parent) : QWebEnginePage(profile, parent) {
        m_memory = new MemoryPage(profile);
        profile->installUrlSchemeHandler("xdnote-preview", m_memory);
    }
    void present(const QString &html, quint64 generation) {
        m_expectedBase = QUrl(QStringLiteral("xdnote-preview:/%1/%2").arg(m_id).arg(generation));
        m_memory->setPage(m_expectedBase, html.toUtf8());
        load(m_expectedBase);
    }
    QUrl expectedBase() const { return m_expectedBase; }
protected:
    bool acceptNavigationRequest(const QUrl &url, NavigationType type, bool mainFrame) override {
        // Authorize the exact generated document, regardless of navigation type.
        Q_UNUSED(type);
        return mainFrame && !m_expectedBase.isEmpty() && url == m_expectedBase;
    }
    QWebEnginePage *createWindow(WebWindowType) override { return nullptr; }
private:
    MemoryPage *m_memory = nullptr;
    QString m_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QUrl m_expectedBase;
};

RenderResult renderSnapshot(QByteArray source, quint64 generation, quint64 revision) {
    const auto pointer = reinterpret_cast<const unsigned char *>(source.constData());
    auto buffer = xdnote_markdown_render(pointer, std::size_t(source.size()));
    const auto release = qScopeGuard([&] { xdnote_markdown_free(&buffer); });
    if (buffer.status != 0 || !buffer.data || buffer.length > 8 * 1024 * 1024)
        return {{}, QStringLiteral("预览失败：无效源文或生成内容过大"), generation, revision};
    return {QByteArray(reinterpret_cast<const char *>(buffer.data), qsizetype(buffer.length)), {}, generation, revision};
}

QString pageFor(const QByteArray &body, quint64 generation, int padding) {
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
        "<style>body{margin:%10px auto;padding:0 %10px;max-width:%2px;font:%3px/%4 system-ui,sans-serif;"
        "color:%5;background:%6;overflow-wrap:anywhere}pre{white-space:pre-wrap;background:%7;padding:16px}"
        "code{font-family:monospace}table{border-collapse:collapse}td,th{border:1px solid %8;padding:6px 12px}"
        ".link-placeholder{color:%9;text-decoration:underline}.image-placeholder{color:%9}"
        "h1{font-size:%11px}h2{font-size:%12px}h3{font-size:%13px}"
        "blockquote{border-left:3px solid %8;margin-left:0;padding-left:16px}</style></head><body>")
        .arg(generation).arg(reader.value(QStringLiteral("maxContentWidth")).toInt())
        .arg(reader.value(QStringLiteral("size")).toInt()).arg(reader.value(QStringLiteral("lineHeight")).toDouble())
        .arg(colors.value(QStringLiteral("text")).toString(), colors.value(QStringLiteral("background")).toString(),
             colors.value(QStringLiteral("codeSurface")).toString(), colors.value(QStringLiteral("border")).toString(),
             colors.value(QStringLiteral("textSecondary")).toString())
        .arg(padding).arg(typography.value("headings").toObject().value("documentH1").toInt())
        .arg(typography.value("headings").toObject().value("documentH2").toInt())
        .arg(typography.value("headings").toObject().value("documentH3").toInt());
    return head + QString::fromUtf8(body) + QStringLiteral("</body></html>");
}
}

MarkdownPreview::MarkdownPreview(DocumentSession *session, QWidget *parent, int padding)
    : QWidget(parent), m_session(session), m_padding(padding) {
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
                page->present(pageFor(result.html, result.generation, m_padding), result.generation);
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
