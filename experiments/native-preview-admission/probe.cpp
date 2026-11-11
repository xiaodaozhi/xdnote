#include "NativePreviewView.h"
#include "Appearance.h"
#include "UiFonts.h"
#include "../../apps/desktop/tests/preview_fixtures.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFontInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScrollBar>
#include <QTimer>
#include <QThread>
#include <cstdio>
#include <stdexcept>

using namespace xdnote;
namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }

}
int main(int argc, char **argv) {
    QApplication application(argc, argv); installFontFallbacks(); application.setQuitOnLastWindowClosed(false);
    QJsonObject result;
    try {
        const auto args = application.arguments(); require(args.size() == 4, "expected bytes, fixture, screenshot");
        bool valid = false; const int bytes = args[1].toInt(&valid);
        require(valid && bytes >= 10240 && bytes <= 2 * 1024 * 1024, "fixture size outside existing source scope");
        const auto source = previewFixtures::fixture(bytes, args[2]);
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
        int ticks = 0; QTimer heartbeat; heartbeat.setInterval(1);
        QObject::connect(&heartbeat, &QTimer::timeout, &view, [&] { ++ticks; }); heartbeat.start();
        require(view.presentAsync(stamp, content), qPrintable(view.error()));
        const auto settle = [&] {
            while (view.error().isEmpty() && timer.elapsed() < 60000) {
                QApplication::processEvents(); QThread::msleep(1);
                if (!view.ready()) continue;
                QApplication::processEvents(); if (view.ready()) break;
            }
            require(view.ready(), "background layout did not finish");
        };
        settle();
        const qreal plan = timer.nsecsElapsed() / 1e6;
        QApplication::processEvents(); settle(); const auto image = view.grab().toImage();
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
                  {"backgroundHeartbeatTicks", ticks}, {"flows", stats.flows}, {"chunks", stats.chunks}, {"layouts", stats.layouts},
                  {"peakCachedDocuments", stats.peakCachedDocuments}, {"peakCachedEstimatedBytes", stats.peakCachedEstimatedBytes},
                  {"sourceUnchanged", true}, {"globalSelectionPreserved", true}, {"sourceSha256", QString::fromLatin1(digest.toHex())},
                  {"sourceFontFamily", QFontInfo(sourceFont()).family()}, {"sourceFontFixedPitch", QFontInfo(sourceFont()).fixedPitch()},
                  {"scope", "offscreen native admission; background geometry, lazy text; component evidence separate from product process"}};
        auto revoked = stamp; ++revoked.resources; view.expect(revoked);
        require(view.statistics().decodedImageBytes == 0 && view.statistics().cachedDocuments == 0, "revoke retained resource/layout cache");
        result.insert("revokeClearedCache", true);
        QTimer::singleShot(300, &application, &QCoreApplication::quit); application.exec();
    } catch (const std::exception &error) { result = {{"ok", false}, {"error", QString::fromUtf8(error.what())}}; }
    std::puts(QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
    return result.value("ok").toBool() ? 0 : 1;
}
