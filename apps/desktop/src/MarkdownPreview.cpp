#include "MarkdownPreview.h"
#include "MarkdownBridge.h"
#include "MarkdownHeadings.h"
#include "WorkspaceImagePreview.h"
#include "WorkspacePath.h"
#include "DocumentSession.h"
#include "VisualTokens.h"
#include "Appearance.h"
#include "SharedWidgets.h"
#include "UiFonts.h"

#include <QJsonObject>
#include <QBuffer>
#include <QUuid>
#include <QScopeGuard>
#include <QVBoxLayout>
#include <QWebEnginePage>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineLoadingInfo>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineUrlSchemeHandler>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineView>
#include <QRegularExpression>
#include <QtConcurrentRun>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <limits>

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
        // Only generated documents and in-memory image tokens may use the
        // private scheme. The handler independently matches exact image URLs.
        const bool bootstrap = request.resourceType() == QWebEngineUrlRequestInfo::ResourceTypeMainFrame
            && (request.requestUrl().scheme() == QStringLiteral("xdnote-preview")
                || request.requestUrl() == QUrl(QStringLiteral("about:blank")));
        const bool image = request.resourceType() == QWebEngineUrlRequestInfo::ResourceTypeImage
            && request.requestUrl().scheme() == QStringLiteral("xdnote-preview");
        request.block(!bootstrap && !image);
    }
};

class MemoryPage final : public QWebEngineUrlSchemeHandler {
public:
    using QWebEngineUrlSchemeHandler::QWebEngineUrlSchemeHandler;
    void setPage(const QUrl &url, QByteArray html, QHash<QUrl, QByteArray> images) {
        m_url = url; m_html = std::move(html); m_images = std::move(images);
    }
    void clearImages() { m_images.clear(); }
    void requestStarted(QWebEngineUrlRequestJob *request) override {
        // Runs on the owning GUI thread. No path is ever resolved on disk.
        if (request->requestMethod() != "GET" || m_url.isEmpty()) {
            request->fail(QWebEngineUrlRequestJob::RequestDenied); return;
        }
        auto *buffer = new QBuffer(request);
        if (request->requestUrl() == m_url) {
            buffer->setData(m_html); buffer->open(QIODevice::ReadOnly); request->reply("text/html", buffer); return;
        }
        const auto image = m_images.constFind(request->requestUrl());
        if (image == m_images.cend()) { delete buffer; request->fail(QWebEngineUrlRequestJob::RequestDenied); return; }
        buffer->setData(*image); buffer->open(QIODevice::ReadOnly); request->reply("image/png", buffer);
    }
private:
    QUrl m_url;
    QByteArray m_html;
    QHash<QUrl, QByteArray> m_images;
};

class RestrictedPage final : public QWebEnginePage {
public:
    RestrictedPage(QWebEngineProfile *profile, QObject *parent) : QWebEnginePage(profile, parent) {
        m_memory = new MemoryPage(profile);
        profile->installUrlSchemeHandler("xdnote-preview", m_memory);
        QWebEngineScript controls;
        controls.setName("xdnote-code-copy"); controls.setWorldId(QWebEngineScript::ApplicationWorld); controls.setInjectionPoint(QWebEngineScript::DocumentReady);
        controls.setSourceCode(QStringLiteral(
            "(()=>{let tab=false;const clear=()=>document.querySelectorAll('.xdnote-keyboard-focus').forEach(e=>e.classList.remove('xdnote-keyboard-focus'));"
            "document.addEventListener('keydown',e=>{if(e.key==='Tab')tab=true;else if(['Enter',' ','Escape'].includes(e.key)){tab=false;clear();}},true);"
            "document.addEventListener('focusin',e=>{clear();if(tab)e.target.classList.add('xdnote-keyboard-focus');tab=false;},true);"
            "document.addEventListener('pointerdown',()=>{tab=false;clear();},true);window.addEventListener('blur',clear);"
            "document.addEventListener('click',function(e){const b=e.target.closest('button[data-xdnote-code]');if(!b||!e.isTrusted)return;clear();const g=document.querySelector('meta[name=xdnote-generation]');if(g)console.info('%1'+g.content+':'+b.dataset.xdnoteCode);},true);})();").arg(m_copyPrefix));
        scripts().insert(controls);
    }
    std::function<void(quint64, int)> copyRequested;
    std::function<void(quint64, int)> fragmentRequested;
    QString imagePrefix(quint64 generation) const { return QStringLiteral("xdnote-preview:/%1/%2/images/").arg(m_id).arg(generation); }
    QString linkPrefix(quint64 generation) const { return QStringLiteral("xdnote-preview:/%1/%2/links/").arg(m_id).arg(generation); }
    void clearImages() { m_memory->clearImages(); }
    void present(const QString &html, quint64 generation, const QHash<int, QByteArray> &images) {
        m_expectedGeneration = generation;
        m_expectedBase = QUrl(QStringLiteral("xdnote-preview:/%1/%2").arg(m_id).arg(generation));
        QHash<QUrl, QByteArray> resources;
        for (auto it = images.cbegin(); it != images.cend(); ++it) resources.insert(QUrl(imagePrefix(generation) + QString::number(it.key())), it.value());
        m_memory->setPage(m_expectedBase, html.toUtf8(), std::move(resources));
        load(m_expectedBase);
    }
    QUrl expectedBase() const { return m_expectedBase; }
protected:
    bool acceptNavigationRequest(const QUrl &url, NavigationType type, bool mainFrame) override {
        // Never hand document-controlled URLs to WebEngine or the OS. A click
        // may name only a numeric token generated for this exact page.
        const auto prefix = linkPrefix(m_expectedGeneration);
        const auto encoded = url.toString(QUrl::FullyEncoded);
        if (mainFrame && type == QWebEnginePage::NavigationTypeLinkClicked && !m_expectedBase.isEmpty()
            && encoded.startsWith(prefix)) {
            const auto suffix = encoded.mid(prefix.size()); bool valid = false;
            const auto index = suffix.toInt(&valid);
            if (valid && index >= 0 && suffix == QString::number(index) && fragmentRequested)
                fragmentRequested(m_expectedGeneration, index);
            return false;
        }
        // Authorize only the exact generated document, regardless of type.
        return mainFrame && !m_expectedBase.isEmpty() && url == m_expectedBase;
    }
    QWebEnginePage *createWindow(WebWindowType) override { return nullptr; }
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel, const QString &message, int, const QString &) override {
        if (!message.startsWith(m_copyPrefix)) return;
        const auto match = QRegularExpression("^([0-9]{1,20}):([0-9]{1,7})$").match(message.mid(m_copyPrefix.size()));
        if (!match.hasMatch()) return; bool valid = false; const auto generation = match.captured(1).toULongLong(&valid); const int index = match.captured(2).toInt();
        if (valid && index < 2 * 1024 * 1024 && copyRequested) copyRequested(generation, index);
    }
private:
    MemoryPage *m_memory = nullptr;
    QString m_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString m_copyPrefix = "xdnote-copy:" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ':';
    QUrl m_expectedBase;
    quint64 m_expectedGeneration = 0;
};

RenderResult renderSnapshot(QByteArray source, QUuid documentId, quint64 generation, quint64 revision,
    const std::shared_ptr<WorkspaceResourceReader> &root, const QString &documentPath,
    const QString &documentIdentity, const QByteArray &prefix, const AuthorizedPreviewImages &authorizedImages) {
    const DocumentSnapshot snapshot{EditResult::Applied, source, revision, documentId};
    const auto prepared = preparePreviewImages(snapshot, root, documentPath, documentIdentity, authorizedImages);
    QStringList headingIds;
    bool enableFragments = false;
    if (prepared.analyzed) {
        const auto headings = analyzeMarkdownHeadings(snapshot);
        if (headings.success) {
            for (const auto &heading : headings.headings) headingIds.append(heading.id);
            enableFragments = true;
        }
    }
    const auto pointer = reinterpret_cast<const unsigned char *>(source.constData());
    auto renderWithResources = enableFragments ? xdnote_markdown_render_images_and_fragments : xdnote_markdown_render_images;
    if (enableFragments && root && root->active()) renderWithResources = xdnote_markdown_render_images_and_links;
    auto buffer = prepared.analyzed ? renderWithResources(pointer, std::size_t(source.size()), reinterpret_cast<const unsigned char *>(prefix.constData()), std::size_t(prefix.size()),
        reinterpret_cast<const unsigned char *>(prepared.states.constData()), std::size_t(prepared.states.size())) : xdnote_markdown_render(pointer, std::size_t(source.size()));
    const auto release = qScopeGuard([&] { xdnote_markdown_free(&buffer); });
    if (buffer.status != 0 || !buffer.data || buffer.length > 8 * 1024 * 1024)
        return {{}, {}, QStringLiteral("预览失败：无效源文或生成内容过大"), generation, revision};
    return {QByteArray(reinterpret_cast<const char *>(buffer.data), qsizetype(buffer.length)), prepared.png, {}, generation, revision, prepared.linkTargets, headingIds, prepared.watchedPaths};
}

QString pageFor(const QByteArray &body, quint64 generation, int padding, Appearance *appearance) {
    const auto typography = visualTokens().value(QStringLiteral("typography")).toObject();
    const auto reader = typography.value(QStringLiteral("reader")).toObject();
    const auto colors = visualTokens().value("themes").toObject().value(appearance->scheme()).toObject();
    // All markup and CSS around the body are application-owned. Raw source HTML
    // is escaped; admitted resources and fragment actions use generated tokens.
    QString head = QStringLiteral(
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"xdnote-generation\" content=\"%1\">"
        "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; script-src 'none'; "
        "style-src 'unsafe-inline'; img-src xdnote-preview:; connect-src 'none'; object-src 'none'; "
        "frame-src 'none'; form-action 'none'; base-uri 'none'\">"
        "<style>:root{color-scheme:%14}body{margin:%10px auto;padding:0 %10px;max-width:%2px;font:%3px/%4 system-ui,sans-serif;"
        "color:%5;background:%6;overflow-wrap:anywhere}pre{white-space:pre-wrap;background:%7;padding:16px}"
        "code{font-family:monospace}table{border-collapse:collapse}td,th{border:1px solid %8;padding:6px 12px}"
        ".link-placeholder{color:%9;text-decoration:underline}a.link-placeholder{cursor:pointer}.image-placeholder{color:%9}img{max-width:100%;height:auto}"
        "h1{font-size:%11px}h2{font-size:%12px}h3{font-size:%13px}h1,h2,h3{line-height:1.4;margin:1.2em 0 .65em}"
        "body>:first-child{margin-top:0}pre{border:1px solid %8;border-radius:6px}"
        "input[type=checkbox]{accent-color:%15;pointer-events:none}::selection{background:%16;color:%17}"
        "::-webkit-scrollbar{width:%18px;height:%18px}::-webkit-scrollbar-track{background:%6}::-webkit-scrollbar-thumb{background:%9;border:%19px solid %6;border-radius:%20px;min-height:%21px}"
        "blockquote{border-left:3px solid %8;margin-left:0;padding-left:16px}</style></head><body>")
        .arg(generation).arg(reader.value(QStringLiteral("maxContentWidth")).toInt())
        .arg(reader.value(QStringLiteral("size")).toInt()).arg(reader.value(QStringLiteral("lineHeight")).toDouble())
        .arg(colors.value(QStringLiteral("text")).toString(), colors.value(QStringLiteral("background")).toString(),
             colors.value(QStringLiteral("codeSurface")).toString(), colors.value(QStringLiteral("border")).toString(),
             colors.value(QStringLiteral("textSecondary")).toString())
        .arg(padding).arg(typography.value("headings").toObject().value("documentH1").toInt())
        .arg(typography.value("headings").toObject().value("documentH2").toInt())
        .arg(typography.value("headings").toObject().value("documentH3").toInt())
        .arg(appearance->scheme(), appearance->accentColor().name(), appearance->accentColor("soft").name(), appearance->accentColor("softForeground").name())
        .arg(visualTokens().value("controls").toObject().value("scrollbarWidth").toInt())
        .arg(visualTokens().value("controls").toObject().value("scrollbarInset").toInt())
        .arg(visualTokens().value("controls").toObject().value("scrollbarWidth").toInt() / 2)
        .arg(visualTokens().value("controls").toObject().value("scrollbarHandleMin").toInt());
    head.replace("system-ui,sans-serif", readerFontCss()); head.replace("font-family:monospace", "font-family:" + sourceFontCss());
    const QString components = QStringLiteral("<style>"
        "figure.code-block{margin:1em 0;border:1px solid %1;border-radius:8px;overflow:hidden;background:%2}"
        ".code-block figcaption{display:flex;align-items:center;justify-content:space-between;gap:16px;padding:4px 12px;border-bottom:1px solid %1;font:12px/24px %3;color:%4}"
        ".code-block button{border:0;border-radius:4px;background:transparent;color:%4;font:14px/24px %3;padding:2px 8px;cursor:pointer}"
        ".code-block button:hover{background:%5;color:%6}button:focus,a:focus,pre:focus,.table-scroll:focus{outline:none}.xdnote-keyboard-focus:focus{outline:2px solid %7;outline-offset:-2px}"
        ".code-block pre{margin:0;max-width:100%;overflow-x:auto;white-space:pre;overflow-wrap:normal;border:0;border-radius:0;padding:12px 16px}"
        "code{font-size:14px;line-height:1.8}.table-scroll{max-width:100%;overflow-x:auto;margin:1em 0}.table-scroll table{width:max-content;min-width:100%}.table-scroll td,.table-scroll th{white-space:nowrap;overflow-wrap:normal}"
        "li:has(>input[type=checkbox]){list-style:none}ul:has(>li>input[type=checkbox]){padding-left:4px}"
        "input[type=checkbox]{appearance:none;display:inline-block;width:16px;height:16px;margin:0 8px 0 0;vertical-align:-2px;border:1.5px solid %4;border-radius:4px;background:transparent;opacity:1}"
        "input[type=checkbox]:checked{background:%7;border-color:%7}input[type=checkbox]:checked:after{content:'';display:block;width:7px;height:4px;border-left:2px solid %8;border-bottom:2px solid %8;transform:translate(3px,3px) rotate(-45deg)}"
        ".syntax-keyword{color:%9}.syntax-string,.syntax-number{color:%10}.syntax-comment{color:%4}"
        "</style>")
        .arg(appearance->color("border").name(), appearance->color("codeSurface").name(), readerFontCss(), appearance->color("textSecondary").name(), appearance->accentColor("soft").name(), appearance->color("text").name(), appearance->accentColor().name(), appearance->accentColor("foreground").name(), appearance->color("syntaxHeading").name(), appearance->color("syntaxCode").name());
    head.replace("</head>", components + "</head>");
    return head + QString::fromUtf8(body) + QStringLiteral("</body></html>");
}
}

MarkdownPreview::MarkdownPreview(DocumentSession *session, QWidget *parent, int padding, Appearance *appearance)
    : QWidget(parent), m_session(session), m_padding(padding), m_appearance(appearance ? appearance : new Appearance(this)) {
    m_view = new QWebEngineView(this);
    m_view->setObjectName(QStringLiteral("markdownPreview"));
    auto *profile = new QWebEngineProfile(this);
    profile->setHttpCacheType(QWebEngineProfile::NoCache);
    profile->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
    profile->setUrlRequestInterceptor(new BlockResourceRequests(profile));
    auto *page = new RestrictedPage(profile, m_view);
    m_view->setPage(page);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_view, &QWidget::customContextMenuRequested, this, [this, page](const QPoint &position) {
        SharedMenu menu(m_appearance, m_view); menu.setObjectName("readingContextMenu");
        auto *copy = menu.addAction(themedIcon("copy", m_appearance->color("text")), QStringLiteral("复制")); copy->setEnabled(page->hasSelection()); connect(copy, &QAction::triggered, page, [page] { page->triggerAction(QWebEnginePage::Copy); });
        auto *select = menu.addAction(themedIcon("select-all", m_appearance->color("text")), QStringLiteral("全选")); connect(select, &QAction::triggered, page, [page] { page->triggerAction(QWebEnginePage::SelectAll); });
        menu.exec(m_view->mapToGlobal(position));
    });
    page->copyRequested = [this, page](quint64 generation, int index) {
        if (m_closed || generation != m_generation || generation != m_presentingGeneration || !currentRevision(m_session, m_displayedRevision)) return;
        const QPointer<MarkdownPreview> guard(this);
        page->runJavaScript(QStringLiteral("(()=>{const c=document.querySelectorAll('figure.code-block pre code')[%1];return c?c.textContent:null})()").arg(index), QWebEngineScript::ApplicationWorld,
            [guard, generation](const QVariant &value) { if (guard && !guard->m_closed && generation == guard->m_generation && value.isValid() && currentRevision(guard->m_session, guard->m_displayedRevision)) { const auto text = value.toString(); if (text.toUtf8().size() <= 2 * 1024 * 1024) emit guard->copyTextRequested(text); } });
    };
    page->fragmentRequested = [this, page](quint64 generation, int index) {
        if (m_closed || generation != m_generation || generation != m_presentingGeneration
            || m_displayedRevision != m_presentingRevision || !currentRevision(m_session, m_displayedRevision)
            || index < 0 || index >= m_presentingLinkTargets.size()) return;
        const auto target = m_presentingLinkTargets.at(index);
        if (!target.startsWith('#')) { emit documentLinkRequested(target, m_displayedRevision); return; }
        const auto resolved = resolveWorkspaceTarget(QStringLiteral("document.md"), target);
        if (!resolved.success || !resolved.hasFragment || resolved.fragment.isEmpty()) {
            emit fragmentNavigationMessage(QStringLiteral("链接中的标题标识无效。")); return;
        }
        navigateToHeading(resolved.fragment, m_displayedRevision);
    };
    page->setBackgroundColor(m_appearance->color("background"));
    connect(m_appearance, &Appearance::changed, this, [this, page] { page->setBackgroundColor(m_appearance->color("background")); refresh(); });
    connect(page, &QWebEnginePage::scrollPositionChanged, this, [this](const QPointF &position) { if (!m_restoring) m_scroll = position; });
    auto *settings = page->settings();
    for (auto option : {QWebEngineSettings::JavascriptEnabled,
                       QWebEngineSettings::LocalStorageEnabled, QWebEngineSettings::PluginsEnabled,
                       QWebEngineSettings::LocalContentCanAccessFileUrls,
                       QWebEngineSettings::LocalContentCanAccessRemoteUrls,
                       QWebEngineSettings::JavascriptCanAccessClipboard, QWebEngineSettings::WebGLEnabled})
        settings->setAttribute(option, false);
    settings->setAttribute(QWebEngineSettings::AutoLoadImages, true);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_view);
    connect(session, &DocumentSession::changed, this, &MarkdownPreview::refresh, Qt::QueuedConnection);
    m_imageDelay.setSingleShot(true); m_imageDelay.setInterval(180);
    const auto imageChanged = [this, page] { page->clearImages(); m_imageDelay.start(); };
    connect(&m_imageWatch, &QFileSystemWatcher::fileChanged, this, imageChanged);
    connect(&m_imageWatch, &QFileSystemWatcher::directoryChanged, this, imageChanged);
    connect(&m_imageDelay, &QTimer::timeout, this, &MarkdownPreview::refresh);
    connect(session, &DocumentSession::closed, this, [this, page] {
        m_closed = true; ++m_generation; m_hasPending = false; m_presentingLinkTargets.clear(); m_presentingHeadingIds.clear(); m_view->stop(); page->clearImages();
    });
    connect(session, &QObject::destroyed, this, [this, page] { m_closed = true; ++m_generation; m_hasPending = false; m_presentingLinkTargets.clear(); m_presentingHeadingIds.clear(); page->clearImages(); });
    connect(&m_worker, &QFutureWatcher<RenderResult>::finished, this, [this, page] {
        const auto result = m_worker.result();
        m_running = false;
        if (!m_closed && currentRevision(m_session, result.revision) && result.generation == m_generation) {
            if (!result.error.isEmpty()) emit previewError(result.error);
            else {
                m_presentingGeneration = result.generation;
                m_presentingRevision = result.revision;
                m_presentingLinkTargets = result.linkTargets;
                m_presentingHeadingIds = result.headingIds;
                const auto previous = m_imageWatch.files() + m_imageWatch.directories();
                if (!previous.isEmpty()) m_imageWatch.removePaths(previous);
                if (!result.imageWatchPaths.isEmpty()) m_imageWatch.addPaths(result.imageWatchPaths);
                m_restoring = true;
                page->present(pageFor(result.html, result.generation, m_padding, m_appearance), result.generation, result.images);
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
            guard->m_pageReady = true;
            guard->restoreReadingPosition(guard->m_scroll);
            emit guard->previewReady(revision);
            if (guard && guard->m_navigationPending) guard->navigateToHeading(guard->m_pendingFragment, guard->m_navigationRevision);
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

void MarkdownPreview::restoreReadingPosition(const QPointF &position) {
    if (m_closed || !std::isfinite(position.x()) || !std::isfinite(position.y())) return;
    m_scroll = QPointF(qBound(0.0, position.x(), 10000000.0), qBound(0.0, position.y(), 10000000.0));
    m_restoring = true;
    // Fixed application-owned code in an isolated world; no document text or
    // URL is interpolated. MainWorld scripts, resources and navigation stay off.
    const auto script = QStringLiteral("window.scrollTo(%1,%2)").arg(qRound(m_scroll.x())).arg(qRound(m_scroll.y()));
    const auto generation = m_generation;
    const QPointer<MarkdownPreview> guard(this);
    m_view->page()->runJavaScript(script, QWebEngineScript::ApplicationWorld, [guard, generation](const QVariant &) {
        if (guard && !guard->m_closed && generation == guard->m_generation) guard->m_restoring = false;
    });
}

MarkdownPreview::~MarkdownPreview() {
    m_closed = true;
    // The worker owns only an immutable byte snapshot and frees Rust output in
    // its own scope. It never captures this widget or the active document.
    disconnect(&m_worker, nullptr, this, nullptr);
    delete m_view;
    m_view = nullptr;
}

void MarkdownPreview::navigateToHeading(const QString &fragment, quint64 revision) {
    if (m_closed || !currentRevision(m_session, revision)) {
        m_navigationPending = false; emit fragmentNavigationMessage(QStringLiteral("文档已变化，请重新定位标题。")); return;
    }
    if (!m_pageReady || m_displayedRevision != revision || m_presentingGeneration != m_generation) {
        m_pendingFragment = fragment; m_navigationRevision = revision; m_navigationPending = true; return;
    }
    m_navigationPending = false;
    const int heading = m_presentingHeadingIds.indexOf(fragment);
    if (heading < 0) { emit fragmentNavigationMessage(QStringLiteral("未找到标题：%1").arg(fragment.left(120))); return; }
    const auto generation = m_generation; const QPointer<MarkdownPreview> guard(this);
    const auto script = QStringLiteral("(()=>{const h=document.querySelectorAll('h1[id],h2[id],h3[id],h4[id],h5[id],h6[id]')[%1];if(!h)return false;h.scrollIntoView({block:'start'});return true;})()").arg(heading);
    m_view->page()->runJavaScript(script, QWebEngineScript::ApplicationWorld, [guard, generation, revision, fragment](const QVariant &value) {
        if (!guard || guard->m_closed || generation != guard->m_generation || !currentRevision(guard->m_session, revision)) return;
        emit guard->fragmentNavigationMessage(value.toBool() ? QString() : QStringLiteral("无法定位标题，请重新打开链接。"));
        if (guard && value.toBool()) emit guard->fragmentNavigated(fragment);
    });
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
    m_pageReady = false;
    if (m_navigationPending && snapshot.revision != m_navigationRevision) {
        m_navigationPending = false; emit fragmentNavigationMessage(QStringLiteral("文档已变化，请重新定位标题。"));
    }
    // A new source revision invalidates any image tokens still owned by the
    // previous render, even before the replacement worker finishes.
    static_cast<RestrictedPage *>(m_view->page())->clearImages();
    m_pending = snapshot.utf8;
    m_pendingDocumentId = snapshot.documentId;
    m_pendingRevision = snapshot.revision;
    m_hasPending = true;
    startPending();
}

void MarkdownPreview::setResourceContext(std::shared_ptr<WorkspaceResourceReader> root, const QString &documentPath, const QString &documentIdentity, const AuthorizedPreviewImages &authorizedImages) {
    if (m_closed || (m_resourceRoot == root && m_resourceDocumentPath == documentPath && m_resourceDocumentIdentity == documentIdentity && m_authorizedImages == authorizedImages)) return;
    const bool cancelledNavigation = m_navigationPending; m_navigationPending = false;
    m_resourceRoot = std::move(root); m_resourceDocumentPath = documentPath; m_resourceDocumentIdentity = documentIdentity;
    m_authorizedImages = authorizedImages;
    m_imageDelay.stop();
    const auto previousWatches = m_imageWatch.files() + m_imageWatch.directories();
    if (!previousWatches.isEmpty()) m_imageWatch.removePaths(previousWatches);
    if (m_generation == std::numeric_limits<quint64>::max()) { m_closed = true; return; }
    ++m_generation; m_hasPending = false;
    static_cast<RestrictedPage *>(m_view->page())->clearImages();
    refresh();
    if (cancelledNavigation) emit fragmentNavigationMessage(QStringLiteral("文档位置已变化，请重新定位标题。"));
}

void MarkdownPreview::startPending() {
    if (m_closed || m_running || !m_hasPending) return;
    m_running = true;
    m_hasPending = false;
    auto snapshot = std::move(m_pending);
    const auto generation = m_generation;
    const auto revision = m_pendingRevision;
    const auto documentId = m_pendingDocumentId;
    const auto root = m_resourceRoot;
    const auto authorizedImages = m_authorizedImages;
    const auto documentPath = m_resourceDocumentPath, documentIdentity = m_resourceDocumentIdentity;
    const auto prefix = static_cast<RestrictedPage *>(m_view->page())->imagePrefix(generation).toUtf8();
    m_worker.setFuture(QtConcurrent::run([snapshot = std::move(snapshot), documentId, generation, revision, root, documentPath, documentIdentity, prefix, authorizedImages] {
        return renderSnapshot(snapshot, documentId, generation, revision, root, documentPath, documentIdentity, prefix, authorizedImages);
    }));
}
}
