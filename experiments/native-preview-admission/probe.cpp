#include "NativePreviewView.h"
#include "Appearance.h"
#include "UiFonts.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFontInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScrollBar>
#include <QTimer>
#include <cstdio>
#include <stdexcept>

using namespace xdnote;
namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
QByteArray fixture(int bytes, const QString &flavor) {
    if (flavor == "idle") return {};
    if (flavor == "paragraph") return QByteArray(bytes, 'x');
    if (flavor == "code") return "```cpp\n" + QByteArray(bytes - 11, 'x') + "\n```";
    QByteArray source;
    if (flavor == "table") {
        source = "|a|b|\n|--|--|\n";
        while (source.size() + 6 <= bytes) source += "|x|y|\n";
        return source;
    }
    const auto unit = flavor == "many" ? QByteArray("a b\n\n") :
        QStringLiteral("# 中文标题\n\n中文与 Latin 😀，**粗体** *斜体* ~~删除~~。\n\n[文档引用][shared]\n\n- [x] 完成\n- [ ] 待办\n\n|列|状态|\n|:--|--:|\n|甲|乙|\n\n```cpp\nconst char *text = \"<script>literal</script>\";\n```\n\n").toUtf8();
    const QByteArray definition("\n\n[shared]: nested/target.md\n");
    while (source.size() + unit.size() + definition.size() <= bytes) source += unit;
    source += QByteArray(bytes - source.size() - definition.size(), 'x'); return source + definition;
}
}
int main(int argc, char **argv) {
    QApplication application(argc, argv); installFontFallbacks(); application.setQuitOnLastWindowClosed(false);
    QJsonObject result;
    try {
        const auto args = application.arguments(); require(args.size() == 4, "expected bytes, fixture, screenshot");
        bool valid = false; const int bytes = args[1].toInt(&valid);
        require(valid && bytes >= 10240 && bytes <= 2 * 1024 * 1024, "fixture size outside existing source scope");
        const auto source = fixture(bytes, args[2]);
        const auto digest = QCryptographicHash::hash(source, QCryptographicHash::Sha256);
        QElapsedTimer timer; timer.start(); QString error;
        auto model = NativePreviewModel::parse(source, &error); require(bool(model), qPrintable(error));
        const qreal parsed = timer.nsecsElapsed() / 1e6; const auto encoded = model->encodedBytes();
        auto projected = NativePreviewContent::build(std::move(*model), {}, &error); require(bool(projected), qPrintable(error));
        auto content = std::make_shared<NativePreviewContent>(std::move(*projected));
        const qreal projection = timer.nsecsElapsed() / 1e6;
        Appearance appearance; appearance.setPreferences("light", "graphite");
        NativePreviewView view(&appearance); view.resize(800, 600);
        view.show(); QApplication::processEvents();
        const NativePreviewStamp stamp{QUuid::createUuid(), 1, 1, 1, 1}; view.expect(stamp);
        require(view.present(stamp, content), qPrintable(view.error()));
        const qreal plan = timer.nsecsElapsed() / 1e6;
        QApplication::processEvents(); const auto image = view.grab().toImage();
        require(image.save(args[3]), "cannot save native first frame");
        const qreal frame = timer.nsecsElapsed() / 1e6;
        view.selectAll(); QString copied;
        QObject::connect(&view, &NativePreviewView::copyTextRequested, &view, [&](const QString &text) { copied = text; });
        view.copy(); require(copied == content->text, "global copy differs from semantic text");
        for (qreal fraction : {0.0, .5, 1.0, .25, .75, 0.0}) {
            view.verticalScrollBar()->setValue(qRound(view.verticalScrollBar()->maximum() * fraction));
            QApplication::processEvents(); view.grab();
            require(view.selectedText() == content->text, "selection changed during cache eviction");
        }
        const auto stats = view.statistics();
        require(stats.peakCachedDocuments <= 48 && stats.peakCachedEstimatedBytes <= 8 * 1024 * 1024, "layout cache exceeded guards");
        require(QCryptographicHash::hash(source, QCryptographicHash::Sha256) == digest, "source changed");
        result = {{"ok", true}, {"fixture", args[2]}, {"sourceBytes", source.size()}, {"modelEncodedBytes", encoded},
                  {"retainedContentBytesEstimate", content->estimatedBytes()}, {"parseMs", parsed}, {"projectionCompleteMs", projection},
                  {"geometryCompleteMs", plan}, {"firstObservedFrameMs", frame}, {"tourCompleteMs", timer.nsecsElapsed() / 1e6},
                  {"flows", stats.flows}, {"chunks", stats.chunks}, {"layouts", stats.layouts},
                  {"peakCachedDocuments", stats.peakCachedDocuments}, {"peakCachedEstimatedBytes", stats.peakCachedEstimatedBytes},
                  {"sourceUnchanged", true}, {"globalSelectionPreserved", true}, {"sourceSha256", QString::fromLatin1(digest.toHex())},
                  {"sourceFontFamily", QFontInfo(sourceFont()).family()}, {"sourceFontFixedPitch", QFontInfo(sourceFont()).fixedPitch()},
                  {"scope", "offscreen native admission; global geometry pass, lazy short text and bounded long fragments, not final incremental product admission"}};
        auto revoked = stamp; ++revoked.resources; view.expect(revoked);
        require(view.statistics().decodedImageBytes == 0 && view.statistics().cachedDocuments == 0, "revoke retained resource/layout cache");
        result.insert("revokeClearedCache", true);
        QTimer::singleShot(300, &application, &QCoreApplication::quit); application.exec();
    } catch (const std::exception &error) { result = {{"ok", false}, {"error", QString::fromUtf8(error.what())}}; }
    std::puts(QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
    return result.value("ok").toBool() ? 0 : 1;
}
