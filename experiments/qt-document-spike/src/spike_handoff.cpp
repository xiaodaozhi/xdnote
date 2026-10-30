#include "spike_handoff.h"

#include <QMetaObject>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QQuickWindow>

bool restoreSelection(QQuickItem *editor, int start, int end, int cursor)
{
    if (!editor)
        return false;
    bool invoked = false;
    if (start == end) {
        invoked = editor->setProperty("cursorPosition", cursor);
    } else {
        const int anchor = cursor == start ? end : start;
        invoked = QMetaObject::invokeMethod(editor, "select", Qt::DirectConnection,
                                            Q_ARG(int, anchor), Q_ARG(int, cursor));
    }
    return invoked && editor->property("selectionStart").toInt() == start
        && editor->property("selectionEnd").toInt() == end
        && editor->property("cursorPosition").toInt() == cursor;
}

bool transferDocument(QQuickItem *from, QQuickItem *to)
{
    if (!from || !to || from == to || from->property("inputMethodComposing").toBool())
        return false;
    auto *source = from->property("textDocument").value<QQuickTextDocument *>();
    auto *destination = to->property("textDocument").value<QQuickTextDocument *>();
    if (!source || !destination || !source->textDocument())
        return false;

    const int start = from->property("selectionStart").toInt();
    const int end = from->property("selectionEnd").toInt();
    const int cursor = from->property("cursorPosition").toInt();
    destination->setTextDocument(source->textDocument());
    return restoreSelection(to, start, end, cursor);
}

bool reparentEditorFixture(QQuickItem *editor, QQuickWindow *destination)
{
    if (!editor || !destination || editor->property("inputMethodComposing").toBool())
        return false;
    editor->setParentItem(destination->contentItem());
    return editor->window() == destination;
}
