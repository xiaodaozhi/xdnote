#include "AdmissionWindow.h"
#include "DocumentSession.h"
#include "MarkdownPreview.h"
#include "SourceEditor.h"
#include "VisualTokens.h"

#include <QAccessible>
#include <QApplication>
#include <QInputMethodEvent>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWidget>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QWebEnginePage>
#include <QWebEngineSettings>
#include <QWebEngineView>
#include <future>

using namespace xdnote;

class DesktopTests final : public QObject {
    Q_OBJECT
private slots:
    void literalTextAndAtomicUndo() {
        QByteArray original = QStringLiteral("中文😀\u00a0\u2028\u2029\r\n末尾").toUtf8();
        original.append('\0'); original.append("after NUL\n");
        DocumentSession session(original);
        SourceEditor first(&session), second(&session);
        QCOMPARE(session.snapshot().utf8, original);
        QCOMPARE(first.send(SCI_GETDOCPOINTER), second.send(SCI_GETDOCPOINTER));
        auto rev = session.revision();
        QCOMPARE(session.replace(3, 4, "x", rev), EditResult::InvalidRange); // middle of emoji
        QCOMPARE(session.replace(0, 0, QString(QChar(0xd800)), rev), EditResult::InvalidText);
        QCOMPARE(session.snapshot().utf8, original);
        QCOMPARE(session.replace(2, 4, QStringLiteral("你好"), rev), EditResult::Applied);
        QVERIFY(session.revision() > rev);
        QVERIFY(session.snapshot().utf8.startsWith(QStringLiteral("中文你好").toUtf8()));
        QCOMPARE(session.undo(), EditResult::Applied);
        QCOMPARE(session.snapshot().utf8, original);
        QCOMPARE(session.redo(), EditResult::Applied);
        QCOMPARE(session.replace(0, 0, "stale", rev), EditResult::StaleRevision);
    }

    void viewLifetimeAndReadOnly() {
        auto session = std::make_unique<DocumentSession>("initial");
        auto view = std::make_unique<SourceEditor>(session.get());
        QCOMPARE(session->replace(0, 7, "edited", 0), EditResult::Applied);
        view.reset();
        SourceEditor replacement(session.get());
        QCOMPARE(session->snapshot().utf8, QByteArray("edited"));
        QCOMPARE(session->setReadOnly(true), EditResult::Applied);
        QCOMPARE(session->replace(0, 0, "blocked", session->revision()), EditResult::ReadOnly);
        QCOMPARE(session->undo(), EditResult::ReadOnly);
        QVERIFY(!session->rejection().isEmpty());
        QCOMPARE(session->snapshot().utf8, QByteArray("edited"));
        QCOMPARE(session->setReadOnly(false), EditResult::Applied);
        QCOMPARE(session->undo(), EditResult::Applied);
        QCOMPARE(session->snapshot().utf8, QByteArray("initial"));
        session.reset();
        QVERIFY(replacement.session() == nullptr);
        QVERIFY(replacement.send(SCI_GETREADONLY));
    }

    void compositionAndCommit() {
        DocumentSession session;
        SourceEditor view(&session);
        QInputMethodEvent preedit(QStringLiteral("ni"), {});
        QApplication::sendEvent(&view, &preedit);
        QVERIFY(view.compositionPending());
        QCOMPARE(session.snapshot().result, EditResult::Composing);
        QCOMPARE(session.replace(0, 0, "bad", 0), EditResult::Composing);
        QCOMPARE(session.requestClose(), EditResult::Composing);
        QInputMethodEvent mixed(QStringLiteral("hao"), {});
        mixed.setCommitString(QStringLiteral("你"));
        QApplication::sendEvent(&view, &mixed);
        QCOMPARE(session.snapshot().result, EditResult::Composing);
        QInputMethodEvent commit;
        commit.setCommitString(QStringLiteral("好"));
        QApplication::sendEvent(&view, &commit);
        QVERIFY(!view.compositionPending());
        QCOMPARE(session.snapshot().utf8, QStringLiteral("你好").toUtf8());
        QCOMPARE(session.undo(), EditResult::Applied);
        QVERIFY(!session.snapshot().utf8.contains("hao"));
    }

    void reentrancyClosingAndWrongThread() {
        DocumentSession session("a");
        EditResult reentrant = EditResult::Applied;
        QSignalSpy closed(&session, &DocumentSession::closed);
        connect(&session, &DocumentSession::changed, &session, [&](quint64 rev) {
            reentrant = session.replace(0, 0, "bad", rev);
            QCOMPARE(session.snapshot().result, EditResult::Busy);
            QCOMPARE(session.requestClose(), EditResult::Applied);
        });
        QCOMPARE(session.replace(0, 1, "b", 0), EditResult::Applied);
        QCOMPARE(reentrant, EditResult::Busy);
        QCOMPARE(session.snapshot().result, EditResult::Closing);
        QCOMPARE(session.replace(0, 0, "bad", 1), EditResult::Closing);
        QTRY_COMPARE(closed.count(), 1);
        auto wrongThread = std::async(std::launch::async, [&] { return session.snapshot().result; });
        QCOMPARE(wrongThread.get(), EditResult::WrongThread);
    }

    void accessibleCommandsAndInvalidation() {
        auto session = std::make_unique<DocumentSession>(QStringLiteral("甲😀乙").toUtf8());
        SourceEditor view(session.get());
        auto *accessible = QAccessible::queryAccessibleInterface(&view);
        QVERIFY(accessible && accessible->isValid());
        auto *text = accessible->textInterface();
        auto *edit = accessible->editableTextInterface();
        QVERIFY(text && edit);
        QCOMPARE(text->characterCount(), 4);
        edit->replaceText(1, 2, "bad");
        QCOMPARE(session->snapshot().utf8, QStringLiteral("甲😀乙").toUtf8());
        edit->replaceText(1, 3, QStringLiteral("中文"));
        QCOMPARE(session->snapshot().utf8, QStringLiteral("甲中文乙").toUtf8());
        session->setReadOnly(true);
        accessible->setText(QAccessible::Value, "blocked");
        QCOMPARE(session->snapshot().utf8, QStringLiteral("甲中文乙").toUtf8());
        QVERIFY(accessible->state().readOnly);
        QVERIFY(!accessible->text(QAccessible::Description).isEmpty());
        session->setReadOnly(false);
        QCOMPARE(session->undo(), EditResult::Applied);
        QCOMPARE(text->text(0, -1), QStringLiteral("甲😀乙"));
        session.reset();
        QVERIFY(!accessible->isValid());
        QCOMPARE(text->characterCount(), 0);
        edit->insertText(0, "ignored");
    }

    void previewIsolationAndLatestRevision() {
        DocumentSession session("# safe\n<script>alert(1)</script>\n![x](file:///private)\n[x](https://example.invalid)");
        MarkdownPreview preview(&session);
        QSignalSpy ready(&preview, &MarkdownPreview::previewReady);
        QSignalSpy errors(&preview, &MarkdownPreview::previewError);
        preview.resize(700, 500); preview.show();
        QTRY_VERIFY_WITH_TIMEOUT(ready.count() > 0, 30000);
        QVERIFY(!preview.view()->settings()->testAttribute(QWebEngineSettings::JavascriptEnabled));
        QVERIFY(!preview.view()->settings()->testAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls));
        QString html;
        bool received = false;
        preview.view()->page()->toHtml([&](const QString &value) { html = value; received = true; });
        QTRY_VERIFY_WITH_TIMEOUT(received, 10000);
        QVERIFY(html.contains("&lt;script&gt;"));
        QVERIFY(!html.contains("<script>"));
        QVERIFY(!html.contains("file:///private"));
        QVERIFY(!html.contains("href=\"https://"));
        QVERIFY(html.contains("default-src 'none'"));
        for (int i = 0; i < 8; ++i) {
            const auto snap = session.snapshot();
            QCOMPARE(session.replace(0, QString::fromUtf8(snap.utf8).size(), QStringLiteral("# latest %1").arg(i), snap.revision), EditResult::Applied);
            preview.refresh();
        }
        const auto latest = session.revision();
        QTRY_COMPARE_WITH_TIMEOUT(preview.displayedRevision(), latest, 30000);
        QCOMPARE(errors.count(), 0);
        ready.clear();
        session.requestClose();
        QTRY_VERIFY(session.isClosing());
        QTest::qWait(100);
        QCOMPARE(ready.count(), 0);
    }

    void mixedHostAndSharedChrome() {
        DocumentSession session("# integration");
        AdmissionWindow window(&session);
        auto *quick = window.findChild<QQuickWidget *>("sharedChrome");
        QVERIFY(quick);
        QCOMPARE(quick->status(), QQuickWidget::Ready);
        QVERIFY(quick->errors().isEmpty());
        const auto geometry = visualTokens()["windows"].toObject()["main"].toObject();
        QCOMPARE(quick->height(), geometry["toolbarHeight"].toInt());
        window.show();
        QTRY_VERIFY(window.isVisible());
        window.toggleReadOnly();
        QVERIFY(session.isReadOnly());
        window.toggleReadOnly();
        QVERIFY(!session.isReadOnly());
        const auto evidence = qEnvironmentVariable("XDNOTE_TEST_EVIDENCE");
        if (!evidence.isEmpty()) QVERIFY(window.grab().save(evidence + "/desktop-admission.png"));
    }
};

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QApplication app(argc, argv);
    SourceEditor::installAccessibility();
    DesktopTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "desktop_tests.moc"
