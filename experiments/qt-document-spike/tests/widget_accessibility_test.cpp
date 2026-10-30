#include <QAccessible>
#include <QPlainTextEdit>
#include <QTextCursor>
#include <QTextDocument>
#include <QtTest>

// Direct Qt interface calls are a controlled comparison, not evidence that
// any particular screen reader sends these operations.
class WidgetAccessibilityTests : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void wholeValueWrite_data();
    void wholeValueWrite();
    void rangeWrite_data();
    void rangeWrite();
};

void WidgetAccessibilityTests::initTestCase()
{
    QAccessible::setActive(true);
}

void WidgetAccessibilityTests::wholeValueWrite_data()
{
    QTest::addColumn<bool>("readOnly");
    QTest::newRow("editable") << false;
    QTest::newRow("read-only") << true;
}

void WidgetAccessibilityTests::wholeValueWrite()
{
    QFETCH(bool, readOnly);
    const QString original = QStringLiteral("甲🙂乙");
    const QString withTail = original + QStringLiteral("尾");
    QPlainTextEdit editor;
    editor.setPlainText(original);
    editor.document()->clearUndoRedoStacks();
    QTextCursor cursor(editor.document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QStringLiteral("尾"));
    QVERIFY(editor.document()->isUndoAvailable());
    editor.setReadOnly(readOnly);

    QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(&editor);
    QVERIFY(accessible);
    QCOMPARE(bool(accessible->state().readOnly), readOnly);
    QVERIFY(accessible->textInterface());
    QVERIFY(accessible->editableTextInterface());
    QCOMPARE(accessible->textInterface()->text(0, accessible->textInterface()->characterCount()), withTail);

    const QString replacement = QStringLiteral("替换");
    accessible->setText(QAccessible::Value, replacement);
    if (readOnly) {
        // This value setter respects read-only and leaves the previous edit undoable.
        QCOMPARE(editor.toPlainText(), withTail);
        QVERIFY(editor.document()->isUndoAvailable());
        editor.undo();
        QCOMPARE(editor.toPlainText(), original);
        editor.redo();
        QCOMPARE(editor.toPlainText(), withTail);
    } else {
        // Whole-value replacement clears the existing undo history in this fixture.
        QCOMPARE(editor.toPlainText(), replacement);
        QVERIFY(!editor.document()->isUndoAvailable());
        editor.undo();
        QCOMPARE(editor.toPlainText(), replacement);
    }
    QCOMPARE(accessible->textInterface()->text(0, accessible->textInterface()->characterCount()),
             editor.toPlainText());
}

void WidgetAccessibilityTests::rangeWrite_data()
{
    QTest::addColumn<bool>("readOnly");
    QTest::addColumn<int>("operation");
    QTest::newRow("editable insert") << false << 0;
    QTest::newRow("read-only insert") << true << 0;
    QTest::newRow("editable delete") << false << 1;
    QTest::newRow("read-only delete") << true << 1;
    QTest::newRow("editable replace") << false << 2;
    QTest::newRow("read-only replace") << true << 2;
}

void WidgetAccessibilityTests::rangeWrite()
{
    QFETCH(bool, readOnly);
    QFETCH(int, operation);
    const QString original = QStringLiteral("甲🙂乙");
    const QString withTail = original + QStringLiteral("尾");
    const QString afterDeletion = QStringLiteral("甲乙尾");
    QPlainTextEdit editor;
    editor.setPlainText(original);
    editor.document()->clearUndoRedoStacks();
    QTextCursor cursor(editor.document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QStringLiteral("尾"));
    QVERIFY(editor.document()->isUndoAvailable());
    editor.setReadOnly(readOnly);

    QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(&editor);
    QVERIFY(accessible);
    QCOMPARE(bool(accessible->state().readOnly), readOnly);
    QAccessibleTextInterface *text = accessible->textInterface();
    QAccessibleEditableTextInterface *editable = accessible->editableTextInterface();
    QVERIFY(text);
    QVERIFY(editable);
    QCOMPARE(text->text(0, text->characterCount()), withTail);

    // Offsets 1..3 cover the entire emoji in this Qt UTF-16 fixture.
    QString expected;
    if (operation == 0) {
        editable->insertText(1, QStringLiteral("中"));
        expected = QStringLiteral("甲中🙂乙尾");
    } else if (operation == 1) {
        editable->deleteText(1, 3);
        expected = afterDeletion;
    } else {
        editable->replaceText(1, 3, QStringLiteral("中"));
        expected = QStringLiteral("甲中乙尾");
    }
    // This records the same mutation in the read-only row, not desired behavior.
    QCOMPARE(editor.toPlainText(), expected);
    QCOMPARE(text->text(0, text->characterCount()), expected);
    QVERIFY(editor.document()->isUndoAvailable());

    editor.undo();
    if (operation == 2) {
        // Replacement is two undo steps in this sample: insertion, then deletion.
        QCOMPARE(editor.toPlainText(), afterDeletion);
        editor.undo();
    }
    QCOMPARE(editor.toPlainText(), withTail);
    editor.undo();
    QCOMPARE(editor.toPlainText(), original);
    editor.redo();
    QCOMPARE(editor.toPlainText(), withTail);
    editor.redo();
    if (operation == 2) {
        QCOMPARE(editor.toPlainText(), afterDeletion);
        editor.redo();
    }
    QCOMPARE(editor.toPlainText(), expected);
    QCOMPARE(text->text(0, text->characterCount()), expected);
}

QTEST_MAIN(WidgetAccessibilityTests)
#include "widget_accessibility_test.moc"
