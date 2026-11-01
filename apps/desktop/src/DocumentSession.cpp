#include "DocumentSession.h"
#include "SourceEditor.h"

#include <QScopedValueRollback>
#include <QThread>
#include <QTimer>
#include <limits>
#include <stdexcept>

namespace xdnote {
QString resultDescription(EditResult result) {
    switch (result) {
    case EditResult::Applied: return QStringLiteral("已应用");
    case EditResult::Unchanged: return QStringLiteral("内容未变化");
    case EditResult::ReadOnly: return QStringLiteral("文档为只读，无法修改");
    case EditResult::Composing: return QStringLiteral("请先完成或取消输入法组合");
    case EditResult::Busy: return QStringLiteral("编辑事务尚未结束，请稍后重试");
    case EditResult::Closing: return QStringLiteral("文档正在关闭");
    case EditResult::InvalidText: return QStringLiteral("文字无效或超过准入样本限制");
    case EditResult::InvalidRange: return QStringLiteral("文字范围不是有效字符边界");
    case EditResult::WrongThread: return QStringLiteral("编辑请求不在文档所属线程");
    case EditResult::StaleRevision: return QStringLiteral("文档已经变化，请重新取得当前位置");
    }
    return {};
}

bool DocumentSession::validText(const QString &text) {
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text[i].isHighSurrogate()) {
            if (++i >= text.size() || !text[i].isLowSurrogate()) return false;
        } else if (text[i].isLowSurrogate()) return false;
    }
    return true;
}

bool DocumentSession::utf16Boundary(const QString &text, int offset) {
    return offset >= 0 && offset <= text.size()
        && !(offset > 0 && offset < text.size() && text[offset - 1].isHighSurrogate() && text[offset].isLowSurrogate());
}

DocumentSession::DocumentSession(const QByteArray &utf8, QObject *parent) : QObject(parent) {
    if (utf8.size() > AdmissionSourceLimit || QString::fromUtf8(utf8).toUtf8() != utf8)
        throw std::invalid_argument("invalid admission UTF-8 snapshot");
    m_owner = std::make_unique<ScintillaEditBase>();
    m_owner->send(SCI_SETCODEPAGE, SC_CP_UTF8);
    m_owner->send(SCI_ADDTEXT, uptr_t(utf8.size()), reinterpret_cast<sptr_t>(utf8.constData()));
    m_owner->send(SCI_EMPTYUNDOBUFFER);
    m_owner->send(SCI_SETSAVEPOINT);
    connect(m_owner.get(), &ScintillaEditBase::notifyChange, this, [this] {
        m_pending = true;
        scheduleFlush();
    });
}

DocumentSession::~DocumentSession() {
    // Invalidate adapters before the hidden owner's destructor releases its
    // document reference and before QObject clears cached weak references.
    m_closing = true;
}

void DocumentSession::attach(SourceEditor *view) {
    Q_ASSERT(QThread::currentThread() == thread());
    view->send(SCI_SETDOCPOINTER, 0, m_owner->send(SCI_GETDOCPOINTER));
    m_views.append(view);
}

bool DocumentSession::isReadOnly() const { return m_closing || m_owner->send(SCI_GETREADONLY) != 0; }

bool DocumentSession::isComposing() const {
    for (const auto &view : m_views)
        if (view && view->compositionPending()) return true;
    return false;
}
bool DocumentSession::isBusy() const {
    if (m_mutations || m_notifying || m_reporting) return true;
    for (const auto &view : m_views) if (view && view->dispatching()) return true;
    return false;
}

EditResult DocumentSession::writable() const {
    if (QThread::currentThread() != thread()) return EditResult::WrongThread;
    if (m_closing) return EditResult::Closing;
    if (isBusy()) return EditResult::Busy;
    if (isComposing()) return EditResult::Composing;
    if (isReadOnly()) return EditResult::ReadOnly;
    return EditResult::Applied;
}

QByteArray DocumentSession::bytes() const {
    const auto length = m_owner->send(SCI_GETLENGTH);
    if (length < 0 || length > AdmissionSourceLimit) return {};
    QByteArray value(qsizetype(length + 1), '\0');
    const auto copied = m_owner->send(SCI_GETTEXT, uptr_t(length + 1), reinterpret_cast<sptr_t>(value.data()));
    if (copied != length) return {};
    value.resize(qsizetype(length));
    return value;
}

void DocumentSession::scheduleFlush() {
    if (m_flushQueued) return;
    m_flushQueued = true;
    QTimer::singleShot(0, this, [this] { m_flushQueued = false; flush(); });
}

void DocumentSession::flush() {
    if (!m_pending || m_mutations || m_notifying || isComposing()) return;
    for (const auto &view : m_views) if (view && view->dispatching()) return;
    m_pending = false;
    if (m_revision == std::numeric_limits<quint64>::max()) {
        m_closing = true;
        return;
    }
    ++m_revision;
    QScopedValueRollback<bool> notifying(m_notifying, true);
    emit changed(m_revision);
}

void DocumentSession::inputFinished() { if (m_pending) scheduleFlush(); }
quint64 DocumentSession::revision() { flush(); return m_revision; }

DocumentSnapshot DocumentSession::snapshot() {
    if (QThread::currentThread() != thread()) return {EditResult::WrongThread, {}, 0};
    if (m_closing) return {EditResult::Closing, {}, m_revision};
    if (isBusy()) return {EditResult::Busy, {}, m_revision};
    if (isComposing()) return {EditResult::Composing, {}, m_revision};
    for (const auto &view : m_views) if (view && view->dispatching()) return {EditResult::Busy, {}, m_revision};
    flush();
    if (m_closing) return {EditResult::Closing, {}, m_revision};
    if (m_owner->send(SCI_GETLENGTH) > AdmissionSourceLimit)
        return {EditResult::InvalidText, {}, m_revision};
    return {EditResult::Applied, bytes(), m_revision};
}

void DocumentSession::reportRejection(EditResult result) {
    if (QThread::currentThread() != thread()) return;
    m_rejection = resultDescription(result);
    if (m_reporting) return;
    QScopedValueRollback<bool> reporting(m_reporting, true);
    emit commandRejected(result);
    emit stateChanged();
}

EditResult DocumentSession::complete(EditResult result) {
    if (result != EditResult::Applied && result != EditResult::Unchanged) reportRejection(result);
    else {
        if (!m_rejection.isEmpty()) {
            m_rejection.clear();
            QScopedValueRollback<bool> notifying(m_notifying, true);
            emit stateChanged();
        }
        flush();
    }
    return result;
}

EditResult DocumentSession::replace(int start, int end, const QString &text, quint64 expectedRevision) {
    const auto allowed = writable();
    if (allowed != EditResult::Applied) return complete(allowed);
    flush();
    if (m_closing) return complete(EditResult::Closing);
    if (expectedRevision != m_revision) return complete(EditResult::StaleRevision);
    if (m_owner->send(SCI_GETLENGTH) > AdmissionSourceLimit) return complete(EditResult::InvalidText);
    if (!validText(text)) return complete(EditResult::InvalidText);
    const QByteArray before = bytes();
    const QString current = QString::fromUtf8(before);
    if (start > end || !utf16Boundary(current, start) || !utf16Boundary(current, end))
        return complete(EditResult::InvalidRange);
    const qsizetype first = current.left(start).toUtf8().size();
    const qsizetype last = current.left(end).toUtf8().size();
    const QByteArray replacement = text.toUtf8();
    if (before.size() - (last - first) + replacement.size() > AdmissionSourceLimit)
        return complete(EditResult::InvalidText);
    if (before.mid(first, last - first) == replacement) return complete(EditResult::Unchanged);
    {
        QScopedValueRollback<int> mutation(m_mutations, m_mutations + 1);
        m_owner->send(SCI_BEGINUNDOACTION);
        m_owner->send(SCI_SETTARGETSTART, uptr_t(first));
        m_owner->send(SCI_SETTARGETEND, uptr_t(last));
        m_owner->send(SCI_REPLACETARGET, uptr_t(replacement.size()), reinterpret_cast<sptr_t>(replacement.constData()));
        m_owner->send(SCI_ENDUNDOACTION);
    }
    return complete(EditResult::Applied);
}

EditResult DocumentSession::undo() {
    const auto allowed = writable();
    if (allowed != EditResult::Applied) return complete(allowed);
    if (!m_owner->send(SCI_CANUNDO)) return complete(EditResult::Unchanged);
    { QScopedValueRollback<int> mutation(m_mutations, m_mutations + 1); m_owner->send(SCI_UNDO); }
    return complete(EditResult::Applied);
}

EditResult DocumentSession::redo() {
    const auto allowed = writable();
    if (allowed != EditResult::Applied) return complete(allowed);
    if (!m_owner->send(SCI_CANREDO)) return complete(EditResult::Unchanged);
    { QScopedValueRollback<int> mutation(m_mutations, m_mutations + 1); m_owner->send(SCI_REDO); }
    return complete(EditResult::Applied);
}

EditResult DocumentSession::setReadOnly(bool readOnly) {
    if (QThread::currentThread() != thread()) return EditResult::WrongThread;
    if (m_closing) return complete(EditResult::Closing);
    if (isBusy()) return complete(EditResult::Busy);
    if (isComposing()) return complete(EditResult::Composing);
    m_owner->send(SCI_SETREADONLY, readOnly);
    { QScopedValueRollback<bool> notifying(m_notifying, true); emit stateChanged(); }
    return complete(EditResult::Applied);
}

EditResult DocumentSession::requestClose() {
    if (QThread::currentThread() != thread()) return EditResult::WrongThread;
    if (m_closing) return EditResult::Closing;
    if (isComposing()) return complete(EditResult::Composing);
    m_closing = true;
    // Defer state changes and notification until the active Scintilla callback
    // and application observer have returned. Closing never deletes this object.
    QTimer::singleShot(0, this, &DocumentSession::finishClose);
    return EditResult::Applied;
}
void DocumentSession::finishClose() {
    if (isBusy()) { QTimer::singleShot(1, this, &DocumentSession::finishClose); return; }
    m_owner->send(SCI_SETREADONLY, true);
    emit stateChanged();
    emit closed();
}
}
