// Read-only, controlled-section layout experiment. Not a product renderer.
#include "MarkdownBridge.h"
#include "UiFonts.h"
#include <QAbstractScrollArea>
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QCache>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <cstdio>
#include <memory>
#include <stdexcept>

namespace {
constexpr int CacheDocuments = 8;
constexpr qsizetype SectionCharacterLimit = 65536;
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
class SafeDocument final : public QTextDocument {
public:
    explicit SafeDocument(QObject *parent = nullptr) : QTextDocument(parent) {
        setDefaultFont(xdnote::uiFont()); setDocumentMargin(0);
        setDefaultStyleSheet("body{color:#1f2937;font-size:14px} pre{white-space:pre-wrap} th{font-weight:bold}");
    }
    int denied = 0;
protected:
    QVariant loadResource(int, const QUrl &) override { ++denied; return {}; }
};
struct Section { QString html, id; };
QByteArray generatedSource(int bytes, const QString &flavor) {
    if (flavor == "paragraph") return QByteArray(bytes, 'x');
    if (flavor == "code") return "```\n" + QByteArray(bytes - 8, 'x') + "\n```";
    if (flavor == "table") {
        QByteArray result("|a|b|\n|--|--|\n"); while (result.size() + 6 <= bytes) result += "|x|y|\n";
        return result + QByteArray(bytes - result.size(), ' ');
    }
    const auto block = QStringLiteral("# 中文标题 Heading\n\n中文、Latin 😀，**粗体**、*斜体*。\n\n[跨节引用][shared]\n\n- [x] 已完成\n- [ ] 待办\n\n|列|值|\n|--|--|\n|甲|乙|\n\n```cpp\nconst char *text = \"<script>literal</script>\";\n```\n\n<script>window.injected=true</script>\n\n正文段落，保留文字和换行。\n\n").toUtf8();
    const QByteArray definition("\n\n[shared]: target.md\n");
    QByteArray source; source.reserve(bytes);
    while (source.size() + block.size() + definition.size() <= bytes) source += block;
    source += QByteArray(bytes - source.size() - definition.size(), 'x'); return source + definition;
}
QByteArray bridge(XdnoteRenderBuffer buffer) {
    if (buffer.status) { xdnote_markdown_free(&buffer); throw std::runtime_error("Rust admission rejected fixture"); }
    const QByteArray result(reinterpret_cast<const char *>(buffer.data), qsizetype(buffer.length));
    xdnote_markdown_free(&buffer); return result;
}
QList<Section> independentFixtureSections(const QByteArray &source, const QString &html) {
    // The fixture consists only of independent top-level H1 sections. This is
    // deliberately NOT an arbitrary Markdown/HTML block partitioner.
    const auto bytes = bridge(xdnote_markdown_headings(reinterpret_cast<const unsigned char *>(source.constData()), size_t(source.size())));
    const auto headings = QJsonDocument::fromJson(bytes).object().value("headings").toArray();
    QList<Section> result; qsizetype previous = 0; QString previousId;
    for (const auto &value : headings) {
        const auto heading = value.toObject(); require(heading.value("level").toInt() == 1, "fixture is not independent H1 sections");
        const auto id = heading.value("id").toString();
        const auto at = html.indexOf("<h1 id=\"" + id + "\">", previous);
        require(at >= previous, "heading map and rendered HTML differ");
        if (at > previous) result.append({html.mid(previous, at - previous), previousId});
        previous = at; previousId = id;
    }
    if (previous < html.size()) result.append({html.mid(previous), previousId});
    QString joined; joined.reserve(html.size()); for (const auto &section : result) joined += section.html;
    require(joined == html, "section partition changed global parser output"); return result;
}
class SectionWindow final : public QAbstractScrollArea {
public:
    explicit SectionWindow(const QList<Section> &sections) : m_sections(sections), m_cache(CacheDocuments) {
        verticalScrollBar()->setRange(0, int(sections.size()) - 1); verticalScrollBar()->setPageStep(1);
        connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { viewport()->update(); });
    }
    void selectSection(int index) { verticalScrollBar()->setValue(index); viewport()->update(); }
    int maximumCached = 0, layouts = 0;
    bool selectionLoaded(int index) const { return m_cache.contains(index); }
    QString firstPlainText() { return document(0)->toPlainText(); }
protected:
    void scrollContentsBy(int, int) override { viewport()->update(); }
    void resizeEvent(QResizeEvent *event) override { m_cache.clear(); QAbstractScrollArea::resizeEvent(event); }
    void paintEvent(QPaintEvent *) override {
        QPainter painter(viewport()); painter.fillRect(viewport()->rect(), QColor("#fcfcfb"));
        qreal y = 24; int index = verticalScrollBar()->value();
        while (index < m_sections.size() && y < viewport()->height()) {
            auto *item = document(index++); const auto height = item->documentLayout()->documentSize().height();
            painter.save(); painter.translate(24, y); item->drawContents(&painter, QRectF(0, 0, viewport()->width() - 48, viewport()->height() - y)); painter.restore();
            y += height + 16;
        }
    }
private:
    SafeDocument *document(int index) {
        if (auto *value = m_cache.object(index)) return value;
        auto *value = new SafeDocument; value->setHtml(m_sections[index].html);
        value->setTextWidth(qMax(1, viewport()->width() - 48)); m_cache.insert(index, value); ++layouts;
        maximumCached = qMax(maximumCached, int(m_cache.size())); return value;
    }
    const QList<Section> &m_sections;
    QCache<int, SafeDocument> m_cache;
};
bool capture(QWidget *widget, const QString &path) {
    const auto image = widget->grab().toImage(); int ink = 0;
    for (int y = 8; y < qMin(200, image.height()); y += 2)
        for (int x = 8; x < image.width() - 8; x += 2) if (qGray(image.pixel(x, y)) < 180) ++ink;
    return ink > 20 && image.save(path);
}
}

int main(int argc, char **argv) {
    QApplication app(argc, argv); xdnote::installFontFallbacks(); app.setQuitOnLastWindowClosed(false);
    QJsonObject report;
    try {
        const auto args = app.arguments(); require(args.size() == 5, "expected mode, bytes, fixture, screenshot path");
        const auto mode = args[1], flavor = args[3]; require(mode == "whole" || mode == "windowed", "unknown candidate");
        bool valid = false; const int bytes = args[2].toInt(&valid); require(valid && bytes >= 10240 && bytes <= 2 * 1024 * 1024, "bounded fixture size required");
        const auto source = generatedSource(bytes, flavor); const auto originalHash = QCryptographicHash::hash(source, QCryptographicHash::Sha256);
        QElapsedTimer elapsed; elapsed.start();
        const auto body = QString::fromUtf8(bridge(xdnote_markdown_render(reinterpret_cast<const unsigned char *>(source.constData()), size_t(source.size()))));
        const auto sections = independentFixtureSections(source, body);
        report = {{"mode", mode}, {"fixture", flavor}, {"sourceBytes", source.size()}, {"sections", sections.size()},
                  {"sourceSha256", QString::fromLatin1(originalHash.toHex())}, {"modelMs", elapsed.nsecsElapsed() / 1e6},
                  {"scope", "controlled sections, independent read-only renderer, not a product implementation"}};
        bool oversized = false; for (const auto &section : sections) oversized |= section.html.size() > SectionCharacterLimit;
        if (mode == "windowed" && oversized) {
            report.insert("supported", false); report.insert("gap", "single-section-exceeds-experimental-quota; general AST and within-block virtualization still required");
        } else {
            std::unique_ptr<QWidget> widget;
            SafeDocument resourceGuard; resourceGuard.setHtml("<img src='file:///xdnote-probe-missing.png'><img src='https://example.invalid/a.png'>");
            resourceGuard.setTextWidth(700); resourceGuard.documentLayout()->documentSize(); require(resourceGuard.denied >= 2, "resource hook did not deny local and remote input");
            if (mode == "whole") {
                auto view = std::make_unique<QTextBrowser>(); auto *document = new SafeDocument(view.get());
                view->setDocument(document); view->setReadOnly(true); view->setOpenLinks(false); view->setOpenExternalLinks(false); view->setHtml(body); widget = std::move(view);
            } else widget = std::make_unique<SectionWindow>(sections);
            widget->resize(800, 600); widget->show(); QApplication::processEvents();
            require(capture(widget.get(), args[4]), "first frame blank or cannot save screenshot");
            report.insert("firstObservedFrameMs", elapsed.nsecsElapsed() / 1e6);
            QElapsedTimer tour; tour.start();
            if (auto *view = dynamic_cast<SectionWindow *>(widget.get())) {
                const auto firstText = view->firstPlainText(); require(firstText.contains(QStringLiteral("中文、Latin 😀")), "Unicode text was lost");
                require(firstText.contains(QStringLiteral("跨节引用")) && !firstText.contains("[shared]"), "global reference context was lost");
                int positions = 0;
                for (const double fraction : {0.0, 0.5, 1.0, 0.25, 0.75, 0.0}) {
                    const int index = qRound((sections.size() - 1) * fraction); view->selectSection(index); QApplication::processEvents(); widget->grab();
                    require(view->selectionLoaded(index), "requested section not laid out"); ++positions;
                }
                require(view->maximumCached <= CacheDocuments, "cache exceeded quota");
                report.insert("maxCachedDocuments", view->maximumCached); report.insert("laidOutDocuments", view->layouts); report.insert("visitedPositions", positions);
            } else {
                auto *wholeView = static_cast<QTextBrowser *>(widget.get()); require(wholeView->toPlainText().contains(QStringLiteral("中文、Latin 😀")), "Unicode text was lost");
            }
            report.insert("tourMs", tour.nsecsElapsed() / 1e6); report.insert("deniedResourceRequests", resourceGuard.denied);
            report.insert("sourceUnchanged", QCryptographicHash::hash(source, QCryptographicHash::Sha256) == originalHash);
            report.insert("supported", true); report.insert("screenshot", true);
            // Keep diagnostic widgets alive briefly for the process-tree sampler.
            QTimer::singleShot(600, &app, &QCoreApplication::quit); app.exec();
        }
        report.insert("ok", true);
    } catch (const std::exception &error) { report.insert("ok", false); report.insert("error", error.what()); }
    std::puts(QJsonDocument(report).toJson(QJsonDocument::Compact).constData());
    return report.value("ok").toBool() ? 0 : 1;
}
