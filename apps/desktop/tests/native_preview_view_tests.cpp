#include "NativePreviewView.h"
#include "Appearance.h"
#include "InputModality.h"
#include "UiFonts.h"
#include "VisualTokens.h"
#include <QAccessible>
#include <QBuffer>
#include <QCryptographicHash>
#include <QImage>
#include <QFontInfo>
#include <QTextLayout>
#include <QPointer>
#include <QScrollBar>
#include <QSignalSpy>
#include <QDir>
#include <QPushButton>
#include <QWheelEvent>
#include <QtTest>
#include <QVBoxLayout>
#include <algorithm>

using namespace xdnote;
namespace {
std::shared_ptr<const NativePreviewContent> content(const QByteArray &source, const QByteArray &states = {}) {
    auto model = NativePreviewModel::parse(source);
    if (!model) return {};
    auto result = NativePreviewContent::build(std::move(*model), states);
    return result ? std::make_shared<NativePreviewContent>(std::move(*result)) : nullptr;
}
NativePreviewStamp stamp(quint64 request = 1) { return {QUuid("{71803001-4910-4670-a178-edf2b27d12ca}"), 7, request, 2, 3}; }
}
class NativePreviewViewTests final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { installFontFallbacks(); InputModality::forApplication(); qRegisterMetaType<NativePreviewStamp>(); }
    void cachedImagesRemainStableAcrossRepaints_data() { QTest::addColumn<QString>("theme"); QTest::newRow("light") << QString("light"); QTest::newRow("dark") << QString("dark"); }
    void cachedImagesRemainStableAcrossRepaints() {
        QFETCH(QString, theme); Appearance appearance; QVERIFY(appearance.setPreferences(theme, "graphite"));
        const auto data = content("![loaded](one.png)\n\ninline ![badge](missing.svg)\n\n![block](missing.png)\n\n" + QByteArray("paragraph\n\n").repeated(100), QByteArray::fromHex("010505")); QVERIFY(data); QCOMPARE(data->images.size(), 3);
        QImage source(64, 32, QImage::Format_ARGB32); source.fill(Qt::red); QByteArray png; QBuffer buffer(&png); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(source.save(&buffer, "PNG"));
        NativePreviewView view(&appearance); view.resize(640, 420); view.expect(stamp()); QVERIFY(view.presentAsync(stamp(), data, {{0, png}})); view.show(); QTRY_VERIFY(view.ready());
        for (const qreal zoom : {1.0, 1.5}) {
            view.setZoomFactor(zoom); QTRY_VERIFY(view.ready()); view.verticalScrollBar()->setValue(0); QApplication::processEvents();
            const auto before = view.grab().toImage(); const auto bytes = view.statistics().decodedImageBytes; const auto loads = view.statistics().resourceLoads;
            QVERIFY(bytes > 0); QVERIFY(loads > 0);
            bool visuallyStable = true;
            for (int turn = 0; turn < 80; ++turn) {
                view.viewport()->update(); QApplication::processEvents(); const auto image = view.grab().toImage();
                visuallyStable &= image == before;
            }
            const auto evidence = qEnvironmentVariable("XDNOTE_NATIVE_EVIDENCE");
            if (!evidence.isEmpty()) { QVERIFY(QDir().mkpath(evidence)); const auto label = evidence + "/image-repaint-" + theme + "-" + QString::number(zoom); QVERIFY(before.save(label + "-before.png")); QVERIFY(view.grab().save(label + "-after.png")); }
            QCOMPARE(view.statistics().decodedImageBytes, bytes); QCOMPARE(view.statistics().resourceLoads, loads); QCOMPARE(view.statistics().deniedResources, 0); QVERIFY(visuallyStable);
            view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum()); view.grab();
            view.verticalScrollBar()->setValue(0); QApplication::processEvents(); QCOMPARE(view.grab().toImage(), before);
            QVERIFY(view.statistics().peakCachedEstimatedBytes <= 8 * 1024 * 1024);
        }
        view.expect(stamp(2)); QCOMPARE(view.statistics().decodedImageBytes, qsizetype(0));
    }
    void readerStyleEvidence_data() { QTest::addColumn<QString>("theme"); QTest::newRow("light") << QString("light"); QTest::newRow("dark") << QString("dark"); }
    void readerStyleEvidence() {
        QFETCH(QString, theme);
        const auto source = QStringLiteral("# 写作，让思考清晰\n\n一个安静的 **Markdown 阅读空间**，保持文字层级与舒适的节奏。支持 `inline code` 与 [文档链接](guide.md)。\n\n![Downloads](missing-download.svg) ![License MIT](missing-license.svg)\n\n## 列表与引用\n\n- %1\n- [x] 保留源文与撤销，不在阅读模式写入文件。\n\n> 留一点时间，自由写作。\n>\n> 阅读界面保持克制，信息自然呈现。\n\n## 清晰的代码与表格\n\n```cpp\n// Native preview, one source of truth.\nconst QString title = \"写作，从这里开始\";\nreturn document;\n```\n\n| 项目 | 状态 |\n| :--- | :--- |\n| 原生阅读 | 已接入 |\n| 相对链接 | 复用文档会话 |\n| 图片资源 | 保留明确诊断 |\n\n![阅读示意图](missing-preview.png)\n\n### 继续你的记录\n\nGood typography gives long paragraphs a comfortable measure, clear hierarchy, and enough breathing room. 中文与 Latin、数字 123 和 emoji 😀 可以一起阅读。\n").arg(QStringLiteral("这一段用于检查列表自动换行后的悬挂缩进，正文应与第一行文字对齐，而不是退回项目符号的位置。").repeated(5)).toUtf8();
        const auto data = content(source, QByteArray(3, char(5))); QVERIFY(data); QCOMPARE(data->images.size(), 3);
        Appearance appearance; QVERIFY(appearance.setPreferences(theme, "graphite"));
        NativePreviewView view(&appearance); view.resize(840, 1080); view.expect(stamp()); QVERIFY(view.present(stamp(), data)); view.show(); QApplication::processEvents(); view.grab();
        QCOMPARE(view.admittedImageCount(), 0); const auto before = data->text;
        const auto firstImage = view.characterRect(data->images[0].start); QVERIFY(firstImage.width() <= 241); QVERIFY(firstImage.height() < 80);
        const auto flow = std::find_if(data->flows.cbegin(), data->flows.cend(), [](const auto &value) { return value.markerLength && value.length > 150; }); QVERIFY(flow != data->flows.cend());
        view.scrollToText(flow->start); const auto first = view.characterRect(flow->start + flow->markerLength);
        bool wrapped = false;
        for (int i = flow->markerLength + 1; i < flow->length; ++i) {
            const auto next = view.characterRect(flow->start + i);
            if (next.y() > first.y() + 2) { QVERIFY(qAbs(next.x() - first.x()) <= 2); wrapped = true; break; }
        }
        QVERIFY(wrapped); view.verticalScrollBar()->setValue(0); view.grab();
        const auto evidence = qEnvironmentVariable("XDNOTE_NATIVE_EVIDENCE");
        if (!evidence.isEmpty()) {
            QVERIFY(QDir().mkpath(evidence)); QVERIFY(view.grab().save(evidence + "/reader-" + theme + "-top.png"));
            view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum()); QVERIFY(view.grab().save(evidence + "/reader-" + theme + "-details.png"));
        }
        QSignalSpy copy(&view, &NativePreviewView::copyTextRequested); view.selectAll(); view.copy(); QCOMPARE(copy.size(), 1); QCOMPARE(copy.first().first().toString(), before);
        QCOMPARE(data->text, before); QCOMPARE(view.statistics().deniedResources, 0); QVERIFY(view.statistics().peakCachedEstimatedBytes <= 8 * 1024 * 1024);
    }
    void sourceFontIsMonospacedAndEmojiHasGlyphFallback() {
        QVERIFY2(QFontInfo(sourceFont()).fixedPitch(), qPrintable(QFontInfo(sourceFont()).family()));
        const auto emoji = QString::fromUcs4(U"😀");
        QTextLayout layout(emoji, uiFont(16)); layout.beginLayout(); layout.createLine().setLineWidth(100); layout.endLayout();
        int glyphs = 0;
        for (const auto &run : layout.glyphRuns()) for (const auto index : run.glyphIndexes()) { QVERIFY(index != 0); ++glyphs; }
        QVERIFY(glyphs > 0);
    }
    void projectsGlobalSemanticsAndDraws() {
        const auto source = QStringLiteral("# 中文标题\n\n正文 **粗体** *斜体* ~~删除~~ `code` 😀。\n\n> - [x] 完成\n>   - 子项\n\n```cpp\nconst char *text = \"<script>literal</script>\";\n```\n\n|左|右|\n|:--|--:|\n|甲|乙|\n\n[跳转](#中文标题)\n").toUtf8();
        const auto data = content(source); QVERIFY(data);
        QVERIFY(data->text.contains(QStringLiteral("正文 粗体 斜体 删除 code 😀")));
        QVERIFY(data->text.contains(QStringLiteral("☑ 完成"))); QVERIFY(data->text.contains(QStringLiteral("• 子项")));
        QCOMPARE(data->owners.size(), 2); QCOMPARE(data->cells.size(), 4);
        Appearance appearance(nullptr, ""); appearance.setPreferences("light", "graphite");
        NativePreviewView view(&appearance); view.resize(800, 600); view.expect(stamp());
        QVERIFY2(view.present(stamp(), data), qPrintable(view.error()));
        view.show(); QApplication::processEvents();
        const auto image = view.grab().toImage(); QVERIFY(!image.isNull());
        QVERIFY(view.statistics().cachedDocuments > 0); QVERIFY(view.statistics().flows > 0);
        QSignalSpy copied(&view, &NativePreviewView::copyTextRequested);
        view.selectAll(); view.copy(); QCOMPARE(copied.size(), 1); QCOMPARE(copied.first().first().toString(), data->text);
        QVERIFY(view.navigateToHeading(QStringLiteral("中文标题"), stamp()));
    }
    void projectionKeepsTightLooseAndNestedListOrder() {
        const auto data = content(QStringLiteral("1. **一**\n2. 二\n   - 子项\n\n> 引用\n\n- [ ] 待办\n\n- 段落一\n\n  段落二\n\n    ```\n    A\u2028B\u2029C\n    ```\n").toUtf8());
        QVERIFY(data); QVERIFY(data->text.startsWith(QStringLiteral("1. 一\n\n2. 二")));
        QVERIFY(data->text.contains(QStringLiteral("☐ 待办"))); QVERIFY(data->text.contains(QStringLiteral("• 子项")));
        int prior = -1;
        for (const auto &flow : data->flows) { QVERIFY(flow.start >= prior); QVERIFY(flow.length >= 0); prior = flow.start; }
        const auto literal = QStringLiteral("```\nA\u2028B\u2029C\n```\n").toUtf8();
        const auto code = content(literal); QVERIFY(code); QCOMPARE(code->flows.size(), 3);
        QCOMPARE(code->text.mid(code->owners.first().start, code->owners.first().length), QStringLiteral("A\u2028B\u2029C\n"));
    }
    void longParagraphScrollSelectionResizeAndEviction() {
        QByteArray source;
        const auto unit = QStringLiteral("中文 abc fi 😀 é 文字和空格。 ").toUtf8();
        while (source.size() < 128 * 1024) source += unit;
        source += " END_SENTINEL";
        const auto data = content(source); QVERIFY(data); QCOMPARE(data->flows.size(), 1);
        Appearance appearance; NativePreviewView view(&appearance); view.resize(800, 520); view.expect(stamp());
        QVERIFY2(view.present(stamp(), data), qPrintable(view.error())); view.show(); QApplication::processEvents();
        QVERIFY(view.statistics().chunks > 16); QVERIFY(view.verticalScrollBar()->maximum() > 10000);
        view.setSelection(2, int(data->text.size()) - 2); const auto selected = view.selectedText();
        for (int step : {0, 2, 5, 9, 1, 7, 3, 8, 0}) {
            const int offset = qMin(int(data->text.size()) - 10, step * (int(data->text.size()) / 10));
            view.scrollToText(offset); QApplication::processEvents(); view.grab();
            QCOMPARE(view.selectedText(), selected);
        }
        const auto stats = view.statistics(); QVERIFY(stats.peakCachedDocuments <= 48); QVERIFY(stats.peakCachedEstimatedBytes <= 8 * 1024 * 1024);
        const int middle = int(data->text.size()) / 2;
        const int safeMiddle = data->text[middle].isLowSurrogate() ? middle - 1 : middle;
        view.scrollToText(safeMiddle); const int prior = view.verticalScrollBar()->value(); QVERIFY(prior > 1000);
        view.resize(640, 430); QApplication::processEvents(); QVERIFY(view.verticalScrollBar()->value() > 1000);
        const auto rect = view.characterRect(safeMiddle); const int relative = rect.y() - view.viewport()->mapToGlobal(QPoint()).y();
        QVERIFY2(qAbs(relative) < 100, qPrintable(QString::number(relative)));
        QSignalSpy copied(&view, &NativePreviewView::copyTextRequested); view.copy(); QCOMPARE(copied.first().first().toString(), selected);
    }
    void oversizedCodeAndWideTableKeepIndependentHorizontalScrolling() {
        const auto source = QByteArray("```text\n") + QByteArray(16000, 'x') + "\n```\n\n|a|b|\n|--|--|\n|" + QByteArray(5000, 'y') + "|z|\n";
        const auto data = content(source); QVERIFY(data);
        Appearance appearance; NativePreviewView view(&appearance); view.resize(720, 500); view.expect(stamp());
        QVERIFY2(view.present(stamp(), data), qPrintable(view.error())); view.show(); QApplication::processEvents(); view.grab();
        QList<QScrollBar *> horizontal;
        for (auto *bar : view.findChildren<QScrollBar *>()) if (bar->orientation() == Qt::Horizontal && !bar->isHidden() && bar->maximum() > 0) horizontal.append(bar);
        QCOMPARE(horizontal.size(), 2); const auto firstBefore = horizontal.last()->value();
        horizontal.first()->setValue(20000); QCOMPARE(horizontal.last()->value(), firstBefore);
        view.grab(); QVERIFY(view.statistics().cachedDocuments < 20);
        QSignalSpy copied(&view, &NativePreviewView::copyTextRequested);
        for (int index = 0; index < data->actions.size(); ++index) if (data->actions[index].copyCode) QVERIFY(view.activateAction(index));
        QCOMPARE(copied.size(), 1); QCOMPARE(copied.first().first().toString(), QString(16000, 'x') + '\n');
    }
    void mouseLinksRequireTextHitAndSelectionDragDoesNotNavigate() {
        const auto data = content("[Alpha link](nested/doc.md#heading)\n\n[remote](https://example.invalid)\n"); QVERIFY(data);
        Appearance appearance; NativePreviewView view(&appearance); view.resize(700, 450); view.expect(stamp()); QVERIFY(view.present(stamp(), data, {}, true)); view.show(); QApplication::processEvents();
        QSignalSpy opened(&view, &NativePreviewView::documentLinkRequested);
        const auto rect = view.characterRect(2); const auto point = view.viewport()->mapFromGlobal(rect.center());
        QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, QPoint(0, point.y())); QCOMPARE(opened.size(), 0);
        QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, point); QCOMPARE(opened.size(), 1);
        QCOMPARE(opened.first().first().toString(), QString("nested/doc.md#heading"));
        QTest::mousePress(view.viewport(), Qt::LeftButton, {}, point);
        const auto destination = view.viewport()->mapFromGlobal(view.characterRect(8).center());
        QMouseEvent move(QEvent::MouseMove, destination, view.viewport()->mapToGlobal(destination), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view.viewport(), &move); QTest::mouseRelease(view.viewport(), Qt::LeftButton, {}, destination);
        QCOMPARE(opened.size(), 1); QVERIFY(!view.selectedText().isEmpty());
        QVERIFY(!view.activateAction(1)); QCOMPARE(opened.size(), 1);
    }
    void staleDocumentRevisionPathAndResourcesAreRejected() {
        const auto data = content("# title\n\n[open](a.md)\n"); QVERIFY(data);
        Appearance appearance; NativePreviewView view(&appearance); view.expect(stamp()); QVERIFY(view.present(stamp(), data));
        for (int field = 0; field < 5; ++field) {
            auto different = stamp();
            if (field == 0) different.document = QUuid::createUuid();
            if (field == 1) ++different.revision; if (field == 2) ++different.request;
            if (field == 3) ++different.path; if (field == 4) ++different.resources;
            QVERIFY(!view.present(different, data)); QVERIFY(!view.navigateToHeading("title", different));
        }
        view.expect(stamp(2)); QVERIFY(!view.content()); QVERIFY(!view.activateAction(0)); QVERIFY(!view.present(stamp(), data));
        QVERIFY(view.present(stamp(2), data));
    }
    void keyboardActionsClearOutlineAndLocalLinksRequireAdmission() {
        const auto data = content(QByteArray("[open](a.md)\n\n") + QByteArray("para\n\n").repeated(100) + "```\ncode\n```\n" + QByteArray("after\n\n").repeated(100)); QVERIFY(data);
        Appearance appearance; NativePreviewView view(&appearance); view.resize(600, 400); view.expect(stamp()); QVERIFY(view.present(stamp(), data));
        QSignalSpy opened(&view, &NativePreviewView::documentLinkRequested); QVERIFY(!view.activateAction(0)); QCOMPARE(opened.size(), 0);
        view.expect(stamp(2)); QVERIFY(view.present(stamp(2), data, {}, true)); view.show(); QApplication::setActiveWindow(&view); view.setFocus(); QApplication::processEvents();
        InputModality::forApplication()->clear(); QVERIFY(!view.keyboardOutlineVisible());
        QTest::keyClick(&view, Qt::Key_Tab); QVERIFY(view.keyboardOutlineVisible());
        QTest::keyClick(&view, Qt::Key_Return); QCOMPARE(opened.size(), 1); QVERIFY(!view.keyboardOutlineVisible());
        QTest::keyClick(&view, Qt::Key_Tab); QVERIFY(view.keyboardOutlineVisible());
        bool visibleCopy = false;
        for (auto *button : view.findChildren<QPushButton *>()) if (!button->isHidden() && button->isEnabled() && button->accessibleName() == QStringLiteral("复制代码")) visibleCopy = view.viewport()->rect().intersects(button->geometry());
        QVERIFY(visibleCopy);
        QTest::keyClick(&view, Qt::Key_Escape); QVERIFY(!view.keyboardOutlineVisible());
    }
    void pointerReleaseCannotActivateAReplacementSnapshot() {
        const auto data = content("[first](first.md)\n"); QVERIFY(data);
        Appearance appearance; NativePreviewView view(&appearance); view.resize(600, 300); view.expect(stamp()); QVERIFY(view.present(stamp(), data, {}, true)); view.show(); QApplication::processEvents();
        const auto point = view.viewport()->mapFromGlobal(view.characterRect(1).center());
        QSignalSpy opened(&view, &NativePreviewView::documentLinkRequested); QTest::mousePress(view.viewport(), Qt::LeftButton, {}, point);
        view.expect(stamp(2)); QVERIFY(view.present(stamp(2), content("[second](second.md)\n"), {}, true));
        QTest::mouseRelease(view.viewport(), Qt::LeftButton, {}, point); QCOMPARE(opened.size(), 0);
    }
    void nativeImagesUseOnlyAdmittedMemoryAndRevokeImmediately() {
        const auto data = content("before ![red image](private.png) after\n", QByteArray(1, char(1))); QVERIFY(data);
        QImage original(80, 40, QImage::Format_ARGB32_Premultiplied); original.fill(QColor("#e32929"));
        QByteArray png; QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly); QVERIFY(original.save(&buffer, "PNG"));
        Appearance appearance; NativePreviewView view(&appearance); view.resize(700, 400); view.expect(stamp());
        QVERIFY(view.present(stamp(), data, {{0, png}})); view.show(); QApplication::processEvents();
        const auto pixels = view.grab().toImage(); int red = 0;
        for (int y = 0; y < pixels.height(); ++y) for (int x = 0; x < pixels.width(); ++x)
            if (pixels.pixelColor(x, y).red() > 180 && pixels.pixelColor(x, y).green() < 80) ++red;
        QVERIFY(red > 1000); QVERIFY(view.statistics().resourceLoads > 0); QVERIFY(view.statistics().decodedImageBytes > 0);
        auto changed = stamp(); ++changed.resources; view.expect(changed); QCOMPARE(view.statistics().decodedImageBytes, qsizetype(0));
        QVERIFY(!view.present(stamp(), data, {{0, png}}));
        const auto blocked = content("![blocked](https://example.invalid/picture.png)\n"); QVERIFY(blocked);
        QVERIFY(view.present(changed, blocked, {{0, png}})); view.grab(); QCOMPARE(view.statistics().resourceLoads, 0);
        QVERIFY(view.content()->text.contains("blocked"));
    }
    void unavailableImagesKeepReadableReasonsAndSourceIdentity() {
        const QByteArray source("![alt](private.png)\n");
        for (char state : {char(2), char(3), char(4), char(5)}) {
            const auto data = content(source, QByteArray(1, state)); QVERIFY(data);
            QVERIFY(data->text.contains(QStringLiteral("图片（"))); QVERIFY(data->text.contains("alt"));
            QCOMPARE(data->model.sourceDigest(), QCryptographicHash::hash(source, QCryptographicHash::Sha256));
        }
    }
    void accessibleTextIsReadOnlyAndSurvivesLayoutEviction() {
        const auto data = content(QStringLiteral("# 文本\n\n中文😀正文\n\n").repeated(2000).toUtf8()); QVERIFY(data);
        Appearance appearance; NativePreviewView view(&appearance); view.resize(700, 400); view.expect(stamp()); QVERIFY(view.present(stamp(), data)); view.show(); QApplication::processEvents();
        auto *interface = QAccessible::queryAccessibleInterface(&view); QVERIFY(interface); QVERIFY(interface->state().readOnly); QVERIFY(!interface->editableTextInterface());
        auto *text = interface->textInterface(); QVERIFY(text); QCOMPARE(text->characterCount(), int(data->text.size())); QCOMPARE(text->text(0, -1), data->text);
        interface->setText(QAccessible::Value, "forbidden"); QCOMPARE(text->text(0, -1), data->text);
        text->setSelection(0, 0, int(data->text.size())); QCOMPARE(view.selectedText(), data->text);
        text->scrollToSubstring(int(data->text.size()) - 20, int(data->text.size()) - 10); view.grab();
        QCOMPARE(text->selectionCount(), 1); int start = -1, end = -1; text->selection(0, &start, &end); QCOMPARE(start, 0); QCOMPARE(end, int(data->text.size()));
        text->setCursorPosition(0); QCOMPARE(text->selectionCount(), 0);
    }
    void staleCodeButtonsCannotCopyANewSnapshotAndReentrantCloseIsSafe() {
        const auto data = content("```\nfirst\n```\n"); QVERIFY(data);
        Appearance appearance; NativePreviewView view(&appearance); view.resize(700, 400); view.expect(stamp()); QVERIFY(view.present(stamp(), data)); view.show(); QApplication::processEvents();
        auto buttons = view.findChildren<QPushButton *>(); QVERIFY(!buttons.isEmpty()); QPointer<QPushButton> old = buttons.first();
        view.expect(stamp(2)); QVERIFY(view.present(stamp(2), content("```\nsecond\n```\n")));
        QSignalSpy copied(&view, &NativePreviewView::copyTextRequested); if (old) old->click(); QCOMPARE(copied.size(), 0);
        auto currentButtons = view.findChildren<QPushButton *>(); QPushButton *active = nullptr;
        for (auto *button : currentButtons) if (!button->isHidden() && button->isEnabled()) active = button;
        QVERIFY(active);
        connect(&view, &NativePreviewView::copyTextRequested, &view, [&view] { view.expect(stamp(3)); });
        active->click(); QCOMPARE(copied.size(), 1); QCOMPARE(copied.first().first().toString(), QString("second\n")); QVERIFY(!view.content());
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void synchronousCallbacksCanCloseOrReplacePreviewSafely() {
        Appearance appearance; const auto data = content("[open](old.md)\n"); QVERIFY(data);
        QPointer<NativePreviewView> selected = new NativePreviewView(&appearance);
        selected->expect(stamp()); QVERIFY(selected->present(stamp(), data));
        connect(selected, &NativePreviewView::selectionChanged, selected, [selected] { delete selected.data(); });
        selected->setSelection(0, 2); QVERIFY(!selected);
        QPointer<NativePreviewView> mouseSelected = new NativePreviewView(&appearance);
        mouseSelected->resize(600, 300); mouseSelected->expect(stamp()); QVERIFY(mouseSelected->present(stamp(), data)); mouseSelected->show(); QApplication::processEvents();
        const auto point = mouseSelected->viewport()->mapFromGlobal(mouseSelected->characterRect(2).center());
        connect(mouseSelected, &NativePreviewView::selectionChanged, mouseSelected, [mouseSelected] { delete mouseSelected.data(); });
        QMouseEvent press(QEvent::MouseButtonPress, point, point, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(mouseSelected->viewport(), &press); QVERIFY(!mouseSelected);
        QPointer<NativePreviewView> failed = new NativePreviewView(&appearance);
        failed->expect(stamp());
        connect(failed, &NativePreviewView::layoutFailed, failed, [failed] { delete failed.data(); });
        failed->showError(stamp(), QStringLiteral("测试错误")); QVERIFY(!failed);
        NativePreviewView view(&appearance); view.expect(stamp()); QVERIFY(view.present(stamp(), data, {}, true));
        connect(&view, &NativePreviewView::documentLinkRequested, &view, [&view] { view.expect(stamp(2)); });
        QSignalSpy opened(&view, &NativePreviewView::documentLinkRequested);
        QVERIFY(view.activateAction(0)); QCOMPARE(opened.size(), 1);
        QVERIFY(opened.first()[1].value<NativePreviewStamp>() == stamp()); QVERIFY(!view.content());
    }
    void fragmentNavigationUsesGlobalIdsAndIncompleteReferencesStayInert() {
        const auto data = content("[go](#title-1)\n\n# title\n\n# title\n"); QVERIFY(data);
        Appearance appearance; NativePreviewView view(&appearance); view.resize(700, 300); view.expect(stamp()); QVERIFY(view.present(stamp(), data));
        QSignalSpy moved(&view, &NativePreviewView::fragmentNavigated); QVERIFY(view.activateAction(0)); QCOMPARE(moved.size(), 1); QCOMPARE(moved.first().first().toString(), QString("title-1"));
        const auto guarded = content(QByteArray("[x] ").repeated(1000) + "\n\n[x]: " + QByteArray(2000, 'a') + "\n"); QVERIFY(guarded); QVERIFY(!guarded->model.referencesComplete());
        view.expect(stamp(2)); QVERIFY(view.present(stamp(2), guarded)); QVERIFY(!view.activateAction(0));
    }
    void longBidiAndHugeGraphemeKeepParagraphAndSelection() {
        Appearance appearance; NativePreviewView view(&appearance); view.resize(700, 400);
        const auto bidi = content(QStringLiteral("العربية abc 123 ").repeated(1000).toUtf8()); QVERIFY(bidi);
        view.expect(stamp()); QVERIFY(view.present(stamp(), bidi)); QVERIFY(view.error().isEmpty());
        view.selectAll(); QCOMPARE(view.selectedText(), bidi->text);
        view.scrollToText(12000); QVERIFY(view.readingPosition().y() > 0);
        const auto cluster = content((QString("a") + QString(20000, QChar(0x0301))).toUtf8()); QVERIFY(cluster);
        view.expect(stamp(2)); QVERIFY(view.present(stamp(2), cluster)); QVERIFY(view.error().isEmpty());
        view.selectAll(); QCOMPARE(view.selectedText(), cluster->text);
    }
    void backgroundLayoutRemainsResponsiveAndRejectsSupersededPlans() {
        Appearance appearance; NativePreviewView view(&appearance); view.resize(700, 400); view.show(); QApplication::processEvents();
        const auto large = content(QByteArray("Long paragraph text 中文 ").repeated(40000)); QVERIFY(large);
        QSignalSpy ready(&view, &NativePreviewView::layoutReady);
        view.expect(stamp()); QVERIFY(view.presentAsync(stamp(), large));
        int ticks = 0; QTimer timer; timer.setInterval(1); connect(&timer, &QTimer::timeout, &view, [&] { ++ticks; }); timer.start();
        QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 30000); QVERIFY(ticks > 3);
        ready.clear(); view.expect(stamp(2)); QVERIFY(view.presentAsync(stamp(2), large));
        view.expect(stamp(3)); const auto latest = content("# latest\n"); QVERIFY(view.presentAsync(stamp(3), latest));
        QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 30000); QCOMPARE(ready.size(), 1);
        QVERIFY(ready.first().first().value<NativePreviewStamp>() == stamp(3)); QCOMPARE(view.content()->text, latest->text);
        auto *closed = new NativePreviewView(&appearance); closed->resize(700, 400); closed->show(); QApplication::processEvents(); closed->expect(stamp()); QVERIFY(closed->presentAsync(stamp(), large)); delete closed;
    }
    void hiddenPreviewDefersGeometryAndShowsOnlyTheLatestSnapshot() {
        Appearance appearance; NativePreviewView view(&appearance); view.resize(700, 400);
        const auto large = content(QByteArray("long hidden paragraph ").repeated(40000)); QVERIFY(large);
        QSignalSpy ready(&view, &NativePreviewView::layoutReady);
        view.expect(stamp()); QVERIFY(view.presentAsync(stamp(), large)); QTest::qWait(20);
        QVERIFY(!view.ready()); QVERIFY(ready.isEmpty()); QCOMPARE(view.statistics().layouts, 0);
        view.expect(stamp(2)); const auto latest = content("# latest\n"); QVERIFY(view.presentAsync(stamp(2), latest));
        view.show(); QTRY_VERIFY_WITH_TIMEOUT(view.ready(), 15000); QVERIFY(!ready.isEmpty());
        QVERIFY(view.displayedStamp() == stamp(2)); QCOMPARE(view.content()->text, latest->text);
        view.hide(); QVERIFY(!view.ready()); view.show(); QTRY_VERIFY_WITH_TIMEOUT(view.ready(), 15000);
        QCOMPARE(view.content()->text, latest->text);
        QWidget host; QVBoxLayout layout(&host); NativePreviewView nested(&appearance, &host); layout.addWidget(&nested);
        host.resize(700, 400); nested.expect(stamp()); QVERIFY(nested.presentAsync(stamp(), latest));
        host.show(); QTRY_VERIFY_WITH_TIMEOUT(nested.ready(), 15000);
        host.hide(); host.show(); QTRY_VERIFY_WITH_TIMEOUT(nested.ready(), 15000);
        QCOMPARE(nested.content()->text, latest->text);
    }
    void virtualAccessibleActionsAndTabStopsKeepGlobalPositions() {
        Appearance appearance; NativePreviewView view(&appearance); view.resize(700, 500);
        const auto data = content("[open](other.md)\n\n```cpp\nconst\tvalue = 1; /* multi\nline */\n```\n"); QVERIFY(data);
        view.expect(stamp()); QVERIFY(view.present(stamp(), data, {}, true)); view.show(); QApplication::processEvents();
        auto *accessible = QAccessible::queryAccessibleInterface(&view); QVERIFY(accessible);
        QCOMPARE(accessible->childCount(), 2); auto *link = accessible->child(0); QVERIFY(link && link->isValid());
        QCOMPARE(link->role(), QAccessible::Link); QCOMPARE(link->text(QAccessible::Name), QString("open"));
        QSignalSpy opened(&view, &NativePreviewView::documentLinkRequested), copied(&view, &NativePreviewView::copyTextRequested);
        link->actionInterface()->doAction(QAccessibleActionInterface::pressAction()); QCOMPARE(opened.size(), 1);
        auto *copy = accessible->child(1); QVERIFY(copy && copy->actionInterface()); copy->actionInterface()->doAction(QAccessibleActionInterface::pressAction());
        QCOMPARE(copied.size(), 1); QVERIFY(copied.first().first().toString().contains('\t'));
        const int from = data->text.indexOf("const"), to = data->text.indexOf("value");
        const auto advance = view.characterRect(to).left() - view.characterRect(from).left();
        QVERIFY(qAbs(advance - QFontMetricsF(sourceFont()).horizontalAdvance(' ') * 8) < 3);
        QVERIFY(!data->highlights.isEmpty()); view.expect(stamp(2)); QVERIFY(!link->isValid()); QVERIFY(!copy->isValid());
    }
    void lightAndDarkUseSharedTokensAndSaveEvidence() {
        const auto data = content(QStringLiteral("# 一个安静的写作空间\n\n把想法写下来，让思考慢慢清晰。**中文粗体**与 *italic*。 😀\n\n- [x] 完成一项任务\n- [ ] 保留一点时间\n\n> 少一点干扰，多一点专注。\n\n```cpp\nconst auto title = \"写作\";\n```\n\n|项目|状态|\n|:--|--:|\n|原生预览|进行中|\n\n[打开文档](notes/start.md)\n").toUtf8()); QVERIFY(data);
        const auto directory = qEnvironmentVariable("XDNOTE_NATIVE_EVIDENCE");
        for (const auto theme : {"light", "dark"}) {
            Appearance appearance; QVERIFY(appearance.setPreferences(theme, "graphite"));
            NativePreviewView view(&appearance); view.resize(880, 760); view.expect(stamp()); QVERIFY(view.present(stamp(), data)); view.show(); QApplication::processEvents();
            const auto image = view.grab().toImage(); QCOMPARE(image.pixelColor(4, 4), appearance.color("background"));
            const auto taskPosition = int(data->text.indexOf(QChar(0x2611))); QVERIFY(taskPosition >= 0);
            const auto taskRect = view.characterRect(taskPosition); QVERIFY(taskRect.width() >= 15);
            const auto row = std::find_if(data->flows.cbegin(), data->flows.cend(), [](const NativePreviewContent::Flow &flow) {
                return flow.kind == NativePreviewContent::FlowKind::TableRow && !flow.header;
            });
            QVERIFY(row != data->flows.cend());
            const auto cell = data->cells[row->firstCell];
            auto textRect = view.characterRect(cell.start); textRect.translate(-view.viewport()->mapToGlobal(QPoint()));
            const int maximumWidth = visualTokens()["typography"].toObject()["reader"].toObject()["maxContentWidth"].toInt();
            const int tableWidth = qMin(maximumWidth, view.viewport()->width() - 48);
            const qreal tableLeft = (view.viewport()->width() - tableWidth) / 2.0;
            const qreal ratio = image.devicePixelRatio(); bool bottomBorder = false;
            for (int y = textRect.top() + 1; y < qMin(view.viewport()->height(), textRect.top() + 60); ++y) {
                int border = 0;
                for (int x = tableLeft; x < tableLeft + tableWidth; ++x)
                    if (image.pixelColor(qFloor(x * ratio), qFloor(y * ratio)) == appearance.color("border")) ++border;
                if (border > tableWidth * .95) { bottomBorder = true; break; }
            }
            QVERIFY(bottomBorder);
            // An odd viewport width puts the logical inset on a half pixel.
            const int right = qFloor((tableLeft + tableWidth - .5) * ratio), centerY = qFloor(textRect.center().y() * ratio);
            bool rightBorder = false;
            for (int x = qMax(0, right - 1); x <= qMin(image.width() - 1, right + 1); ++x) rightBorder |= image.pixelColor(x, centerY) == appearance.color("border");
            QVERIFY(rightBorder);
            if (!directory.isEmpty()) { QVERIFY(QDir().mkpath(directory)); QVERIFY(image.save(directory + "/native-preview-" + theme + ".png")); }
        }
    }
};
QTEST_MAIN(NativePreviewViewTests)
#include "native_preview_view_tests.moc"
