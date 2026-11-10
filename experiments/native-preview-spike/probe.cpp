// Isolated read-only candidates, not a product renderer or source-text owner.
#include "MarkdownBridge.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTimer>
#include <cstdio>
#ifdef WITH_WEBENGINE
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineView>
#endif

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false);
    if (app.arguments().size() != 3) return 2;
    bool valid = false; const int size = app.arguments()[1].toInt(&valid);
    if (!valid || size < 1024 || size > 2 * 1024 * 1024) return 2;
    const auto block = QStringLiteral("# 中文标题 Heading\n\n中文、Latin 😀，**粗体**、*斜体*。\n\n- [x] 已完成\n- [ ] 待办\n\n|列|值|\n|--|--|\n|甲|乙|\n\n```cpp\nconst char *text = \"<script>literal</script>\";\n```\n\n<script>window.injected=true</script>\n\n正文段落，保留文字和换行。\n\n").toUtf8();
    QByteArray source; source.reserve(size);
    while (source.size() + block.size() <= size) source += block;
    source += QByteArray(size - source.size(), 'x');
    auto rendered = xdnote_markdown_render(reinterpret_cast<const unsigned char *>(source.constData()), size_t(source.size()));
    if (rendered.status) { xdnote_markdown_free(&rendered); return 3; }
    const auto body = QString::fromUtf8(reinterpret_cast<char *>(rendered.data), qsizetype(rendered.length));
    xdnote_markdown_free(&rendered);
    const auto html = "<!doctype html><html><head><meta charset=utf-8></head><body>" + body + "</body></html>";
    QTemporaryDir temporary; if (!temporary.isValid()) return 6;
    QFile fixture(temporary.filePath("generated.html"));
    if (!fixture.open(QIODevice::WriteOnly) || fixture.write(html.toUtf8()) < 0) return 6;
    fixture.close();
    QElapsedTimer timer; timer.start();
    const auto finish = [&](QWidget *widget, const QString &text, const char *kind) {
        const auto ms = timer.nsecsElapsed() / 1e6;
        const auto pixels = widget->grab().toImage(); int ink = 0;
        for (int y = 8; y < qMin(200, pixels.height()); y += 2)
            for (int x = 8; x < pixels.width() - 8; x += 2)
                if (qGray(pixels.pixel(x, y)) < 180) ++ink;
        const bool painted = ink > 20 && pixels.save(app.arguments()[2]);
        const QJsonObject report{{"kind", kind}, {"sourceBytes", size}, {"sourceSha256", QString::fromLatin1(QCryptographicHash::hash(source, QCryptographicHash::Sha256).toHex())},
            {"readyIncludingPaintSettleMs", ms}, {"paintSettleMs", 200}, {"chineseAndEmoji", text.contains(QStringLiteral("中文、Latin 😀"))},
            {"literalHtml", text.contains("<script>window.injected=true</script>")}, {"screenshot", painted},
            {"scope", "isolated read-only preview; not full product or performance acceptance"}};
        std::puts(QJsonDocument(report).toJson(QJsonDocument::Compact).constData()); std::fflush(stdout);
        QTimer::singleShot(2500, &app, &QCoreApplication::quit);
    };
#ifdef WITH_WEBENGINE
    QWebEngineProfile profile; QWebEnginePage page(&profile); QWebEngineView view; view.setPage(&page);
    page.settings()->setAttribute(QWebEngineSettings::JavascriptEnabled, false);
    page.settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
    page.settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
    view.resize(800, 600); view.show();
    QObject::connect(&page, &QWebEnginePage::loadFinished, &app, [&](bool ok) {
        if (!ok) { app.exit(4); return; }
        page.toPlainText([&](const QString &text) { QTimer::singleShot(200, &app, [&, text] { finish(&view, text, "webengine"); }); });
    });
    // setHtml uses a data URL with a separate 2 MiB URL limit. A bounded local
    // generated fixture avoids confusing that API limit with rendering cost.
    page.load(QUrl::fromLocalFile(fixture.fileName()));
#else
    QTextBrowser view; view.setOpenLinks(false); view.setOpenExternalLinks(false); view.setReadOnly(true);
    view.resize(800, 600); view.setHtml(html); view.show();
    QTimer::singleShot(200, &app, [&] { finish(&view, view.toPlainText(), "native"); });
#endif
    QTimer::singleShot(30000, &app, [&] { app.exit(5); });
    return app.exec();
}
