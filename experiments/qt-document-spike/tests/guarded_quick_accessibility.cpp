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
        if (kind == QAccessible::Description) {
            if (auto *editor = qobject_cast<GuardedFixtureTextEdit *>(object())) {
                if (!editor->writeError().isEmpty())
                    return editor->writeError();
            }
        }
        return QAccessibleQuickItem::text(kind);
    }

    void setText(QAccessible::Text kind, const QString &value) override
    {
        auto *editor = qobject_cast<GuardedFixtureTextEdit *>(object());
        if (editor && kind == QAccessible::Value && role() == QAccessible::EditableText)
            editor->replaceAccessibleText(value);
    }

    QAccessible::State state() const override
    {
        auto result = QAccessibleQuickItem::state();
        if (auto *editor = qobject_cast<GuardedFixtureTextEdit *>(object())) {
            if (auto life = editor->lifetime().lock()) {
                result.busy = life->busy;
                result.invalid = life->closing;
                result.readOnly = result.readOnly || life->closing || life->busy
                    || editor->isReadOnly() || editor->isInputMethodComposing();
            }
        }
        return result;
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
        auto *editor = qobject_cast<GuardedFixtureTextEdit *>(object());
        if (!editor || QThread::currentThread() != editor->thread())
            return nullptr;
        const auto life = editor->lifetime().lock();
        if (!life || life->closing)
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

};
} // namespace

struct GuardedFixtureTextEdit::NotificationState
{
    QString lastText;
    bool notifying = false;
    bool dirty = false;
};

GuardedFixtureTextEdit::GuardedFixtureTextEdit(QQuickItem *parent)
    : QQuickTextEdit(parent), m_lifetime(std::make_shared<FixtureEditorLifetime>()),
      m_notifications(std::make_shared<NotificationState>())
{
    m_lifetime->editor = this;
    m_lifetime->document = std::make_shared<QTextDocument>();
    QQuickTextDocument *quick = textDocument();
    quick->setTextDocument(m_lifetime->document.get());
    connect(quick, &QQuickTextDocument::textDocumentChanged, this, &GuardedFixtureTextEdit::bindDocument);
    // Keep the document alive through QQuickTextEdit's base destructor, then
    // release it even when a native provider retains the lifetime token.
    connect(this, &QObject::destroyed, [life = m_lifetime] {
        life->editor = nullptr;
        life->closing = true;
        life->document.reset();
    });
    bindDocument();
}

GuardedFixtureTextEdit::~GuardedFixtureTextEdit()
{
    m_lifetime->closing = true;
    m_lifetime->editor = nullptr;
}

FixtureEditResult GuardedFixtureTextEdit::reportWriteResult(FixtureEditResult result)
{
    QString message;
    switch (result) {
    case FixtureEditResult::ReadOnly: message = QStringLiteral("文档为只读，未写入。"); break;
    case FixtureEditResult::Composing: message = QStringLiteral("输入法正在组合文字，请完成输入后重试。"); break;
    case FixtureEditResult::Busy: message = QStringLiteral("文档正在处理编辑请求，请稍后重试。"); break;
    case FixtureEditResult::Closing: message = QStringLiteral("文档正在关闭，未接受新的写入。"); break;
    case FixtureEditResult::InvalidText: message = QStringLiteral("文字包含当前不支持的字符，未写入。"); break;
    case FixtureEditResult::UndoUnavailable: message = QStringLiteral("无法保留撤销记录，未写入。"); break;
    case FixtureEditResult::NoDocument: case FixtureEditResult::Detached:
        message = QStringLiteral("文档会话不可用，未写入。"); break;
    default: break;
    }
    m_writeError = message;
    const auto life = m_lifetime;
    if (life->reporting)
        return result;
    QScopedValueRollback<bool> reporting(life->reporting, true);
    QPointer<GuardedFixtureTextEdit> alive(this);
    emit accessibleWriteFinished(int(result), message);
    if (alive && !message.isEmpty()) {
        QAccessibleAnnouncementEvent event(alive, message);
        QAccessible::updateAccessibility(&event);
    }
    return result;
}

FixtureEditResult GuardedFixtureTextEdit::replaceAccessibleText(const QString &value)
{
    if (QThread::currentThread() != thread())
        return FixtureEditResult::WrongThread;
    const auto life = m_lifetime;
    if (life->closing) return reportWriteResult(FixtureEditResult::Closing);
    if (life->busy) return reportWriteResult(FixtureEditResult::Busy);
    if (isReadOnly()) return reportWriteResult(FixtureEditResult::ReadOnly);
    if (isInputMethodComposing()) return reportWriteResult(FixtureEditResult::Composing);
    if (auto *accessible = QAccessible::queryAccessibleInterface(this)) {
        if (accessible->state().readOnly) return reportWriteResult(FixtureEditResult::ReadOnly);
    }
    if (!supportedReplacement(value)) return reportWriteResult(FixtureEditResult::InvalidText);
    const auto document = life->document; // Pins the document across every synchronous callback.
    if (!document) return reportWriteResult(FixtureEditResult::NoDocument);
    if (textDocument()->textDocument() != document.get()) return reportWriteResult(FixtureEditResult::Detached);
    if (!document->isUndoRedoEnabled()) return reportWriteResult(FixtureEditResult::UndoUnavailable);
    if (plainFixtureText(document.get()) == value) return reportWriteResult(FixtureEditResult::Unchanged);
    life->busy = true;
    QPointer<GuardedFixtureTextEdit> alive(this);
    {
        QTextCursor cursor(document.get());
        cursor.beginEditBlock();
        cursor.select(QTextCursor::Document);
        cursor.insertText(value);
        cursor.endEditBlock();
    }
    if (!alive) {
        life->busy = false;
        return FixtureEditResult::Closing;
    }
    finishTransaction();
    return reportWriteResult(FixtureEditResult::Applied);
}

FixtureEditResult GuardedFixtureTextEdit::replaceOwnedDocument(std::shared_ptr<QTextDocument> document)
{
    if (QThread::currentThread() != thread()) return FixtureEditResult::WrongThread;
    const auto life = m_lifetime;
    if (life->closing) return FixtureEditResult::Closing;
    if (life->busy) return FixtureEditResult::Busy;
    if (isInputMethodComposing()) return FixtureEditResult::Composing;
    if (!document || document->parent() || document->thread() != thread()) return FixtureEditResult::NoDocument;
    if (life->document == document) return FixtureEditResult::Unchanged;
    life->busy = true;
    const auto previous = life->document;
    life->document = std::move(document);
    textDocument()->setTextDocument(life->document.get());
    finishTransaction();
    return FixtureEditResult::Applied;
}

void GuardedFixtureTextEdit::finishTransaction()
{
    m_lifetime->busy = false;
    if (m_lifetime->closing && !m_lifetime->closeScheduled) {
        m_lifetime->closeScheduled = true;
        deleteLater();
    }
}

void GuardedFixtureTextEdit::requestClose()
{
    if (QThread::currentThread() != thread()) return;
    m_lifetime->closing = true;
    if (!m_lifetime->busy) finishTransaction();
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
        if (alive)
            emit alive->accessibleValueChanged(before, after);
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
