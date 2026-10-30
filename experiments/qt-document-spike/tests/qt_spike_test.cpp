#include "spike_document.h"
#include "spike_handoff.h"
#include "spike_ipc.h"
#include "file_identity_probe.h"
#include "displaced_probe.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QInputMethodEvent>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextFrame>
#include <QTextFragment>
#include <QTextList>
#include <QTextTable>
#include <QUuid>
#include <QtEndian>
#include <QtTest>

#include <memory>

#ifdef Q_OS_WIN
#  define NOMINMAX
#  include <windows.h>
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

bool startFixtureWriter(QProcess *writer, const QString &path, QString *error)
{
    writer->setProgram(fixtureWriterPath());
    writer->setArguments({path});
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

QQuickItem *makeEditor(QQmlEngine &engine, QQuickWindow &window)
{
    QQmlComponent component(&engine);
    component.setData(QByteArrayLiteral(
        "import QtQuick\n"
        "TextEdit { width: 320; height: 200; textFormat: TextEdit.PlainText; "
        "selectByMouse: true; objectName: 'spikeEditor' }\n"), QUrl());
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
    void previewParsesMarkdownAndBlocksResources();
    void markdownCorpusParsesBasicSemantics();
    void markdownAnchorsRemainUntrusted();
    void previewUsesPlaceholdersForLocalAndRemoteImages();
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
    void displacedFileRetainsInterveningWrite();
    void openWriterCanStillChangeDisplacedFile();
    void missingTargetDoesNotReplacePreparedFile();
    void editDuringSaveStaysDirty();
    void deletedTargetIsNotRecreated();
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
    QSaveFile output(path);
    output.setDirectWriteFallback(false);
    QVERIFY2(output.open(QIODevice::WriteOnly), qPrintable(output.errorString()));
    QCOMPARE(output.write("local"), qint64(5));
    QCOMPARE(readFile(path), baseline); // Last comparison in the candidate sequence.
    QVERIFY(writeFile(path, "external")); // Another actor writes after that comparison.
    QVERIFY2(output.commit(), qPrintable(output.errorString()));
    QCOMPARE(readFile(path), QByteArray("local")); // The intervening update was lost.
}

void QtDocumentSpikeTests::fixtureWriterRejectsOutsideScope()
{
    auto temporary = fixtureDirectory();
    QVERIFY(temporary);
    const QString other = temporary->filePath(QStringLiteral("other.md"));
    QVERIFY(writeFile(other, "original"));
    QProcess writer;
    writer.start(fixtureWriterPath(), {other});
    QVERIFY2(writer.waitForStarted(5000), qPrintable(writer.errorString()));
    QVERIFY2(writer.waitForFinished(5000), qPrintable(writer.errorString()));
    QCOMPARE(writer.exitStatus(), QProcess::NormalExit);
    QCOMPARE(writer.exitCode(), 3);
    QCOMPARE(readFile(other), QByteArray("original"));
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

QTEST_MAIN(QtDocumentSpikeTests)
#include "qt_spike_test.moc"
