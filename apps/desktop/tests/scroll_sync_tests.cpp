#include "FileApplication.h"
#include "WorkspaceWindow.h"
#include "SingleFileWindow.h"
#include "SourceEditor.h"
#include "MarkdownPreview.h"
#include "Appearance.h"
#include "GuiRuntime.h"
#include "file_fixtures.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QPointer>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSplitter>
#include <QTemporaryDir>
#include <QTest>
#include <QInputMethodEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <cmath>

using namespace xdnote;
namespace {
QByteArray sample() {
    QByteArray text = QStringLiteral("# 开篇 😀\n\n这里是第一段。\n\n").toUtf8() + QByteArray("\n").repeated(70);
    text += QStringLiteral("## 锚点 B\n\n- 列表项一\n- 列表项二\n\n> 引用段落\n\n```cpp\n").toUtf8();
    for (int i = 0; i < 60; ++i) text += "int value_" + QByteArray::number(i) + " = " + QByteArray::number(i) + ";\n";
    text += QStringLiteral("```\n\n## 锚点 C\n\n").toUtf8();
    text += QStringLiteral("长段落 中文😀 **强调** 与 [链接](#开篇) ").repeated(180).toUtf8();
    text += QStringLiteral("\n\n| 标题 | 内容 |\n| --- | --- |\n| 第一行 | 一个值 |\n| 第二行 | 另一个值 |\n\n## 锚点 D\n\n![占位图片](missing.png)\n\n").toUtf8();
    for (int i = 0; i < 100; ++i) text += QStringLiteral("尾段 %1：保留充足的滚动空间，检查段落与源文位置。\n\n").arg(i).toUtf8();
    return text;
}
std::shared_ptr<const NativePreviewContent> derived(const QByteArray &source) {
    auto model = NativePreviewModel::parse(source); if (!model) return {};
    auto content = NativePreviewContent::build(std::move(*model));
    return content ? std::make_shared<NativePreviewContent>(std::move(*content)) : nullptr;
}
const NativePreviewStamp identity{QUuid("{12638533-71c1-4358-999a-2c7dc38ca006}"), 7, 2, 3, 4};
int byteAt(const QByteArray &source, const QString &marker) { return int(source.indexOf(marker.toUtf8())); }
sptr_t visualLine(SourceEditor *editor, int byte) { return editor->send(SCI_VISIBLEFROMDOCLINE, uptr_t(editor->send(SCI_LINEFROMPOSITION, uptr_t(byte)))); }
void sourceScroll(SourceEditor *editor, sptr_t line) { auto *bar = editor->verticalScrollBar(); bar->setSliderPosition(qBound(0, int(line), bar->maximum())); bar->triggerAction(QAbstractSlider::SliderMove); }
void readerScroll(NativePreviewView *view, int pixel) { auto *bar = view->verticalScrollBar(); bar->setSliderPosition(qBound(0, pixel, bar->maximum())); bar->triggerAction(QAbstractSlider::SliderMove); }
// Frame/layout and wrap completion are asynchronous, especially on slower
// runners. Require a quiet, current mapping instead of sleeping a fixed time
// and accidentally interacting with the next transient layout.
bool waitForMapping(SingleFileWindow *pane) {
    auto *preview = pane->preview(); auto *view = preview->view(); auto *source = pane->editor();
    QElapsedTimer elapsed; elapsed.start(); qint64 changedAt = 0; QObject watcher;
    const auto changed = [&] { changedAt = elapsed.elapsed(); };
    QObject::connect(view, &NativePreviewView::layoutStarted, &watcher, changed);
    QObject::connect(view, &NativePreviewView::layoutReady, &watcher, changed);
    for (auto *bar : {source->verticalScrollBar(), view->verticalScrollBar()}) {
        QObject::connect(bar, &QScrollBar::rangeChanged, &watcher, changed);
        QObject::connect(bar, &QScrollBar::valueChanged, &watcher, changed);
    }
    auto sourceSize = source->viewport()->size(), previewSize = view->viewport()->size();
    while (elapsed.elapsed() < 10000) {
        if (!preview->mappingCurrent() || sourceSize != source->viewport()->size() || previewSize != view->viewport()->size()) changed();
        sourceSize = source->viewport()->size(); previewSize = view->viewport()->size();
        if (preview->mappingCurrent() && elapsed.elapsed() - changedAt >= 100) return true;
        QTest::qWait(10);
    }
    return false;
}
struct Fixture {
    QTemporaryDir directory;
    // Window geometry is persisted after a debounce. Keep application settings
    // outside the image-watched workspace, as in a normal installation.
    QTemporaryDir configuration;
    QByteArray source = sample();
    QString path = directory.filePath("scroll.md");
    FileApplication app{nullptr, configuration.filePath("settings.json")};
    WorkspaceWindow *workspace = nullptr;
    bool written = false;
    Fixture() { written = fileFixtures::write(path, source); workspace = app.workspaceWindow(directory.path()); app.openInWorkspace(path, workspace); }
    SingleFileWindow *pane() const { auto *pane = workspace->activePane(); return pane && pane->document() ? pane : nullptr; }
};
}
class ScrollSyncTests final : public QObject {
    Q_OBJECT
private slots:
    void semanticSpansUseUtf8AndRejectOldLayouts() {
        Appearance appearance; NativePreviewView view(&appearance); view.resize(620, 420); view.show();
        const auto source = sample(); const auto content = derived(source); QVERIFY(content); view.expect(identity); QVERIFY(view.presentAsync(identity, content)); QTRY_VERIFY(view.ready());
        for (const auto &marker : {QStringLiteral("## 锚点 B"), QStringLiteral("## 锚点 C"), QStringLiteral("## 锚点 D")}) {
            const auto byte = byteAt(source, marker); QVERIFY(byte > 0); const auto span = view.scrollSpanForSource(byte, identity); QVERIFY(span); QCOMPARE(span->sourceStart, quint32(byte)); QVERIFY(span->sourceEnd > span->sourceStart); QVERIFY(span->bottom > span->top);
            const auto inverse = view.scrollSpanAtY((span->top + span->bottom) / 2, identity); QVERIFY(inverse); QCOMPARE(inverse->sourceStart, span->sourceStart);
        }
        const auto c = byteAt(source, QStringLiteral("## 锚点 C")); QVERIFY(c != QString::fromUtf8(source.first(c)).size());
        auto stale = identity; ++stale.path; QVERIFY(!view.scrollSpanForSource(c, stale)); QVERIFY(!view.scrollSpanAtY(0, stale));
        view.resize(480, 380); QVERIFY(!view.scrollSpanForSource(c, identity)); QTRY_VERIFY(view.ready()); QVERIFY(view.scrollSpanForSource(c, identity));
        ++stale.request; view.expect(stale); QVERIFY(!view.scrollSpanForSource(c, identity)); QVERIFY(!view.scrollSpanAtY(0, stale));
    }
    void codeListsAndTablesKeepTheirOwnSpans() {
        Appearance appearance; NativePreviewView view(&appearance); view.resize(640, 420); view.show(); const auto source = sample(); view.expect(identity); QVERIFY(view.presentAsync(identity, derived(source))); QTRY_VERIFY(view.ready());
        const auto one = view.scrollSpanForSource(source.indexOf("int value_1 ="), identity), two = view.scrollSpanForSource(source.indexOf("int value_50 ="), identity); QVERIFY(one && two); QCOMPARE(one->sourceStart, two->sourceStart); QVERIFY(one->bottom - one->top > 800);
        const auto list = view.scrollSpanForSource(byteAt(source, QStringLiteral("列表项二")), identity); QVERIFY(list); QVERIFY(list->sourceStart != one->sourceStart);
        const auto row1 = view.scrollSpanForSource(byteAt(source, QStringLiteral("第一行")), identity), row2 = view.scrollSpanForSource(byteAt(source, QStringLiteral("第二行")), identity); QVERIFY(row1 && row2); QVERIFY(row1->sourceStart < row2->sourceStart); QVERIFY(row1->top < row2->top);
        QVERIFY(!view.scrollSpanForSource(quint32(source.size() + 1), identity)); QVERIFY(!view.scrollSpanAtY(-1, identity));
    }
    void actualSplitFollowsBlocksInBothDirectionsWithoutChangingTheBuffer() {
        Fixture f; QVERIFY(f.written); QTRY_VERIFY(f.pane()); auto *pane = f.pane(); pane->setViewMode(SingleFileWindow::ViewMode::Split); QVERIFY(waitForMapping(pane));
        auto *editor = pane->editor(); auto *view = pane->preview()->view(); auto *reader = view->verticalScrollBar(); editor->send(SCI_SETSEL, 2, 10); QVERIFY(waitForMapping(pane)); const auto anchor = editor->send(SCI_GETANCHOR), caret = editor->send(SCI_GETCURRENTPOS); auto *focus = QApplication::focusWidget(); const auto original = pane->document()->session()->snapshot();
        const int b = byteAt(f.source, QStringLiteral("## 锚点 B")); const auto targetB = view->scrollSpanForSource(b, view->displayedStamp()); QVERIFY(targetB);
        QSignalSpy moves(reader, &QScrollBar::valueChanged); sourceScroll(editor, visualLine(editor, b)); QTRY_VERIFY(std::abs(reader->value() - targetB->top) <= 2); QVERIFY(waitForMapping(pane)); const auto settled = moves.size(); QTest::qWait(150);
        QStringList steps; for (const auto &move : moves) if (!move.isEmpty()) steps.append(QString::number(move.first().toInt()));
        QVERIFY2(moves.size() == settled, qPrintable(QStringLiteral("extra reader moves: %1 -> %2; reader=%3 target=%4; source=%5 expected=%6; events=%7")
            .arg(settled).arg(moves.size()).arg(reader->value()).arg(targetB->top).arg(editor->send(SCI_GETFIRSTVISIBLELINE)).arg(visualLine(editor, b)).arg(steps.join(','))));
        QCOMPARE(editor->send(SCI_GETFIRSTVISIBLELINE), visualLine(editor, b));
        const auto sourceRatio = double(editor->verticalScrollBar()->value()) / editor->verticalScrollBar()->maximum(); const auto readerRatio = double(reader->value()) / reader->maximum(); QVERIFY(std::abs(sourceRatio - readerRatio) > .05);
        const int c = byteAt(f.source, QStringLiteral("## 锚点 C")); const auto targetC = view->scrollSpanForSource(c, view->displayedStamp()); QVERIFY(targetC); readerScroll(view, qRound(targetC->top)); QTRY_VERIFY(std::abs(editor->send(SCI_GETFIRSTVISIBLELINE) - visualLine(editor, c)) <= 1);
        QCOMPARE(editor->send(SCI_GETANCHOR), anchor); QCOMPARE(editor->send(SCI_GETCURRENTPOS), caret); QCOMPARE(QApplication::focusWidget(), focus); QCOMPARE(pane->document()->session()->snapshot().utf8, original.utf8); QCOMPARE(pane->document()->session()->revision(), original.revision); QVERIFY(!pane->document()->dirty());
        editor->send(SCI_SETSEL, 6, 12); readerScroll(view, reader->maximum()); QTRY_COMPARE(editor->verticalScrollBar()->value(), editor->verticalScrollBar()->maximum());
        // A late, internal source scroll must restore the source to the reader,
        // not seize navigation and pull the reader back to an old paragraph.
        editor->send(SCI_SETFIRSTVISIBLELINE, uptr_t(visualLine(editor, b))); QVERIFY(waitForMapping(pane)); QCOMPARE(reader->value(), reader->maximum()); QCOMPARE(editor->verticalScrollBar()->value(), editor->verticalScrollBar()->maximum());
        pane->setContentZoom(125); QVERIFY(waitForMapping(pane)); QCOMPARE(reader->value(), reader->maximum()); sourceScroll(editor, 0); QTRY_COMPARE(reader->value(), 0);
        reader->setValue(600); QVERIFY(waitForMapping(pane)); QCOMPARE(reader->value(), 0); QCOMPARE(editor->verticalScrollBar()->value(), 0);
        pane->setContentZoom(150); pane->preview()->navigateToHeading(QStringLiteral("锚点-c"), pane->document()->session()->revision()); QVERIFY(waitForMapping(pane)); QTRY_VERIFY(std::abs(editor->send(SCI_GETFIRSTVISIBLELINE) - visualLine(editor, c)) <= 1);
    }
    void wrappedParagraphAndReaderAnchorSurviveZoomResizeAndTheme() {
        Fixture f; QVERIFY(f.written); QTRY_VERIFY(f.pane()); auto *pane = f.pane(); pane->setViewMode(SingleFileWindow::ViewMode::Split); QVERIFY(waitForMapping(pane));
        auto *editor = pane->editor(); auto *view = pane->preview()->view(); const int byte = byteAt(f.source, QStringLiteral("长段落")); const auto line = editor->send(SCI_LINEFROMPOSITION, uptr_t(byte)); const auto rows = editor->send(SCI_WRAPCOUNT, uptr_t(line)); QVERIFY(rows > 10);
        sourceScroll(editor, visualLine(editor, byte) + rows / 2); const auto span = view->scrollSpanForSource(byte, view->displayedStamp()); QVERIFY(span); QTRY_VERIFY(view->verticalScrollBar()->value() > span->top + (span->bottom - span->top) * .25); QVERIFY(view->verticalScrollBar()->value() < span->top + (span->bottom - span->top) * .75);
        // A reader fraction must survive reflow even when it falls between two
        // source display rows; restoring from a rounded source row would drift.
        readerScroll(view, qRound(span->top + .413 * (span->bottom - span->top))); QVERIFY(waitForMapping(pane));
        pane->setContentZoom(150); f.workspace->resize(1030, 760); QVERIFY(pane->appearance()->setPreferences("dark", "olive")); QVERIFY(waitForMapping(pane));
        const auto changed = view->scrollSpanForSource(byte, view->displayedStamp()); QVERIFY(changed); const auto fraction = (view->verticalScrollBar()->value() - changed->top) / (changed->bottom - changed->top); QVERIFY2(std::abs(fraction - .413) < .015, qPrintable(QString::number(fraction)));
        const auto readerPosition = view->verticalScrollBar()->value(); editor->send(SCI_SETWRAPMODE, SC_WRAP_NONE); QVERIFY(waitForMapping(pane)); QCOMPARE(view->verticalScrollBar()->value(), readerPosition); editor->send(SCI_SETWRAPMODE, SC_WRAP_WORD); QVERIFY(waitForMapping(pane)); QCOMPARE(view->verticalScrollBar()->value(), readerPosition);
        const auto sourceFraction = [&] { return double(editor->send(SCI_GETFIRSTVISIBLELINE) - visualLine(editor, byte)) / qMax(sptr_t(1), editor->send(SCI_WRAPCOUNT, uptr_t(line))); };
        QTRY_VERIFY_WITH_TIMEOUT(std::abs(sourceFraction() - .413) < .05, 3000);
        const auto b = byteAt(f.source, QStringLiteral("## 锚点 B")); sourceScroll(editor, visualLine(editor, b)); QVERIFY(waitForMapping(pane)); pane->setContentZoom(125); QVERIFY(waitForMapping(pane));
        QTRY_COMPARE(editor->send(SCI_DOCLINEFROMVISIBLE, uptr_t(editor->send(SCI_GETFIRSTVISIBLELINE))), editor->send(SCI_LINEFROMPOSITION, uptr_t(b)));
        const auto current = view->scrollSpanForSource(b, view->displayedStamp()); QVERIFY(current); QVERIFY(std::abs(view->verticalScrollBar()->value() - current->top) <= 2);
        QCOMPARE(pane->document()->session()->snapshot().utf8, f.source);
        const auto evidence = qEnvironmentVariable("XDNOTE_TEST_EVIDENCE"); if (!evidence.isEmpty()) { QVERIFY(QDir().mkpath(evidence)); QVERIFY(f.workspace->grab().save(evidence + "/split-scroll-dark.png")); }
    }
    void editingAndRevokedRequestCannotUseOldMappings() {
        Fixture f; QVERIFY(f.written); QTRY_VERIFY(f.pane()); auto *pane = f.pane(); pane->setViewMode(SingleFileWindow::ViewMode::Split); QVERIFY(waitForMapping(pane));
        auto *editor = pane->editor(); auto *view = pane->preview()->view(); auto old = view->displayedStamp();
        QInputMethodEvent preedit(QStringLiteral("正在输入"), {}); QApplication::sendEvent(editor, &preedit); QVERIFY(pane->document()->session()->isComposing()); QVERIFY(!pane->preview()->mappingCurrent()); const auto before = view->verticalScrollBar()->value(); sourceScroll(editor, 40); QTest::qWait(25); QCOMPARE(view->verticalScrollBar()->value(), before);
        QInputMethodEvent cancel; QApplication::sendEvent(editor, &cancel); QTRY_VERIFY(!pane->document()->session()->isComposing()); QTRY_VERIFY(pane->preview()->mappingCurrent()); sourceScroll(editor, 40); QTRY_VERIFY(view->verticalScrollBar()->value() != before);
        editor->send(SCI_INSERTTEXT, 0, reinterpret_cast<sptr_t>("inserted\n\n")); QVERIFY(!pane->preview()->mappingCurrent()); QTRY_VERIFY(pane->preview()->mappingCurrent()); QVERIFY(view->displayedStamp().revision > old.revision);
        auto revoked = view->displayedStamp(); ++revoked.resources; view->expect(revoked); const auto retained = view->verticalScrollBar()->value(); sourceScroll(editor, 45); QTest::qWait(40); QCOMPARE(view->verticalScrollBar()->value(), retained); QVERIFY(!pane->preview()->mappingCurrent());
        pane->preview()->refresh(); QVERIFY(waitForMapping(pane)); const auto changed = pane->document()->session()->snapshot(); QCOMPARE(changed.utf8, QByteArray("inserted\n\n") + f.source); QCOMPARE(pane->document()->session()->undo(), EditResult::Applied); QTRY_VERIFY(pane->preview()->mappingCurrent()); QCOMPARE(pane->document()->session()->snapshot().utf8, f.source);
    }
    void deferredSnapshotsResumeAfterBusyNotificationsAndComposition() {
        DocumentSession session("# deferred\n\nretained"); Appearance appearance;
        QCOMPARE(session.beginResourceUpdate(), EditResult::Applied);
        MarkdownPreview preview(&session, nullptr, 24, &appearance); preview.resize(640, 420); preview.show(); QTest::qWait(30); QVERIFY(!preview.view()->content());
        session.endResourceUpdate(); QTRY_VERIFY(preview.mappingCurrent()); const auto before = preview.view()->displayedStamp();
        const auto snapshot = session.snapshot(); bool rebound = false;
        const auto connection = connect(&session, &DocumentSession::stateChanged, &preview, [&] {
            if (rebound) return; rebound = true;
            QVERIFY(session.isBusy()); preview.setResourceContext({}, "after-save.md", "new-file-identity");
        });
        QCOMPARE(session.confirmSaved(snapshot), EditResult::Applied); QObject::disconnect(connection); QTRY_VERIFY(preview.mappingCurrent()); QVERIFY(rebound); QVERIFY(preview.view()->displayedStamp().path > before.path); QCOMPARE(session.snapshot().utf8, snapshot.utf8);
        DocumentSession composing("# IME\n"); SourceEditor editor(&composing, nullptr, &appearance); editor.show();
        QInputMethodEvent preedit(QStringLiteral("组合输入"), {}); QApplication::sendEvent(&editor, &preedit); QVERIFY(composing.isComposing());
        MarkdownPreview delayed(&composing, nullptr, 24, &appearance); delayed.resize(640, 420); delayed.show(); QTest::qWait(30); QVERIFY(!delayed.view()->content());
        QInputMethodEvent cancel; QApplication::sendEvent(&editor, &cancel); QTRY_VERIFY(!composing.isComposing()); QTRY_VERIFY(delayed.mappingCurrent());
        DocumentSession closing("close while deferred"); QCOMPARE(closing.beginResourceUpdate(), EditResult::Applied); auto *pending = new MarkdownPreview(&closing); pending->show(); QTest::qWait(20); QPointer<MarkdownPreview> deleted(pending); pending->deleteLater(); closing.endResourceUpdate(); QTRY_VERIFY(deleted.isNull());
    }
    void wheelAndKeyboardScrollDriveTheOppositeView() {
        Fixture f; QVERIFY(f.written); QTRY_VERIFY(f.pane()); auto *pane = f.pane(); pane->setViewMode(SingleFileWindow::ViewMode::Split); QVERIFY(waitForMapping(pane));
        auto *editor = pane->editor(); auto *view = pane->preview()->view();
        QWheelEvent wheel(QPointF(100, 100), QPointF(editor->viewport()->mapToGlobal(QPoint(100, 100))), {}, QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(editor->viewport(), &wheel); QTRY_VERIFY(editor->send(SCI_GETFIRSTVISIBLELINE) > 0); QTRY_VERIFY(view->verticalScrollBar()->value() > 0);
        QVERIFY(waitForMapping(pane)); const auto beforeDrag = view->verticalScrollBar()->value(); const QPoint start(80, view->viewport()->height() - 12), outside(80, view->viewport()->height() + 18);
        QTest::mousePress(view->viewport(), Qt::LeftButton, {}, start); QMouseEvent drag(QEvent::MouseMove, QPointF(outside), QPointF(view->viewport()->mapToGlobal(outside)), Qt::NoButton, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(view->viewport(), &drag); QTest::mouseRelease(view->viewport(), Qt::LeftButton, {}, start); QVERIFY(waitForMapping(pane)); QVERIFY(view->verticalScrollBar()->value() > beforeDrag);
        view->setFocus(Qt::OtherFocusReason); QVERIFY(waitForMapping(pane)); QKeyEvent end(QEvent::KeyPress, Qt::Key_End, Qt::ControlModifier); QApplication::sendEvent(view, &end); QCOMPARE(view->selection().second, view->content()->text.size()); QVERIFY(waitForMapping(pane)); QVERIFY(view->verticalScrollBar()->maximum() > 0); QCOMPARE(view->verticalScrollBar()->value(), view->verticalScrollBar()->maximum()); QCOMPARE(editor->verticalScrollBar()->value(), editor->verticalScrollBar()->maximum());
        QKeyEvent home(QEvent::KeyPress, Qt::Key_Home, Qt::ControlModifier); QApplication::sendEvent(view, &home); QCOMPARE(view->selection().second, 0); QVERIFY(waitForMapping(pane)); QTRY_COMPARE(editor->verticalScrollBar()->value(), 0); QVERIFY(view->characterRect(0).intersects(QRect(view->viewport()->mapToGlobal(QPoint()), view->viewport()->size()))); QCOMPARE(pane->document()->session()->snapshot().utf8, f.source);
    }
    void modesHiddenTabsAndPendingCloseRemainIndependent() {
        Fixture f; QVERIFY(f.written); QTRY_VERIFY(f.pane()); auto *pane = f.pane(); pane->setViewMode(SingleFileWindow::ViewMode::Split); QVERIFY(waitForMapping(pane)); auto *editor = pane->editor(); auto *view = pane->preview()->view();
        pane->setViewMode(SingleFileWindow::ViewMode::Reading); QVERIFY(waitForMapping(pane)); const auto hidden = editor->send(SCI_GETFIRSTVISIBLELINE); readerScroll(view, 400); QVERIFY(waitForMapping(pane)); QCOMPARE(editor->send(SCI_GETFIRSTVISIBLELINE), hidden);
        const auto readingAnchor = view->scrollSpanAtY(view->verticalScrollBar()->value(), view->displayedStamp()); QVERIFY(readingAnchor); const auto readingFraction = (view->verticalScrollBar()->value() - readingAnchor->top) / (readingAnchor->bottom - readingAnchor->top);
        pane->setViewMode(SingleFileWindow::ViewMode::Split); QVERIFY(waitForMapping(pane)); const auto splitAnchor = view->scrollSpanForSource(readingAnchor->sourceStart, view->displayedStamp()); QVERIFY(splitAnchor); const auto splitFraction = (view->verticalScrollBar()->value() - splitAnchor->top) / (splitAnchor->bottom - splitAnchor->top); QVERIFY(std::abs(readingFraction - splitFraction) < .02);
        const auto other = f.directory.filePath("other.md"); QVERIFY(fileFixtures::write(other, "# Other\n\nother")); f.app.openInWorkspace(other, f.workspace); QTRY_VERIFY(f.workspace->activePane() && f.workspace->activePane() != pane); auto *active = f.workspace->activePane(); QVERIFY(waitForMapping(active)); const auto otherPosition = active->preview()->readingPosition(); sourceScroll(editor, 80); QTest::qWait(30); QCOMPARE(active->preview()->readingPosition(), otherPosition);
        f.workspace->activatePane(pane->paneId()); QTRY_VERIFY(pane->isVisible()); QVERIFY(waitForMapping(pane)); sourceScroll(editor, 90); QPointer<SingleFileWindow> closed(pane); f.workspace->closePane(pane->paneId()); QTRY_VERIFY(closed.isNull()); QCOMPARE(f.workspace->activePane(), active); QCOMPARE(active->document()->session()->snapshot().utf8, QByteArray("# Other\n\nother"));
    }
};
int main(int argc, char **argv) { initializeGuiRuntime(); QApplication app(argc, argv); configureGuiApplication(); app.setQuitOnLastWindowClosed(false); SourceEditor::installAccessibility(); ScrollSyncTests tests; return QTest::qExec(&tests, argc, argv); }
#include "scroll_sync_tests.moc"
