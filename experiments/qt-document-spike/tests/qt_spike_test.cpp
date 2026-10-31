#include "spike_document.h"
#include "spike_handoff.h"
#include "spike_ipc.h"
#include "file_identity_probe.h"
#include "image_reference_probe.h"
#include "displaced_probe.h"

#include <QAccessible>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QInputMethodEvent>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextFrame>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QTextList>
#include <QTextTable>
#include <QTimer>
#include <QUuid>
#include <QtEndian>
#include <QtTest>

#include <memory>

#ifdef Q_OS_WIN
#  define NOMINMAX
#  include <windows.h>
#  include "windows_commit_diagnostic.h"
#else
#  include <unistd.h>
#endif

namespace {
std::unique_ptr<QTemporaryDir> fixtureDirectory()
{
    const QString executableDir = QDir(QCoreApplication::applicationDirPath()).canonicalPath();
    if (executableDir.isEmpty())
        return {};
    const QString requestedRoot = executableDir + QStringLiteral("/spike-test-data");
    if (QFileInfo(requestedRoot).isSymLink() || !QDir().mkpath(requestedRoot))
        return {};
    const QString root = QDir(requestedRoot).canonicalPath();
    if (!root.startsWith(executableDir + QLatin1Char('/')))
        return {};

    auto temporary = std::make_unique<QTemporaryDir>(root + QStringLiteral("/fixture-XXXXXX"));
    temporary->setAutoRemove(false);
    if (!temporary->isValid())
        return {};
    const QString actual = QDir(temporary->path()).canonicalPath();
    if (!actual.startsWith(root + QStringLiteral("/fixture-")))
        return {};
    // The verified test-owned subdirectory is the only recursive cleanup target.
    temporary->setAutoRemove(true);
    return temporary;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size() && file.flush();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QString fixtureWriterPath()
{
    QString executable = QCoreApplication::applicationDirPath() + QStringLiteral("/qt_fixture_writer");
#ifdef Q_OS_WIN
    executable += QStringLiteral(".exe");
#endif
    return executable;
}

bool startFixtureProcess(QProcess *writer, const QStringList &arguments, QString *error)
{
    writer->setProgram(fixtureWriterPath());
    writer->setArguments(arguments);
    writer->start();
    if (!writer->waitForStarted(5000)) {
        *error = writer->errorString();
        return false;
    }
    QElapsedTimer deadline;
    deadline.start();
    QByteArray response;
    while (!response.contains('\n') && deadline.elapsed() < 5000) {
        if (!writer->waitForReadyRead(int(5000 - deadline.elapsed()))) {
            *error = writer->errorString();
            return false;
        }
        response += writer->readAllStandardOutput();
    }
    if (response.trimmed() != QByteArray("READY")) {
        *error = QStringLiteral("Fixture writer did not report READY");
        return false;
    }
    return true;
}

bool startFixtureWriter(QProcess *writer, const QString &path, QString *error)
{
    return startFixtureProcess(writer, {path}, error);
}

bool finishFixtureWriter(QProcess *writer, QString *error)
{
    if (writer->write("W\n") != 2) {
        *error = writer->errorString();
        return false;
    }
    if (writer->bytesToWrite() > 0 && !writer->waitForBytesWritten(5000)) {
        *error = writer->errorString();
        return false;
    }
    writer->closeWriteChannel();
    if (!writer->waitForFinished(5000) || writer->exitStatus() != QProcess::NormalExit
        || writer->exitCode() != 0) {
        *error = QStringLiteral("Fixture writer failed: %1, exit %2")
                     .arg(writer->errorString()).arg(writer->exitCode());
        return false;
    }
    return true;
}

bool finishFixtureReader(QProcess *reader, QString *error)
{
    if (reader->write("R\n") != 2) {
        *error = reader->errorString();
        return false;
    }
    if (reader->bytesToWrite() > 0 && !reader->waitForBytesWritten(5000)) {
        *error = reader->errorString();
        return false;
    }
    reader->closeWriteChannel();
    if (!reader->waitForFinished(5000) || reader->exitStatus() != QProcess::NormalExit
        || reader->exitCode() != 0 || reader->readAllStandardOutput().trimmed() != "HELD_ORIGINAL") {
        *error = QStringLiteral("Fixture reader failed: %1, exit %2")
                     .arg(reader->errorString()).arg(reader->exitCode());
        return false;
    }
    return true;
}

bool createHardLink(const QString &existing, const QString &alias)
{
#ifdef Q_OS_WIN
    const QString nativeExisting = QDir::toNativeSeparators(existing);
    const QString nativeAlias = QDir::toNativeSeparators(alias);
    return CreateHardLinkW(reinterpret_cast<LPCWSTR>(nativeAlias.utf16()),
                           reinterpret_cast<LPCWSTR>(nativeExisting.utf16()), nullptr) != 0;
#else
    return ::link(QFile::encodeName(existing).constData(), QFile::encodeName(alias).constData()) == 0;
#endif
}

QQuickItem *makeEditor(QQmlEngine &engine, QQuickWindow &window, bool accessible = false)
{
    QQmlComponent component(&engine);
    QByteArray source = QByteArrayLiteral(
        "import QtQuick\n"
        "TextEdit { width: 320; height: 200; textFormat: TextEdit.PlainText; "
        "selectByMouse: true; objectName: 'spikeEditor'; ");
    if (accessible)
        source += QByteArrayLiteral("Accessible.role: Accessible.EditableText; "
                                    "Accessible.name: 'Document'; Accessible.readOnly: readOnly; ");
    source += QByteArrayLiteral("}\n");
    component.setData(source, QUrl());
    QObject *created = component.create();
    if (!created) {
        qWarning() << component.errors();
        return nullptr;
    }
    auto *editor = qobject_cast<QQuickItem *>(created);
    if (!editor) {
        delete created;
        return nullptr;
    }
    editor->setParent(&window);
    editor->setParentItem(window.contentItem());
    return editor;
}

QQuickTextDocument *quickDocument(QQuickItem *editor)
{
    return editor->property("textDocument").value<QQuickTextDocument *>();
}

} // namespace

class QtDocumentSpikeTests : public QObject
{
    Q_OBJECT
private slots:
    void sharedDocumentTransferPreservesUndoAndSelection_data();
    void sharedDocumentTransferPreservesUndoAndSelection();
    void syntheticImeCompositionDefersTransfer();
    void quickEditorAccessibilityWithExplicitRole();
    void quickAccessibilityInterfacePersistsAfterDocumentReplacement();
    void singleEditorReparentPreservesAccessibilityAndUndo_data();
    void singleEditorReparentPreservesAccessibilityAndUndo();
    void previewParsesMarkdownAndBlocksResources();
    void markdownCorpusParsesBasicSemantics();
    void markdownAnchorsRemainUntrusted();
    void previewUsesPlaceholdersForLocalAndRemoteImages();
    void imageReferenceLexicalCandidate_data();
    void imageReferenceLexicalCandidate();
    void markdownImageNamesStayBlocked();
    void imageReferenceCanEscapeThroughSymlink();
    void previewLeavesSourceUndoIndependent();
    void openIntentFramesHandlePartialAndInvalidInput();
    void openIntentMovesBetweenRealLocalProcesses();
    void utf8BomAndCrlfRoundTrip();
    void nonBreakingSpaceIsNotRewritten();
    void unsupportedUnicodeSeparatorsDoNotOverwrite();
    void paragraphSeparatorBecomesIndistinguishableFromNewline();
    void rejectsInvalidUtf8AndMixedLineEndings();
    void externalChangeBeforeSaveIsConflict();
    void externalChangeBeforeCommitIsConflict();
    void identicalBytesReplacementIsNotDetected();
    void fileIdentityHandlesObserveAliasesAndReplacement();
    void fileIdentityProbeRejectsUnsupportedTargets();
    void changeAfterFinalCheckCanBeOverwritten();
    void fixtureWriterRejectsOutsideScope();
    void readerHandleAndReplacement_data();
    void readerHandleAndReplacement();
    void displacedFileRetainsInterveningWrite();
    void openWriterCanStillChangeDisplacedFile();
    void missingTargetDoesNotReplacePreparedFile();
    void editDuringSaveStaysDirty();
    void deletedTargetIsNotRecreated();
#ifdef Q_OS_WIN
    void windowsSharingFailuresRetainEdits();
    void windowsSaveFailureObservations();
    void windowsUiaValueWrite_data();
    void windowsUiaValueWrite();
#endif
};

void QtDocumentSpikeTests::sharedDocumentTransferPreservesUndoAndSelection_data()
{
    QTest::addColumn<bool>("reverse");
    QTest::newRow("forward selection") << false;
    QTest::newRow("reverse selection") << true;
}

void QtDocumentSpikeTests::sharedDocumentTransferPreservesUndoAndSelection()
{
    QFETCH(bool, reverse);
    QTextDocument shared;
    QQmlEngine engine;
    QQuickWindow standalone;
    QQuickWindow workspace;
    standalone.create();
    workspace.create();
    QVERIFY(standalone.handle());
    QVERIFY(workspace.handle());

    QQuickItem *first = makeEditor(engine, standalone);
    QQuickItem *second = makeEditor(engine, workspace);
    QVERIFY(first);
    QVERIFY(second);
    QVERIFY(quickDocument(first));
    QVERIFY(quickDocument(second));
    shared.setPlainText(QStringLiteral("甲乙丙"));
    shared.clearUndoRedoStacks();
    quickDocument(first)->setTextDocument(&shared);
    QCOMPARE(first->property("text").toString(), QStringLiteral("甲乙丙"));

    QVERIFY(QMetaObject::invokeMethod(first, "insert", Qt::DirectConnection,
                                     Q_ARG(int, 3), Q_ARG(QString, QStringLiteral("丁"))));
    QCOMPARE(shared.toPlainText(), QStringLiteral("甲乙丙丁"));
    QVERIFY(shared.isUndoAvailable());
    QVERIFY(QMetaObject::invokeMethod(first, "select", Qt::DirectConnection,
                                     Q_ARG(int, reverse ? 3 : 1), Q_ARG(int, reverse ? 1 : 3)));
    const int selectionStart = first->property("selectionStart").toInt();
    const int selectionEnd = first->property("selectionEnd").toInt();
    const int cursor = first->property("cursorPosition").toInt();
    QVERIFY(selectionStart != selectionEnd);
    QCOMPARE(cursor, reverse ? selectionStart : selectionEnd);

    QVERIFY(transferDocument(first, second));
    QCOMPARE(quickDocument(second)->textDocument(), &shared);
    QCOMPARE(second->property("selectionStart").toInt(), selectionStart);
    QCOMPARE(second->property("selectionEnd").toInt(), selectionEnd);
    QCOMPARE(second->property("cursorPosition").toInt(), cursor);
    delete first;
    QCOMPARE(second->property("text").toString(), QStringLiteral("甲乙丙丁"));

    QVERIFY(QMetaObject::invokeMethod(second, "undo", Qt::DirectConnection));
    QCOMPARE(shared.toPlainText(), QStringLiteral("甲乙丙"));
    QVERIFY(QMetaObject::invokeMethod(second, "redo", Qt::DirectConnection));
    QCOMPARE(shared.toPlainText(), QStringLiteral("甲乙丙丁"));
}

void QtDocumentSpikeTests::syntheticImeCompositionDefersTransfer()
{
    QTextDocument shared;
    QQmlEngine engine;
    QQuickWindow standalone;
    QQuickWindow workspace;
    standalone.create();
    workspace.create();
    QQuickItem *first = makeEditor(engine, standalone);
    QQuickItem *second = makeEditor(engine, workspace);
    QVERIFY(first);
    QVERIFY(second);
    QVERIFY(quickDocument(first));
    quickDocument(first)->setTextDocument(&shared);
    first->forceActiveFocus();

    QInputMethodEvent preedit(QStringLiteral("pin"), {});
    QCoreApplication::sendEvent(first, &preedit);
    QVERIFY(first->property("inputMethodComposing").toBool());
    QVERIFY(!transferDocument(first, second));

    QInputMethodEvent commit;
    commit.setCommitString(QStringLiteral("拼"));
    QCoreApplication::sendEvent(first, &commit);
    QVERIFY(!first->property("inputMethodComposing").toBool());
    QCOMPARE(shared.toPlainText(), QStringLiteral("拼"));
    QVERIFY(transferDocument(first, second));
    QCOMPARE(quickDocument(second)->textDocument(), &shared);
}

void QtDocumentSpikeTests::quickEditorAccessibilityWithExplicitRole()
{
    QAccessible::setActive(true);
    QQmlEngine engine;
    QQuickWindow window;
    window.create();
    QQuickItem *unannotated = makeEditor(engine, window);
    QVERIFY(unannotated);
    // Record the current fixture's missing role rather than treating it as an
    // accessible editor merely because it can receive keyboard input.
    QVERIFY(!QAccessible::queryAccessibleInterface(unannotated));
    QQuickItem *editor = makeEditor(engine, window, true);
    QVERIFY(editor);
    const QString sample = QStringLiteral("读写🙂 sample");
    QVERIFY(editor->setProperty("text", sample));
    QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(editor);
    QVERIFY(accessible);
    QVERIFY(accessible->isValid());
    QCOMPARE(accessible->role(), QAccessible::EditableText);
    QAccessibleTextInterface *text = accessible->textInterface();
    QVERIFY(text);
    QCOMPARE(text->text(0, text->characterCount()), sample);
    QCOMPARE(accessible->text(QAccessible::Value), sample);
    // Qt 6.8 exposes a whole-value setter here, not this separate interface.
    QVERIFY(!accessible->editableTextInterface());
    QVERIFY(quickDocument(editor));
    QTextDocument *document = quickDocument(editor)->textDocument();
    QVERIFY(document);
    QVERIFY(QMetaObject::invokeMethod(editor, "insert", Qt::DirectConnection,
                                     Q_ARG(int, sample.size()), Q_ARG(QString, QStringLiteral("尾"))));
    QVERIFY(document->isUndoAvailable());
    QVERIFY(editor->setProperty("readOnly", true));
    QVERIFY(accessible->state().readOnly);
    // This direct Qt API call intentionally records the candidate's behavior:
    // it bypasses this fixture's read-only property and clears the undo stack.
    const QString replacement = QStringLiteral("修改后的文字");
    accessible->setText(QAccessible::Value, replacement);
    QCOMPARE(editor->property("text").toString(), replacement);
    QVERIFY(!document->isUndoAvailable());
}

void QtDocumentSpikeTests::quickAccessibilityInterfacePersistsAfterDocumentReplacement()
{
    QAccessible::setActive(true);
    QTextDocument replacement;
    QQmlEngine engine;
    QQuickWindow window;
    window.create();
    QQuickItem *editor = makeEditor(engine, window, true);
    QVERIFY(editor);
    QQuickTextDocument *quick = quickDocument(editor);
    QVERIFY(quick);
    QPointer<QTextDocument> original = quick->textDocument();
    QVERIFY(original);
    QAccessibleInterface *before = QAccessible::queryAccessibleInterface(editor);
    QVERIFY(before);
    QVERIFY(before->textInterface());

    quick->setTextDocument(&replacement);
    QCOMPARE(quick->textDocument(), &replacement);
    QVERIFY(original.isNull());
    QAccessibleInterface *after = QAccessible::queryAccessibleInterface(editor);
    QCOMPARE(after, before);
    // Do not dereference the cached text interface after replacement: in the
    // Windows Qt 6.8.2 experiment this caused an access violation. Upstream
    // caches a document in the accessible object; this check records the
    // observable preconditions without intentionally triggering a crash.
}

void QtDocumentSpikeTests::singleEditorReparentPreservesAccessibilityAndUndo_data()
{
    QTest::addColumn<bool>("reverse");
    QTest::newRow("forward selection") << false;
    QTest::newRow("reverse selection") << true;
}

void QtDocumentSpikeTests::singleEditorReparentPreservesAccessibilityAndUndo()
{
    QFETCH(bool, reverse);
    QAccessible::setActive(true);
    QQmlEngine engine;
    auto source = std::make_unique<QQuickWindow>();
    auto destination = std::make_unique<QQuickWindow>();
    source->create();
    destination->create();
    QVERIFY(source->handle());
    QVERIFY(destination->handle());
    QQuickItem *editor = makeEditor(engine, *source, true);
    QVERIFY(editor);
    // Its QObject owner must outlive both window content items.
    editor->setParent(&engine);
    QPointer<QQuickItem> alive = editor;
    const QString sample = QStringLiteral("甲🙂乙");
    QVERIFY(editor->setProperty("text", sample));
    QQuickTextDocument *quick = quickDocument(editor);
    QVERIFY(quick);
    QPointer<QTextDocument> document = quick->textDocument();
    QVERIFY(document);
    QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(editor);
    QVERIFY(accessible);
    QVERIFY(accessible->textInterface());
    QCOMPARE(accessible->textInterface()->text(0, accessible->textInterface()->characterCount()), sample);

    QVERIFY(QMetaObject::invokeMethod(editor, "insert", Qt::DirectConnection,
                                     Q_ARG(int, sample.size()), Q_ARG(QString, QStringLiteral("丁"))));
    QVERIFY(document->isUndoAvailable());
    QVERIFY(QMetaObject::invokeMethod(editor, "select", Qt::DirectConnection,
                                     Q_ARG(int, reverse ? 3 : 1), Q_ARG(int, reverse ? 1 : 3)));
    const int start = editor->property("selectionStart").toInt();
    const int end = editor->property("selectionEnd").toInt();
    const int cursor = editor->property("cursorPosition").toInt();
    QCOMPARE(cursor, reverse ? start : end);

    QVERIFY(reparentEditorFixture(editor, destination.get()));
    QCOMPARE(editor->window(), destination.get());
    QCOMPARE(editor->property("selectionStart").toInt(), start);
    QCOMPARE(editor->property("selectionEnd").toInt(), end);
    QCOMPARE(editor->property("cursorPosition").toInt(), cursor);
    QCOMPARE(quick->textDocument(), document.data());
    QCOMPARE(QAccessible::queryAccessibleInterface(editor), accessible);
    QCOMPARE(accessible->window(), destination.get());
    QCOMPARE(accessible->textInterface()->text(0, accessible->textInterface()->characterCount()),
             sample + QStringLiteral("丁"));
    source.reset();
    QVERIFY(alive);
    QVERIFY(document);
    QVERIFY(QMetaObject::invokeMethod(editor, "undo", Qt::DirectConnection));
    QCOMPARE(accessible->textInterface()->text(0, accessible->textInterface()->characterCount()), sample);
    QVERIFY(QMetaObject::invokeMethod(editor, "redo", Qt::DirectConnection));
    QCOMPARE(accessible->textInterface()->text(0, accessible->textInterface()->characterCount()),
             sample + QStringLiteral("丁"));

    auto next = std::make_unique<QQuickWindow>();
    next->create();
    QVERIFY(next->handle());
    QVERIFY(editor->setProperty("cursorPosition", sample.size() + 1));
    editor->forceActiveFocus();
    QInputMethodEvent preedit(QStringLiteral("pin"), {});
    QCoreApplication::sendEvent(editor, &preedit);
    QVERIFY(editor->property("inputMethodComposing").toBool());
    QVERIFY(!reparentEditorFixture(editor, next.get()));
    QCOMPARE(editor->window(), destination.get());
    QInputMethodEvent commit;
    commit.setCommitString(QStringLiteral("拼"));
    QCoreApplication::sendEvent(editor, &commit);
    QVERIFY(!editor->property("inputMethodComposing").toBool());
    QVERIFY(reparentEditorFixture(editor, next.get()));
    QCOMPARE(quick->textDocument(), document.data());
    QCOMPARE(QAccessible::queryAccessibleInterface(editor), accessible);
    QCOMPARE(accessible->window(), next.get());
    QCOMPARE(accessible->textInterface()->text(0, accessible->textInterface()->characterCount()),
             sample + QStringLiteral("丁拼"));
}

void QtDocumentSpikeTests::previewParsesMarkdownAndBlocksResources()
{
    RestrictedPreview preview;
    preview.render(QStringLiteral(
        "# 标题\n\n- [x] 完成\n- [ ] 待办\n\n"
        "| A | B |\n| --- | --- |\n| 1 | 2 |\n\n"
        "```cpp\nint x = 1;\n```\n\n<script>alert(1)</script>\n\n"
        "![远程](https://example.invalid/asset.png)\n"));

    QCOMPARE(preview.begin().blockFormat().headingLevel(), 1);
    int checked = 0;
    int unchecked = 0;
    for (QTextBlock block = preview.begin(); block.isValid(); block = block.next()) {
        if (block.textList()) {
            checked += block.blockFormat().marker() == QTextBlockFormat::MarkerType::Checked;
            unchecked += block.blockFormat().marker() == QTextBlockFormat::MarkerType::Unchecked;
        }
    }
    QCOMPARE(checked, 1);
    QCOMPARE(unchecked, 1);
    bool hasTable = false;
    for (QTextFrame *frame : preview.rootFrame()->childFrames())
        hasTable = hasTable || qobject_cast<QTextTable *>(frame);
    QVERIFY(hasTable);
    QVERIFY(preview.toPlainText().contains(QStringLiteral("int x = 1;")));
    QVERIFY(!preview.toHtml().contains(QStringLiteral("<script"), Qt::CaseInsensitive));

    const QUrl remote(QStringLiteral("https://example.invalid/asset.png"));
    const QVariant image = preview.resource(QTextDocument::ImageResource, remote);
    QVERIFY(image.canConvert<QImage>());
    QVERIFY(preview.requestedResources().contains(remote.toString()));
}

void QtDocumentSpikeTests::markdownCorpusParsesBasicSemantics()
{
    QFile fixture(QStringLiteral(":/tests/fixtures/markdown-core.md"));
    QVERIFY(fixture.open(QIODevice::ReadOnly));
    RestrictedPreview preview;
    preview.render(QString::fromUtf8(fixture.readAll()));
    bool heading = false;
    bool strong = false;
    bool emphasis = false;
    bool quote = false;
    bool ordered = false;
    bool fencedCode = false;
    for (QTextBlock block = preview.begin(); block.isValid(); block = block.next()) {
        heading = heading || (block.text() == QStringLiteral("标题") && block.blockFormat().headingLevel() == 1);
        quote = quote || (block.text() == QStringLiteral("引用")
                          && block.blockFormat().property(QTextFormat::BlockQuoteLevel).toInt() == 1);
        ordered = ordered || (block.text() == QStringLiteral("第一") && block.textList()
                              && block.textList()->format().style() == QTextListFormat::ListDecimal);
        fencedCode = fencedCode || (block.text() == QStringLiteral("int x = 1;")
                                    && block.blockFormat().hasProperty(QTextFormat::BlockCodeFence));
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid())
                continue;
            strong = strong || (fragment.text() == QStringLiteral("加粗")
                                && fragment.charFormat().fontWeight() >= QFont::Bold);
            emphasis = emphasis || (fragment.text() == QStringLiteral("斜体")
                                    && fragment.charFormat().fontItalic());
        }
    }
    QVERIFY(heading);
    QVERIFY(strong);
    QVERIFY(emphasis);
    QVERIFY(quote);
    QVERIFY(ordered);
    QVERIFY(fencedCode);
}

void QtDocumentSpikeTests::markdownAnchorsRemainUntrusted()
{
    QFile fixture(QStringLiteral(":/tests/fixtures/markdown-core.md"));
    QVERIFY(fixture.open(QIODevice::ReadOnly));
    RestrictedPreview preview;
    preview.render(QString::fromUtf8(fixture.readAll()));
    QStringList anchors;
    for (QTextBlock block = preview.begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.isValid() && fragment.charFormat().isAnchor())
                anchors.append(fragment.charFormat().anchorHref());
        }
    }
    QVERIFY(anchors.contains(QStringLiteral("https://example.invalid/a")));
    // This records a parser boundary: a consumer must separately check the
    // scheme and user intent. This test does not activate or open any link.
    QVERIFY(anchors.contains(QStringLiteral("javascript:alert%281%29")));
    QVERIFY(preview.toPlainText().contains(QStringLiteral("<script>alert(1)</script>")));
    QVERIFY(!preview.toHtml().contains(QStringLiteral("<script"), Qt::CaseInsensitive));
}

void QtDocumentSpikeTests::previewUsesPlaceholdersForLocalAndRemoteImages()
{
    auto temporary = fixtureDirectory();
    QVERIFY(temporary);
    const QString imagePath = temporary->filePath(QStringLiteral("本地图.png"));
    QImage fixture(2, 2, QImage::Format_ARGB32_Premultiplied);
    fixture.fill(Qt::red);
    QVERIFY(fixture.save(imagePath, "PNG"));
    const QUrl local = QUrl::fromLocalFile(imagePath);
    const QUrl remote(QStringLiteral("https://example.invalid/asset.png"));
    RestrictedPreview preview;
    preview.render(QStringLiteral("![local](%1) ![remote](%2)")
                       .arg(local.toString(QUrl::FullyEncoded), remote.toString(QUrl::FullyEncoded)));
    for (const QUrl &url : {local, remote}) {
        const QVariant resource = preview.resource(QTextDocument::ImageResource, url);
        QVERIFY(resource.canConvert<QImage>());
        const QImage placeholder = qvariant_cast<QImage>(resource);
        QCOMPARE(placeholder.size(), QSize(1, 1));
        QCOMPARE(placeholder.pixelColor(0, 0).alpha(), 0);
        QVERIFY(preview.requestedResources().contains(url.toString()));
    }
}

void QtDocumentSpikeTests::imageReferenceLexicalCandidate_data()
{
    QTest::addColumn<QByteArray>("reference");
    QTest::addColumn<bool>("accepted");
    QTest::addColumn<QStringList>("expectedComponents");
    const auto allow = [](const char *name, const QByteArray &reference, const QStringList &parts) {
        QTest::newRow(name) << reference << true << parts;
    };
    const auto reject = [](const char *name, const QByteArray &reference) {
        QTest::newRow(name) << reference << false << QStringList{};
    };
    allow("relative", "images/cover.png", {QStringLiteral("images"), QStringLiteral("cover.png")});
    allow("dot component", "./images/./cover.png", {QStringLiteral("images"), QStringLiteral("cover.png")});
    allow("unicode", "images/%E5%9B%BE.png", {QStringLiteral("images"), QStringLiteral("图.png")});
    allow("space and hash", "images/a%20%23b.png", {QStringLiteral("images"), QStringLiteral("a #b.png")});
    allow("literal percent", "images/100%25.png", {QStringLiteral("images"), QStringLiteral("100%.png")});
    allow("decode once", "images/%252e%252e.png", {QStringLiteral("images"), QStringLiteral("%2e%2e.png")});
    reject("empty", "");
    reject("remote", "https://example.invalid/image.png");
    reject("remote authority", "//example.invalid/image.png");
    reject("file scheme", "file:///tmp/image.png");
    reject("embedded data", "data:image/png;base64,AA==");
    reject("absolute path", "/images/cover.png");
    reject("parent", "../outside.png");
    reject("encoded parent", "images/%2e%2e/outside.png");
    reject("mixed encoded parent", "images/%2e./outside.png");
    reject("encoded separators", "images%2f..%2foutside.png");
    reject("backslash", "images\\..\\outside.png");
    reject("encoded backslash", "images%5c..%5coutside.png");
    reject("drive absolute", "C:/outside.png");
    reject("drive relative", "C:outside.png");
    reject("encoded drive", "C%3Aoutside.png");
    reject("alternate stream", "images/cover.png%3Asecret");
    reject("empty component", "images//cover.png");
    reject("trailing separator", "images/");
    reject("query", "images/cover.png?x=1");
    reject("empty query", "images/cover.png?");
    reject("fragment", "images/cover.png#part");
    reject("nul", "images/%00.png");
    reject("control", "images/%1f.png");
    reject("invalid utf8", "images/%FF.png");
    reject("truncated utf8", "images/%E5%9B.png");
    reject("invalid escape", "images/%ZZ.png");
    reject("device with extension", "images/CON.png");
    reject("device with case", "images/aux.txt");
    reject("device with superscript", "images/com%C2%B9.png");
    reject("trailing dot", "images/cover.png.");
    reject("trailing space", "images/cover.png%20");
    reject("encoded wildcard", "images/cover%3F.png");
}

void QtDocumentSpikeTests::imageReferenceLexicalCandidate()
{
    QFETCH(QByteArray, reference);
    QFETCH(bool, accepted);
    QFETCH(QStringList, expectedComponents);
    const auto components = candidateImageReference(reference);
    QCOMPARE(components.has_value(), accepted);
    if (components)
        QCOMPARE(*components, expectedComponents);
}

void QtDocumentSpikeTests::markdownImageNamesStayBlocked()
{
    RestrictedPreview preview;
    preview.render(QStringLiteral("![local](./images/%E5%9B%BE.png)\n\n"
                                  "![parent](../outside.png)\n\n"
                                  "![encoded](images/%2e%2e/outside.png)\n\n"
                                  "![remote](https://example.invalid/cover.png)"));
    QStringList names;
    for (QTextBlock block = preview.begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.isValid() && fragment.charFormat().isImageFormat())
                names.append(fragment.charFormat().toImageFormat().name());
        }
    }
    QCOMPARE(names.size(), 4);
    const auto local = candidateImageReference(names.at(0).toUtf8());
    QVERIFY(local);
    QCOMPARE(*local, (QStringList{QStringLiteral("images"), QStringLiteral("图.png")}));
    QVERIFY(!candidateImageReference(names.at(1).toUtf8()));
    QVERIFY(!candidateImageReference(names.at(2).toUtf8()));
    QVERIFY(!candidateImageReference(names.at(3).toUtf8()));
    for (const QString &name : names) {
        const QVariant resource = preview.resource(QTextDocument::ImageResource, QUrl(name));
        QVERIFY(resource.canConvert<QImage>());
        const QImage placeholder = qvariant_cast<QImage>(resource);
        QCOMPARE(placeholder.size(), QSize(1, 1));
        QCOMPARE(placeholder.pixelColor(0, 0).alpha(), 0);
    }
}

void QtDocumentSpikeTests::imageReferenceCanEscapeThroughSymlink()
{
#ifdef Q_OS_WIN
    QSKIP("This counterexample uses a POSIX symbolic link in the test fixture.");
#else
    auto temporary = fixtureDirectory();
    QVERIFY(temporary);
    const QString workspace = temporary->filePath(QStringLiteral("workspace"));
    QVERIFY(QDir().mkpath(workspace + QStringLiteral("/images")));
    const QString outside = temporary->filePath(QStringLiteral("outside.bin"));
    QVERIFY(writeFile(outside, QByteArray("fixture outside workspace")));
    const QString link = workspace + QStringLiteral("/images/link.png");
    QCOMPARE(::symlink("../../outside.bin", QFile::encodeName(link).constData()), 0);
    const auto removeLink = qScopeGuard([&link] { QFile::remove(link); });
    const auto components = candidateImageReference(QByteArray("images/link.png"));
    QVERIFY(components); // A lexical check cannot see where the link leads.
    QCOMPARE(QFileInfo(link).canonicalFilePath(), QFileInfo(outside).canonicalFilePath());
    QFile followed(link);
    QVERIFY(followed.open(QIODevice::ReadOnly));
    QCOMPARE(followed.readAll(), QByteArray("fixture outside workspace"));
#endif
}

void QtDocumentSpikeTests::previewLeavesSourceUndoIndependent()
{
    QTextDocument source;
    source.setPlainText(QStringLiteral("原文"));
    source.clearUndoRedoStacks();
    QTextCursor cursor(&source);
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QStringLiteral("新增"));
    QVERIFY(source.isUndoAvailable());

    RestrictedPreview preview;
    preview.render(source.toPlainText());
    QCOMPARE(preview.toPlainText(), QStringLiteral("原文新增"));
    QVERIFY(source.isUndoAvailable());
    source.undo();
    QCOMPARE(source.toPlainText(), QStringLiteral("原文"));
    preview.render(source.toPlainText());
    QCOMPARE(preview.toPlainText(), QStringLiteral("原文"));
}

void QtDocumentSpikeTests::openIntentFramesHandlePartialAndInvalidInput()
{
    const QJsonObject request{{QStringLiteral("path"), QStringLiteral("C:/笔记 空格.md")}};
    const QByteArray frame = SpikeIpc::encode(request);
    QVERIFY(!frame.isEmpty());
    QByteArray input = frame.left(3);
    QJsonObject decoded;
    QString error;
    QVERIFY(SpikeIpc::takeFrame(&input, &decoded, &error) == SpikeIpc::DecodeResult::NeedMore);
    input += frame.mid(3, 2);
    QVERIFY(SpikeIpc::takeFrame(&input, &decoded, &error) == SpikeIpc::DecodeResult::NeedMore);
    input += frame.mid(5);
    input += frame;
    QVERIFY(SpikeIpc::takeFrame(&input, &decoded, &error) == SpikeIpc::DecodeResult::Ready);
    QCOMPARE(decoded, request);
    QVERIFY(SpikeIpc::takeFrame(&input, &decoded, &error) == SpikeIpc::DecodeResult::Ready);
    QCOMPARE(decoded, request);
    QVERIFY(input.isEmpty());

    QByteArray oversized(4, '\0');
    qToBigEndian<quint32>(SpikeIpc::maxPayloadBytes + 1, reinterpret_cast<uchar *>(oversized.data()));
    QVERIFY(SpikeIpc::takeFrame(&oversized, &decoded, &error) == SpikeIpc::DecodeResult::Invalid);
    QCOMPARE(oversized.size(), 4);
    QByteArray nonObject = QByteArray::fromHex("000000025b5d");
    QVERIFY(SpikeIpc::takeFrame(&nonObject, &decoded, &error) == SpikeIpc::DecodeResult::Invalid);
}

void QtDocumentSpikeTests::openIntentMovesBetweenRealLocalProcesses()
{
    auto temporary = fixtureDirectory();
    QVERIFY(temporary);
    const QString path = temporary->filePath(QStringLiteral("双击 笔记.md"));
    QVERIFY(writeFile(path, "# fixture\n"));

#ifdef Q_OS_UNIX
    // QLocalServer expands a relative name under TMPDIR; on macOS that path can
    // exceed sockaddr_un::sun_path. A private short directory also protects
    // access on systems that do not enforce socket-file permissions.
    QTemporaryDir socketDirectory(QStringLiteral("/tmp/xdnote-ipc-XXXXXX"));
    QVERIFY2(socketDirectory.isValid(), qPrintable(socketDirectory.errorString()));
    const auto permissions = QFileInfo(socketDirectory.path()).permissions();
    QVERIFY(!(permissions & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
                              | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther)));
    const QString endpoint = socketDirectory.filePath(QStringLiteral("socket"));
    QVERIFY(QFile::encodeName(endpoint).size() < 104);
#else
    const QString endpoint = QStringLiteral("xdnote-spike-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
#endif
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    QVERIFY2(server.listen(endpoint), qPrintable(server.errorString()));

    QString sender = QCoreApplication::applicationDirPath() + QStringLiteral("/qt_open_intent_sender");
#ifdef Q_OS_WIN
    sender += QStringLiteral(".exe");
#endif
    QProcess child;
    child.setProgram(sender);
    child.setArguments({endpoint, path});
    child.start();
    QVERIFY2(child.waitForStarted(5000), qPrintable(child.errorString()));
    QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 5000);
    std::unique_ptr<QLocalSocket> socket(server.nextPendingConnection());
    QVERIFY(socket);

    QByteArray input;
    QJsonObject request;
    QString error;
    QElapsedTimer deadline;
    deadline.start();
    auto state = SpikeIpc::DecodeResult::NeedMore;
    while (state == SpikeIpc::DecodeResult::NeedMore && deadline.elapsed() < 5000) {
        if (socket->bytesAvailable() == 0)
            QVERIFY2(socket->waitForReadyRead(int(5000 - deadline.elapsed())), qPrintable(socket->errorString()));
        input += socket->readAll();
        state = SpikeIpc::takeFrame(&input, &request, &error);
    }
    QVERIFY2(state == SpikeIpc::DecodeResult::Ready, qPrintable(error));
    QCOMPARE(request.value(QStringLiteral("version")).toInt(), 1);
    QCOMPARE(request.value(QStringLiteral("path")).toString(), path);
    const QString id = request.value(QStringLiteral("requestId")).toString();
    QVERIFY(!id.isEmpty());
    const QByteArray reply = SpikeIpc::encode({{QStringLiteral("version"), 1},
                                               {QStringLiteral("requestId"), id},
                                               {QStringLiteral("accepted"), true}});
    QCOMPARE(socket->write(reply), qint64(reply.size()));
    if (socket->bytesToWrite() > 0)
        QVERIFY2(socket->waitForBytesWritten(5000), qPrintable(socket->errorString()));
    QVERIFY2(child.waitForFinished(5000), qPrintable(child.errorString()));
    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    QCOMPARE(child.exitCode(), 0);
}

void QtDocumentSpikeTests::utf8BomAndCrlfRoundTrip()
{
    auto temporary = fixtureDirectory();
    QVERIFY(temporary);
    const QString path = temporary->filePath(QStringLiteral("笔记 空格.md"));
    const QByteArray original = QByteArray::fromHex("efbbbf")
        + QStringLiteral("# 标题\r\n第一行\r\n\r\n").toUtf8();
    QVERIFY(writeFile(path, original));
    SpikeSession session;
    QString error;
    QVERIFY2(session.open(path, &error), qPrintable(error));
    QCOMPARE(session.document()->toPlainText(), QStringLiteral("# 标题\n第一行\n\n"));
    QVERIFY(!session.isDirty());
    QVERIFY(session.save({}, &error) == SpikeFile::SaveOutcome::Saved);
    QCOMPARE(readFile(path), original);

    QTextCursor cursor(session.document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QStringLiteral("新增\n"));
    QVERIFY(session.isDirty());
    QVERIFY2(session.save({}, &error) == SpikeFile::SaveOutcome::Saved, qPrintable(error));
    const QByteArray expected = QByteArray::fromHex("efbbbf")
        + QStringLiteral("# 标题\r\n第一行\r\n\r\n新增\r\n").toUtf8();
    QCOMPARE(readFile(path), expected);
    QVERIFY(!session.isDirty());
}

void QtDocumentSpikeTests::nonBreakingSpaceIsNotRewritten()
{
    auto temporary = fixtureDirectory();
    QVERIFY(temporary);
    const QString path = temporary->filePath(QStringLiteral("nbsp.md"));
    const QByteArray original = QStringLiteral("a\u00a0b\nc").toUtf8();
    QVERIFY(writeFile(path, original));
    SpikeSession session;
    QString error;
    QVERIFY2(session.open(path, &error), qPrintable(error));
    QVERIFY(!session.isDirty());
    QVERIFY2(session.save({}, &error) == SpikeFile::SaveOutcome::Saved, qPrintable(error));
    QCOMPARE(readFile(path), original);

    QTextCursor cursor(session.document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QStringLiteral("x"));
    QVERIFY(session.isDirty());
    QVERIFY2(session.save({}, &error) == SpikeFile::SaveOutcome::Saved, qPrintable(error));
    QCOMPARE(readFile(path), original + QByteArray("x"));
    QVERIFY(!session.isDirty());
}

void QtDocumentSpikeTests::unsupportedUnicodeSeparatorsDoNotOverwrite()
{
    auto temporary = fixtureDirectory();
    QVERIFY(temporary);
    QString error;
    QString text;
    for (ushort separator : {ushort(0x2028), ushort(0x2029)}) {
        const QString path = temporary->filePath(QString::number(separator) + QStringLiteral(".md"));
        const QByteArray original = (QStringLiteral("a") + QChar(separator) + QStringLiteral("b")).toUtf8();
        QVERIFY(writeFile(path, original));
        SpikeFile file;
        QVERIFY(!file.open(path, &text, &error));
        QCOMPARE(readFile(path), original);
    }

    const QString path = temporary->filePath(QStringLiteral("edited.md"));
    QVERIFY(writeFile(path, "original"));
    SpikeSession session;
    QVERIFY2(session.open(path, &error), qPrintable(error));
    QTextCursor cursor(session.document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QString(QChar(0x2028)));
    QVERIFY(session.isDirty());
    QVERIFY(session.save({}, &error) == SpikeFile::SaveOutcome::FailedBeforeCommit);
    QCOMPARE(readFile(path), QByteArray("original"));
}

void QtDocumentSpikeTests::paragraphSeparatorBecomesIndistinguishableFromNewline()
{
    // This records a loss of information in the candidate text model. It is
    // not a desired product behavior or a license to normalize user input.
    QTextDocument newlineDocument;
    QTextDocument separatorDocument;
    QTextCursor newlineCursor(&newlineDocument);
    QTextCursor separatorCursor(&separatorDocument);
    newlineCursor.insertText(QStringLiteral("a\nb"));
    separatorCursor.insertText(QStringLiteral("a\u2029b"));
    QCOMPARE(newlineDocument.toRawText(), QStringLiteral("a\u2029b"));
    QCOMPARE(separatorDocument.toRawText(), newlineDocument.toRawText());
    QCOMPARE(separatorDocument.toPlainText(), newlineDocument.toPlainText());
}

void QtDocumentSpikeTests::rejectsInvalidUtf8AndMixedLineEndings()
{
    auto temporary = fixtureDirectory();
    QVERIFY(temporary);
    const QString invalidPath = temporary->filePath(QStringLiteral("invalid.md"));
    const QString mixedPath = temporary->filePath(QStringLiteral("mixed.md"));
    QVERIFY(writeFile(invalidPath, QByteArray::fromHex("fffe4100")));
    QVERIFY(writeFile(mixedPath, QByteArray("one\r\ntwo\n")));
    SpikeFile file;
    QString text;
    QString error;
    QVERIFY(!file.open(invalidPath, &text, &error));
    QCOMPARE(error, QStringLiteral("Invalid UTF-8"));
    QVERIFY(!file.open(mixedPath, &text, &error));
    QCOMPARE(error, QStringLiteral("Mixed or unsupported line endings"));
}

void QtDocumentSpikeTests::externalChangeBeforeSaveIsConflict()
{
    auto temporary = fixtureDirectory();
    QVERIFY(temporary);
    const QString path = temporary->filePath(QStringLiteral("note.md"));
    QVERIFY(writeFile(path, "original\n"));
    SpikeSession session;
    QString error;
    QVERIFY2(session.open(path, &error), qPrintable(error));
    QTextCursor cursor(session.document());
    cursor.insertText(QStringLiteral("local "));
    QVERIFY(writeFile(path, "external\n"));
    QVERIFY(session.save({}, &error) == SpikeFile::SaveOutcome::Conflict);
    QCOMPARE(readFile(path), QByteArray("external\n"));
    QVERIFY(session.isDirty());
}

void QtDocumentSpikeTests::externalChangeBeforeCommitIsConflict()
{
    auto temporary = fixtureDirectory();
    QVERIFY(temporary);
    const QString path = temporary->filePath(QStringLiteral("note.md"));
    QVERIFY(writeFile(path, "original\n"));
    SpikeSession session;
    QString error;
    QVERIFY2(session.open(path, &error), qPrintable(error));
    QTextCursor cursor(session.document());
    cursor.insertText(QStringLiteral("local "));
    bool injected = false;
    const auto result = session.save([&] { injected = writeFile(path, "external\n"); }, &error);
    QVERIFY(injected);
    QVERIFY(result == SpikeFile::SaveOutcome::Conflict);
    QCOMPARE(readFile(path), QByteArray("external\n"));
    QVERIFY(session.isDirty());
}

void QtDocumentSpikeTests::identicalBytesReplacementIsNotDetected()
{
    // Keep the original file under a different name so the replacement is a
    // distinct object, even though the bytes at the original path are equal.
    auto temporary = fixtureDirectory();
    QVERIFY(temporary);
    const QString path = temporary->filePath(QStringLiteral("note.md"));
    const QString oldPath = temporary->filePath(QStringLiteral("old-note.md"));
    QVERIFY(writeFile(path, "original"));
    SpikeSession session;
    QString error;
    QVERIFY2(session.open(path, &error), qPrintable(error));
    QTextCursor cursor(session.document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QStringLiteral(" local"));
    QVERIFY(QFile::rename(path, oldPath));
    QVERIFY(writeFile(path, "original"));

    // The experimental byte-only baseline misses this replacement.
    QVERIFY2(session.save({}, &error) == SpikeFile::SaveOutcome::Saved, qPrintable(error));
    QCOMPARE(readFile(path), QByteArray("original local"));
    QCOMPARE(readFile(oldPath), QByteArray("original"));
}

void QtDocumentSpikeTests::fileIdentityHandlesObserveAliasesAndReplacement()
{
    auto temporary = fixtureDirectory();
    QVERIFY(temporary);
    const QString path = temporary->filePath(QStringLiteral("笔记 空格.md"));
    const QString alias = temporary->filePath(QStringLiteral("硬链接.md"));
    const QString renamed = temporary->filePath(QStringLiteral("原文件.md"));
    const QByteArray bytes("same bytes");
    QVERIFY(writeFile(path, bytes));

    QString error;
    FixtureIdentityHandle original;
    FixtureFileIdentity originalId;
    QVERIFY2(original.open(path, &error), qPrintable(error));
    QVERIFY2(original.identity(&originalId, &error), qPrintable(error));
    QVERIFY2(createHardLink(path, alias), "Could not create a fixture hard link");
    FixtureIdentityHandle aliased;
    FixtureFileIdentity aliasId;
    QVERIFY2(aliased.open(alias, &error), qPrintable(error));
    QVERIFY2(aliased.identity(&aliasId, &error), qPrintable(error));
    QVERIFY(originalId == aliasId);

    QVERIFY(QFile::rename(path, renamed));
    FixtureIdentityHandle renamedHandle;
    FixtureFileIdentity renamedId;
    QVERIFY2(renamedHandle.open(renamed, &error), qPrintable(error));
    QVERIFY2(renamedHandle.identity(&renamedId, &error), qPrintable(error));
    QVERIFY(renamedId == originalId);
    QVERIFY(writeFile(path, bytes));
    FixtureIdentityHandle replacement;
    FixtureFileIdentity replacementId;
    QVERIFY2(replacement.open(path, &error), qPrintable(error));
    QVERIFY2(replacement.identity(&replacementId, &error), qPrintable(error));
    QVERIFY(!(originalId == replacementId));
    QCOMPARE(readFile(path), readFile(alias));
    FixtureFileIdentity heldId;
    QVERIFY2(original.identity(&heldId, &error), qPrintable(error));
    QVERIFY(heldId == aliasId);
}

void QtDocumentSpikeTests::fileIdentityProbeRejectsUnsupportedTargets()
{
    auto temporary = fixtureDirectory();
    QVERIFY(temporary);
    const QString path = temporary->filePath(QStringLiteral("original.md"));
    QVERIFY(writeFile(path, "fixture"));
    QString error;
    FixtureIdentityHandle absent;
    FixtureFileIdentity invalidId;
    QVERIFY(!absent.identity(&invalidId, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!absent.open(temporary->filePath(QStringLiteral("missing.md")), &error));
    QVERIFY(!error.isEmpty());
    FixtureIdentityHandle directory;
    QVERIFY(!directory.open(temporary->path(), &error));
    QVERIFY(!error.isEmpty());
    FixtureIdentityHandle relative;
    QVERIFY(!relative.open(QStringLiteral("relative.md"), &error));
#ifdef Q_OS_UNIX
    const QString symbolic = temporary->filePath(QStringLiteral("symbolic.md"));
    QVERIFY(::symlink(QFile::encodeName(path).constData(), QFile::encodeName(symbolic).constData()) == 0);
    FixtureIdentityHandle link;
    QVERIFY(!link.open(symbolic, &error));
    QVERIFY(!error.isEmpty());
#endif
}

void QtDocumentSpikeTests::changeAfterFinalCheckCanBeOverwritten()
{
    // Reproduce the candidate's check-then-commit sequence on a test-owned
    // file. QSaveFile's atomic replacement does not make the preceding check
    // conditional on the target remaining unchanged.
    auto temporary = fixtureDirectory();
    QVERIFY(temporary);
    const QString path = temporary->filePath(QStringLiteral("note.md"));
    const QByteArray baseline("original");
    QVERIFY(writeFile(path, baseline));
#ifdef Q_OS_WIN
    const int entriesBeforeOpen = fixtureOtherEntryCount(temporary->path());
    QVERIFY(entriesBeforeOpen >= 0);
#endif
    QSaveFile output(path);
    output.setDirectWriteFallback(false);
    QVERIFY2(output.open(QIODevice::WriteOnly), qPrintable(output.errorString()));
#ifdef Q_OS_WIN
    const int entriesAfterOpen = fixtureOtherEntryCount(temporary->path());
    QVERIFY(entriesAfterOpen >= 0);
#endif
    QCOMPARE(output.write("local"), qint64(5));
    QCOMPARE(readFile(path), baseline); // Last comparison in the candidate sequence.
    QVERIFY(writeFile(path, "external")); // Another actor writes after that comparison.
    const bool committed = output.commit();
    const QString commitError = QStringLiteral("QSaveFile::commit (Qt file error %1): %2")
                                    .arg(int(output.error())).arg(output.errorString());
#ifdef Q_OS_WIN
    if (!committed) {
        const auto observation = observeFixtureCommitFailure(temporary->path());
        qWarning().noquote() << describeFixtureCommitFailure(observation, entriesBeforeOpen, entriesAfterOpen);
    }
#endif
    QVERIFY2(committed, qPrintable(commitError));
    QCOMPARE(readFile(path), QByteArray("local")); // The intervening update was lost.
}

void QtDocumentSpikeTests::fixtureWriterRejectsOutsideScope()
{
    auto temporary = fixtureDirectory();
    QVERIFY(temporary);
    const QString other = temporary->filePath(QStringLiteral("other.md"));
    QVERIFY(writeFile(other, "original"));
    for (const QStringList &arguments : {QStringList{other},
                                         QStringList{QStringLiteral("--hold-qt-read"), other},
                                         QStringList{QStringLiteral("--hold-shared-read"), other}}) {
        QProcess writer;
        writer.start(fixtureWriterPath(), arguments);
        QVERIFY2(writer.waitForStarted(5000), qPrintable(writer.errorString()));
        QVERIFY2(writer.waitForFinished(5000), qPrintable(writer.errorString()));
        QCOMPARE(writer.exitStatus(), QProcess::NormalExit);
        QCOMPARE(writer.exitCode(), 3);
    }
    QCOMPARE(readFile(other), QByteArray("original"));
}

void QtDocumentSpikeTests::readerHandleAndReplacement_data()
{
    QTest::addColumn<bool>("nativeSharedReader");
    QTest::newRow("Qt QFile reader") << false;
    QTest::newRow("native read handle") << true;
}

void QtDocumentSpikeTests::readerHandleAndReplacement()
{
    QFETCH(bool, nativeSharedReader);
    auto temporary = fixtureDirectory();
    QVERIFY(temporary);
    const QString path = temporary->filePath(QStringLiteral("note.md"));
    QVERIFY(writeFile(path, QByteArray("original")));
    SpikeSession session;
    QString error;
    QVERIFY2(session.open(path, &error), qPrintable(error));
    QTextCursor cursor(session.document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QStringLiteral(" local"));
    QVERIFY(session.isDirty());

    QProcess reader;
    const QString mode = nativeSharedReader ? QStringLiteral("--hold-shared-read")
                                            : QStringLiteral("--hold-qt-read");
    QVERIFY2(startFixtureProcess(&reader, {mode, path}, &error), qPrintable(error));
    const auto cleanupReader = qScopeGuard([&reader] {
        if (reader.state() != QProcess::NotRunning) {
            reader.closeWriteChannel();
            if (!reader.waitForFinished(5000)) {
                reader.kill();
                reader.waitForFinished(5000);
            }
        }
    });

#ifdef Q_OS_WIN
    // This is a separate native call with Qt's inspected replacement flag.
    // Its immediately captured error does not describe QSaveFile's later call.
    const QString prepared = temporary->filePath(QStringLiteral("prepared.md"));
    const QString backup = temporary->filePath(QStringLiteral("displaced.md"));
    QVERIFY(writeFile(prepared, QByteArray("candidate")));
    const QString nativePrepared = QDir::toNativeSeparators(prepared);
    const QString nativeTarget = QDir::toNativeSeparators(path);
    const QString nativeBackup = QDir::toNativeSeparators(backup);
    const BOOL moved = MoveFileExW(reinterpret_cast<LPCWSTR>(nativePrepared.utf16()),
                                   reinterpret_cast<LPCWSTR>(nativeTarget.utf16()),
                                   MOVEFILE_REPLACE_EXISTING);
    const DWORD moveError = moved ? ERROR_SUCCESS : GetLastError();
    QVERIFY(!moved);
    QVERIFY(moveError != ERROR_SUCCESS);
    qInfo() << "Separate MoveFileExW failure, native error:" << moveError;
    QCOMPARE(readFile(prepared), QByteArray("candidate"));
    QCOMPARE(readFile(path), QByteArray("original"));
#endif

#ifdef Q_OS_WIN
    const QString expectedObservation = nativeSharedReader
        ? QStringLiteral("fixture target=baseline;.*DELETE-access open=opened; its native error=0")
        : QStringLiteral("fixture target=baseline;.*DELETE-access open=failed; its native error=32");
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(expectedObservation));
#endif
    const auto result = session.save({}, &error);
#ifdef Q_OS_WIN
    QVERIFY2(result == SpikeFile::SaveOutcome::CommitUncertain, qPrintable(error));
    const QString prefix = QStringLiteral("commit (Qt file error %1):")
                               .arg(int(QFileDevice::RenameError));
    QVERIFY2(error.startsWith(prefix), qPrintable(error));
    QCOMPARE(readFile(path), QByteArray("original"));
    QVERIFY(session.isDirty());

    // Compare a different Windows primitive under the same held reader.
    const BOOL replaced = ReplaceFileW(reinterpret_cast<LPCWSTR>(nativeTarget.utf16()),
                                       reinterpret_cast<LPCWSTR>(nativePrepared.utf16()),
                                       reinterpret_cast<LPCWSTR>(nativeBackup.utf16()),
                                       0, nullptr, nullptr);
    const DWORD replaceError = replaced ? ERROR_SUCCESS : GetLastError();
    if (nativeSharedReader) {
        QVERIFY2(replaced, qPrintable(QStringLiteral("ReplaceFileW error %1").arg(replaceError)));
        QCOMPARE(readFile(path), QByteArray("candidate"));
        QCOMPARE(readFile(backup), QByteArray("original"));
    } else {
        QVERIFY(!replaced);
        QVERIFY(replaceError != ERROR_SUCCESS);
        qInfo() << "Separate ReplaceFileW failure, native error:" << replaceError;
        QCOMPARE(readFile(path), QByteArray("original"));
        QCOMPARE(readFile(prepared), QByteArray("candidate"));
    }
#else
    QVERIFY2(result == SpikeFile::SaveOutcome::Saved, qPrintable(error));
    QCOMPARE(readFile(path), QByteArray("original local"));
    QVERIFY(!session.isDirty());
#endif

    // The child reads from its previously opened handle, not by reopening the path.
    QVERIFY2(finishFixtureReader(&reader, &error), qPrintable(error));
#ifdef Q_OS_WIN
    if (!nativeSharedReader) {
        // This follows release of the deliberately held test handle.
        QVERIFY2(session.save({}, &error) == SpikeFile::SaveOutcome::Saved, qPrintable(error));
        QCOMPARE(readFile(path), QByteArray("original local"));
        QVERIFY(!session.isDirty());
    } else {
        // ReplaceFileW changed the target after the session's baseline: a later
        // save must report conflict rather than overwrite it.
        QVERIFY(session.save({}, &error) == SpikeFile::SaveOutcome::Conflict);
        QCOMPARE(readFile(path), QByteArray("candidate"));
        QCOMPARE(readFile(backup), QByteArray("original"));
        QVERIFY(session.isDirty());
    }
#endif
}

void QtDocumentSpikeTests::displacedFileRetainsInterveningWrite()
{
    auto temporary = fixtureDirectory();
    QVERIFY(temporary);
    const QString path = temporary->filePath(QStringLiteral("note.md"));
    const QString prepared = temporary->filePath(QStringLiteral("prepared.md"));
    const QString backup = temporary->filePath(QStringLiteral("displaced.md"));
    QVERIFY(writeFile(path, "original"));
    QVERIFY(writeFile(prepared, "local"));
    QCOMPARE(readFile(path), QByteArray("original")); // A prior comparison can become stale.

    QProcess writer;
    QString error;
    QVERIFY2(startFixtureWriter(&writer, path, &error), qPrintable(error));
    QVERIFY2(finishFixtureWriter(&writer, &error), qPrintable(error));
    QCOMPARE(readFile(path), QByteArray("external"));
    QString displaced;
    QVERIFY2(replaceAndKeepDisplaced(path, prepared, backup, &displaced, &error), qPrintable(error));
    QCOMPARE(readFile(path), QByteArray("local"));
    QCOMPARE(readFile(displaced), QByteArray("external"));
}

void QtDocumentSpikeTests::openWriterCanStillChangeDisplacedFile()
{
    auto temporary = fixtureDirectory();
    QVERIFY(temporary);
    const QString path = temporary->filePath(QStringLiteral("note.md"));
    const QString prepared = temporary->filePath(QStringLiteral("prepared.md"));
    const QString backup = temporary->filePath(QStringLiteral("displaced.md"));
    QVERIFY(writeFile(path, "original"));
    QVERIFY(writeFile(prepared, "local"));

    QProcess writer;
    QString error;
    QVERIFY2(startFixtureWriter(&writer, path, &error), qPrintable(error));
    QString displaced;
    QVERIFY2(replaceAndKeepDisplaced(path, prepared, backup, &displaced, &error), qPrintable(error));
    QCOMPARE(readFile(path), QByteArray("local"));
    QCOMPARE(readFile(displaced), QByteArray("original"));
    QVERIFY2(finishFixtureWriter(&writer, &error), qPrintable(error));
    // A process with a pre-existing handle can keep writing the displaced
    // object, so a post-swap snapshot is not automatically immutable.
    QCOMPARE(readFile(displaced), QByteArray("external"));
    QCOMPARE(readFile(path), QByteArray("local"));
}

void QtDocumentSpikeTests::missingTargetDoesNotReplacePreparedFile()
{
    auto temporary = fixtureDirectory();
    QVERIFY(temporary);
    const QString path = temporary->filePath(QStringLiteral("note.md"));
    const QString prepared = temporary->filePath(QStringLiteral("prepared.md"));
    const QString backup = temporary->filePath(QStringLiteral("displaced.md"));
    QVERIFY(writeFile(prepared, "local"));
    QString displaced;
    QString error;
    QVERIFY(!replaceAndKeepDisplaced(path, prepared, backup, &displaced, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readFile(prepared), QByteArray("local"));
    QVERIFY(!QFileInfo::exists(path));
}

void QtDocumentSpikeTests::editDuringSaveStaysDirty()
{
    auto temporary = fixtureDirectory();
    QVERIFY(temporary);
    const QString path = temporary->filePath(QStringLiteral("note.md"));
    QVERIFY(writeFile(path, "original"));
    SpikeSession session;
    QString error;
    QVERIFY2(session.open(path, &error), qPrintable(error));
    QTextCursor cursor(session.document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QStringLiteral(" first"));
    const auto result = session.save([&] {
        QTextCursor later(session.document());
        later.movePosition(QTextCursor::End);
        later.insertText(QStringLiteral(" later"));
    }, &error);
    QVERIFY2(result == SpikeFile::SaveOutcome::Saved, qPrintable(error));
    QCOMPARE(readFile(path), QByteArray("original first"));
    QCOMPARE(session.document()->toPlainText(), QStringLiteral("original first later"));
    QVERIFY(session.isDirty());
    QVERIFY2(session.save({}, &error) == SpikeFile::SaveOutcome::Saved, qPrintable(error));
    QCOMPARE(readFile(path), QByteArray("original first later"));
    QVERIFY(!session.isDirty());
}

void QtDocumentSpikeTests::deletedTargetIsNotRecreated()
{
    auto temporary = fixtureDirectory();
    QVERIFY(temporary);
    const QString path = temporary->filePath(QStringLiteral("note.md"));
    QVERIFY(writeFile(path, "original"));
    SpikeSession session;
    QString error;
    QVERIFY2(session.open(path, &error), qPrintable(error));
    QTextCursor cursor(session.document());
    cursor.insertText(QStringLiteral("local "));
    QVERIFY(QFile::remove(path));
    QVERIFY(session.save({}, &error) == SpikeFile::SaveOutcome::Conflict);
    QVERIFY(!QFileInfo::exists(path));
    QVERIFY(session.isDirty());
}

#ifdef Q_OS_WIN
void QtDocumentSpikeTests::windowsSaveFailureObservations()
{
    auto temporary = fixtureDirectory();
    QVERIFY(temporary);
    const QString path = temporary->filePath(QStringLiteral("note.md"));
    QVERIFY(writeFile(path, QByteArray("original")));
    QVERIFY(!observeFixtureCommitFailure(temporary->filePath(QStringLiteral("not-a-fixture"))).valid);
    const auto initial = observeFixtureCommitFailure(temporary->path());
    QVERIFY(initial.valid);
    QCOMPARE(initial.targetState, QStringLiteral("original"));
    QCOMPARE(initial.otherEntries, 0);
    QVERIFY(initial.deleteOpenAttempted);
    QVERIFY(initial.deleteOpenSucceeded);

    QSaveFile output(path);
    output.setDirectWriteFallback(false);
    const int entriesBeforeOpen = fixtureOtherEntryCount(temporary->path());
    QVERIFY2(output.open(QIODevice::WriteOnly), qPrintable(output.errorString()));
    const int entriesAfterOpen = fixtureOtherEntryCount(temporary->path());
    QCOMPARE(output.write("local"), qint64(5));
    QFile held(path);
    QVERIFY(held.open(QIODevice::ReadOnly));
    const bool committed = output.commit();
    const auto qtError = output.error();
    const auto blocked = observeFixtureCommitFailure(temporary->path());
    QVERIFY(!committed);
    QCOMPARE(qtError, QFileDevice::RenameError);
    QVERIFY(blocked.valid);
    QCOMPARE(blocked.targetState, QStringLiteral("original"));
    const QByteArray baseline("original");
    const QByteArray attempted("local");
    QCOMPARE(observeFixtureCommitFailure(temporary->path(), &baseline, &attempted).targetState,
             QStringLiteral("baseline"));
    QVERIFY(blocked.deleteOpenAttempted);
    QVERIFY(!blocked.deleteOpenSucceeded);
    QCOMPARE(blocked.deleteOpenNativeError, quint32(ERROR_SHARING_VIOLATION));
    QVERIFY(entriesBeforeOpen >= 0);
    QVERIFY(entriesAfterOpen >= 0);
    QVERIFY(blocked.otherEntries >= 0);
    qInfo().noquote() << describeFixtureCommitFailure(blocked, entriesBeforeOpen, entriesAfterOpen);

    held.close();
    const auto released = observeFixtureCommitFailure(temporary->path());
    QVERIFY(released.valid);
    QCOMPARE(released.targetState, QStringLiteral("original"));
    QVERIFY(released.deleteOpenSucceeded);
    QCOMPARE(readFile(path), QByteArray("original"));
    QVERIFY(writeFile(path, QByteArray("external")));
    QCOMPARE(observeFixtureCommitFailure(temporary->path()).targetState, QStringLiteral("external"));
    QVERIFY(writeFile(path, QByteArray("local")));
    QCOMPARE(observeFixtureCommitFailure(temporary->path()).targetState, QStringLiteral("local"));
    QCOMPARE(observeFixtureCommitFailure(temporary->path(), &baseline, &attempted).targetState,
             QStringLiteral("attempted"));
    QCOMPARE(observeFixtureCommitFailure(temporary->path(), &attempted, &attempted).targetState,
             QStringLiteral("baseline-and-attempted"));
    QVERIFY(writeFile(path, QByteArray("other fixture value")));
    QCOMPARE(observeFixtureCommitFailure(temporary->path()).targetState, QStringLiteral("other"));
    QVERIFY(writeFile(path, QByteArray(65, 'x')));
    QCOMPARE(observeFixtureCommitFailure(temporary->path()).targetState, QStringLiteral("too-large-to-classify"));
    QVERIFY(QFile::remove(path));
    const auto missing = observeFixtureCommitFailure(temporary->path());
    QVERIFY(missing.valid);
    QCOMPARE(missing.targetState, QStringLiteral("missing"));
    QVERIFY(!missing.deleteOpenAttempted);
}

void QtDocumentSpikeTests::windowsSharingFailuresRetainEdits()
{
    auto temporary = fixtureDirectory();
    QVERIFY(temporary);
    const QString path = temporary->filePath(QStringLiteral("note.md"));
    QVERIFY(writeFile(path, QByteArray("original")));
    SpikeSession session;
    QString error;
    QVERIFY2(session.open(path, &error), qPrintable(error));
    QTextCursor cursor(session.document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QStringLiteral(" local"));
    QVERIFY(session.isDirty());
    const QString native = QDir::toNativeSeparators(path);

    // A separate handle denies later readers, making the baseline check fail.
    HANDLE held = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), GENERIC_READ,
                              FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    QVERIFY(held != INVALID_HANDLE_VALUE);
    const auto readFailure = session.save({}, &error);
    CloseHandle(held);
    QVERIFY(readFailure == SpikeFile::SaveOutcome::FailedBeforeCommit);
    const QString readPrefix = QStringLiteral("baseline read: Qt file error %1:")
                                   .arg(int(QFileDevice::OpenError));
    QVERIFY2(error.startsWith(readPrefix), qPrintable(error));
    QCOMPARE(readFile(path), QByteArray("original"));
    QVERIFY(session.isDirty());

    // This handle permits reads/writes but not deletion or replacement.
    held = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                       FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    QVERIFY(held != INVALID_HANDLE_VALUE);
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral(
        "fixture target=baseline;.*DELETE-access open=failed; its native error=32")));
    const auto commitFailure = session.save({}, &error);
    CloseHandle(held);
    QVERIFY(commitFailure == SpikeFile::SaveOutcome::CommitUncertain);
    const QString expectedPrefix = QStringLiteral("commit (Qt file error %1):")
                                       .arg(int(QFileDevice::RenameError));
    QVERIFY2(error.startsWith(expectedPrefix), qPrintable(error));
    QCOMPARE(readFile(path), QByteArray("original"));
    QVERIFY(session.isDirty());

    // Retry only after the deliberately held test handle has been released.
    QVERIFY2(session.save({}, &error) == SpikeFile::SaveOutcome::Saved, qPrintable(error));
    QCOMPARE(readFile(path), QByteArray("original local"));
    QVERIFY(!session.isDirty());
}

void QtDocumentSpikeTests::windowsUiaValueWrite_data()
{
    QTest::addColumn<bool>("readOnly");
    QTest::newRow("read-only") << true;
    QTest::newRow("editable") << false;
}

void QtDocumentSpikeTests::windowsUiaValueWrite()
{
    QFETCH(bool, readOnly);
    if (QGuiApplication::platformName() != QStringLiteral("windows"))
        QSKIP("Windows UI Automation requires a native window; offscreen CI only builds the client.");

    QAccessible::setActive(true);
    QQmlEngine engine;
    QQuickWindow window;
    window.setFlag(Qt::WindowDoesNotAcceptFocus);
    window.setTitle(QStringLiteral("xdnote-controlled-uia-fixture"));
    window.resize(400, 200);
    window.setPosition(-10000, -10000);
    window.show();
    QQuickItem *editor = makeEditor(engine, window, true);
    QVERIFY(editor);
    QVERIFY(editor->setProperty("text", QStringLiteral("original")));
    QVERIFY(QMetaObject::invokeMethod(editor, "insert", Qt::DirectConnection,
                                     Q_ARG(int, 8), Q_ARG(QString, QStringLiteral(" tail"))));
    QVERIFY(editor->setProperty("readOnly", readOnly));
    QQuickTextDocument *quick = quickDocument(editor);
    QVERIFY(quick);
    QTextDocument *document = quick->textDocument();
    QVERIFY(document);
    QVERIFY(document->isUndoAvailable());
    QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(editor);
    QVERIFY(accessible);
    QCOMPARE(bool(accessible->state().readOnly), readOnly);

    QProcess client;
    client.setProgram(QCoreApplication::applicationDirPath() + QStringLiteral("/qt_uia_client.exe"));
    client.setArguments({QString::number(quintptr(window.winId()))});
    const auto cleanup = qScopeGuard([&client] {
        if (client.state() != QProcess::NotRunning) {
            client.kill();
            client.waitForFinished(5000);
        }
    });
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(&client, &QProcess::finished, &loop, &QEventLoop::quit);
    client.start();
    QVERIFY2(client.waitForStarted(5000), qPrintable(client.errorString()));
    timeout.start(15000);
    if (client.state() != QProcess::NotRunning)
        loop.exec(); // Service UI Automation requests on the window's GUI thread.
    QVERIFY2(client.state() == QProcess::NotRunning, "UI Automation fixture timed out");
    QCOMPARE(client.exitStatus(), QProcess::NormalExit);
    const QString clientError = QStringLiteral("UI Automation client exit %1: %2")
                                    .arg(client.exitCode()).arg(QString::fromUtf8(client.readAllStandardError()));
    QVERIFY2(client.exitCode() == 0, qPrintable(clientError));

    const QList<QByteArray> lines = client.readAllStandardOutput().trimmed().split('\n');
    QCOMPARE(lines.size(), 2);
    QCOMPARE(lines.at(0).trimmed(), readOnly ? QByteArray("READONLY 1") : QByteArray("READONLY 0"));
    const QByteArray statusLine = lines.at(1).trimmed();
    QVERIFY(statusLine.startsWith("HRESULT "));
    bool parsed = false;
    const quint32 status = statusLine.mid(8).toUInt(&parsed);
    QVERIFY(parsed);
    qInfo() << "UI Automation SetValue HRESULT:" << status;

    if (readOnly) {
        QVERIFY(status & 0x80000000u);
        QCOMPARE(editor->property("text").toString(), QStringLiteral("original tail"));
        QVERIFY(document->isUndoAvailable());
        document->undo();
        QCOMPARE(editor->property("text").toString(), QStringLiteral("original"));
        document->redo();
        QCOMPARE(editor->property("text").toString(), QStringLiteral("original tail"));
    } else {
        QCOMPARE(status, quint32(0));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("UIA modified"));
        // This negative observation is not the desired product undo behavior.
        QVERIFY(!document->isUndoAvailable());
    }
}
#endif

QTEST_MAIN(QtDocumentSpikeTests)
#include "qt_spike_test.moc"
