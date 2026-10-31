#include "guarded_quick_accessibility.h"

#include <QQuickItem>
#include <QQuickTextDocument>
#include <QPointer>
#include <QScopedValueRollback>
#include <QTextCursor>
#include <QTextDocument>
#include <QThread>
#include <QtQml/qqml.h>

#include <memory>
#include <QtQuick/private/qaccessiblequickitem_p.h>

namespace {
bool supportedReplacement(const QString &value)
{
    for (qsizetype i = 0; i < value.size(); ++i) {
        const QChar ch = value.at(i);
        if (ch == QLatin1Char('\r') || ch == QChar::LineSeparator || ch == QChar::ParagraphSeparator)
            return false;
        if (ch.isHighSurrogate()) {
            if (++i >= value.size() || !value.at(i).isLowSurrogate())
                return false;
        } else if (ch.isLowSurrogate()) {
            return false;
        }
    }
    return true;
}

QString plainFixtureText(QTextDocument *document)
{
    if (!document)
        return {};
    QString text = document->toRawText();
    text.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    return text;
}

class GuardedQuickAccessibility final : public QAccessibleQuickItem
{
public:
    explicit GuardedQuickAccessibility(QQuickItem *item) : QAccessibleQuickItem(item) {}

    QString text(QAccessible::Text kind) const override
    {
        if (kind == QAccessible::Value)
            return plainFixtureText(currentDocument());
        return QAccessibleQuickItem::text(kind);
    }

    void setText(QAccessible::Text kind, const QString &value) override
    {
        QQuickItem *editor = qobject_cast<QQuickItem *>(object());
        const auto writeState = m_writeState;
        if (!editor || QThread::currentThread() != editor->thread() || writeState->writing
            || kind != QAccessible::Value || role() != QAccessible::EditableText
            || editor->property("readOnly").toBool() || state().readOnly
            || editor->property("inputMethodComposing").toBool() || !supportedReplacement(value))
            return;
        QTextDocument *document = currentDocument();
        if (!document || !document->isUndoRedoEnabled())
            return;
        const QString before = plainFixtureText(document);
        if (before == value)
            return;
        QScopedValueRollback<bool> writing(writeState->writing, true);
        QTextCursor cursor(document);
        cursor.beginEditBlock();
        cursor.select(QTextCursor::Document);
        cursor.insertText(value);
        cursor.endEditBlock();
    }

    int characterCount() const override
    {
        QTextDocument *document = currentDocument();
        return document ? qMax(0, document->characterCount() - 1) : 0;
    }

    QString text(int startOffset, int endOffset) const override
    {
        const QString value = plainFixtureText(currentDocument());
        if (startOffset < 0 || endOffset < startOffset || endOffset > value.size())
            return {};
        return value.mid(startOffset, endOffset - startOffset);
    }

    QString textBeforeOffset(int offset, QAccessible::TextBoundaryType boundary,
                             int *start, int *end) const override
    {
        if (!start || !end)
            return {};
        return QAccessibleTextInterface::textBeforeOffset(normalizeOffset(offset), boundary, start, end);
    }

    QString textAtOffset(int offset, QAccessible::TextBoundaryType boundary,
                         int *start, int *end) const override
    {
        if (!start || !end)
            return {};
        return QAccessibleTextInterface::textAtOffset(normalizeOffset(offset), boundary, start, end);
    }

    QString textAfterOffset(int offset, QAccessible::TextBoundaryType boundary,
                            int *start, int *end) const override
    {
        if (!start || !end)
            return {};
        return QAccessibleTextInterface::textAfterOffset(normalizeOffset(offset), boundary, start, end);
    }

private:
    QTextDocument *currentDocument() const
    {
        QQuickItem *editor = qobject_cast<QQuickItem *>(object());
        if (!editor || QThread::currentThread() != editor->thread())
            return nullptr;
        auto *quick = editor->property("textDocument").value<QQuickTextDocument *>();
        return quick ? quick->textDocument() : nullptr;
    }

    int normalizeOffset(int offset) const
    {
        if (offset == -2)
            return cursorPosition();
        return offset;
    }

    struct WriteState { bool writing = false; };
    std::shared_ptr<WriteState> m_writeState = std::make_shared<WriteState>();
};
} // namespace

struct GuardedFixtureTextEdit::NotificationState
{
    QString lastText;
    bool notifying = false;
    bool dirty = false;
};

GuardedFixtureTextEdit::GuardedFixtureTextEdit(QQuickItem *parent)
    : QQuickTextEdit(parent), m_notifications(std::make_shared<NotificationState>())
{
    QQuickTextDocument *quick = textDocument();
    connect(quick, &QQuickTextDocument::textDocumentChanged, this, &GuardedFixtureTextEdit::bindDocument);
    bindDocument();
}

void GuardedFixtureTextEdit::bindDocument()
{
    disconnect(m_documentChange);
    QTextDocument *document = textDocument()->textDocument();
    if (document)
        m_documentChange = connect(document, &QTextDocument::contentsChanged,
                                   this, &GuardedFixtureTextEdit::notifyDocumentText);
    notifyDocumentText();
}

void GuardedFixtureTextEdit::notifyDocumentText()
{
    const auto state = m_notifications;
    state->dirty = true;
    if (state->notifying)
        return;
    QScopedValueRollback<bool> notifying(state->notifying, true);
    QPointer<GuardedFixtureTextEdit> alive(this);
    while (alive && state->dirty) {
        state->dirty = false;
        const QString before = state->lastText;
        const QString after = plainFixtureText(alive->textDocument()->textDocument());
        state->lastText = after;
        if (before == after)
            continue;
        QAccessibleTextUpdateEvent textEvent(alive, 0, before, after);
        QAccessible::updateAccessibility(&textEvent);
        if (!alive)
            return;
        if (state->dirty)
            continue; // Report the latest value rather than a stale reentrant value.
        QAccessibleValueChangeEvent valueEvent(alive, after);
        QAccessible::updateAccessibility(&valueEvent);
    }
}

void registerGuardedQuickFixtureType()
{
    static const int typeId = qmlRegisterType<GuardedFixtureTextEdit>("XdnoteFixture", 1, 0, "GuardedTextEdit");
    Q_UNUSED(typeId);
}

QAccessibleInterface *guardedQuickAccessibilityFactory(const QString &, QObject *object)
{
    auto *editor = qobject_cast<GuardedFixtureTextEdit *>(object);
    return editor ? new GuardedQuickAccessibility(editor) : nullptr;
}

bool isGuardedQuickAccessibility(QAccessibleInterface *interface)
{
    return dynamic_cast<GuardedQuickAccessibility *>(interface) != nullptr;
}
