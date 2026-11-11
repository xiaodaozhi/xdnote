#include "SourceEditor.h"
#include "Utf8Text.h"
#include "DocumentSession.h"
#include "VisualTokens.h"
#include "Appearance.h"
#include "SharedWidgets.h"
#include "UiFonts.h"

#include <QAccessibleWidget>
#include <QColor>
#include <QFontDatabase>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QScopedValueRollback>
#include <QThread>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QMimeData>

namespace xdnote {
namespace {
QString contents(SourceEditor *editor) {
    if (!editor || QThread::currentThread() != editor->thread() || !editor->session()) return {};
    const auto snapshot = editor->session()->snapshot();
    return snapshot.result == EditResult::Applied ? decodeUtf8Literal(snapshot.utf8) : QString();
}

int byteToUtf16(const QString &text, sptr_t offset) {
    const QByteArray bytes = text.toUtf8();
    if (offset < 0 || offset > bytes.size()) return -1;
    const auto prefix = bytes.left(qsizetype(offset));
    const QString decoded = decodeUtf8Literal(prefix);
    return decoded.toUtf8() == prefix ? int(decoded.size()) : -1;
}

void replaceAccessibleText(SourceEditor *view, int start, int end, const QString &value, bool wholeValue) {
    // revision() can flush pending edits and notify observers. Do not call it
    // before checking the owning thread, or from an invalid snapshot.
    if (!view || QThread::currentThread() != view->thread()) return;
    const QPointer<SourceEditor> guardedView(view);
    const QPointer<DocumentSession> session(view->session());
    if (!session) return;
    const auto snapshot = session->snapshot();
    if (!guardedView || !session || guardedView->session() != session) return;
    if (snapshot.result != EditResult::Applied) {
        session->reportRejection(snapshot.result);
        return;
    }
    if (wholeValue) { start = 0; end = int(decodeUtf8Literal(snapshot.utf8).size()); }
    session->replace(start, end, value, snapshot.revision);
}

class AccessibleSource final : public QAccessibleWidget, public QAccessibleTextInterface,
                               public QAccessibleEditableTextInterface {
public:
    explicit AccessibleSource(SourceEditor *editor) : QAccessibleWidget(editor, QAccessible::EditableText) {}
    SourceEditor *editor() const { return qobject_cast<SourceEditor *>(object()); }
    bool isValid() const override {
        auto *view = editor();
        return QAccessibleWidget::isValid() && view && view->session() && !view->session()->isClosing();
    }
    void *interface_cast(QAccessible::InterfaceType type) override {
        if (type == QAccessible::TextInterface) return static_cast<QAccessibleTextInterface *>(this);
        if (type == QAccessible::EditableTextInterface) return static_cast<QAccessibleEditableTextInterface *>(this);
        return QAccessibleWidget::interface_cast(type);
    }
    QAccessible::State state() const override {
        auto value = QAccessibleWidget::state();
        auto *view = editor();
        value.focusable = true;
        value.multiLine = true;
        value.readOnly = !view || !view->session() || view->session()->isReadOnly() || view->session()->isClosing();
        value.editable = !value.readOnly;
        return value;
    }
    QString text(QAccessible::Text role) const override {
        if (role == QAccessible::Value) return contents(editor());
        if (role == QAccessible::Description && editor() && editor()->session()) return editor()->session()->rejection();
        return QAccessibleWidget::text(role);
    }
    void setText(QAccessible::Text role, const QString &value) override {
        if (role != QAccessible::Value) { QAccessibleWidget::setText(role, value); return; }
        // All accessible writes, including whole-value replacement, are normal
        // undoable session commands. Qt's void setter cannot report native HRESULT.
        replaceAccessibleText(editor(), 0, 0, value, true);
    }
    int characterCount() const override { return contents(editor()).size(); }
    QString text(int start, int end) const override {
        const auto value = contents(editor());
        if (end == -1) end = value.size();
        if (start > end || !DocumentSession::utf16Boundary(value, start) || !DocumentSession::utf16Boundary(value, end)) return {};
        return value.mid(start, end - start);
    }
    void selection(int index, int *start, int *end) const override {
        *start = *end = -1;
        auto *view = editor();
        if (index != 0 || !view) return;
        const auto value = contents(view);
        *start = byteToUtf16(value, view->send(SCI_GETSELECTIONSTART));
        *end = byteToUtf16(value, view->send(SCI_GETSELECTIONEND));
    }
    int selectionCount() const override {
        auto *view = editor();
        return view && !view->send(SCI_GETSELECTIONEMPTY) ? 1 : 0;
    }
    void addSelection(int start, int end) override { setSelection(0, start, end); }
    void removeSelection(int index) override { if (index == 0) setCursorPosition(cursorPosition()); }
    void setSelection(int index, int start, int end) override {
        auto *view = editor();
        const auto value = contents(view);
        if (index != 0 || !view || !DocumentSession::utf16Boundary(value, start) || !DocumentSession::utf16Boundary(value, end)) return;
        view->send(SCI_SETSEL, uptr_t(value.left(start).toUtf8().size()), sptr_t(value.left(end).toUtf8().size()));
    }
    int cursorPosition() const override {
        auto *view = editor();
        return view ? byteToUtf16(contents(view), view->send(SCI_GETCURRENTPOS)) : -1;
    }
    void setCursorPosition(int position) override { setSelection(0, position, position); }
    QRect characterRect(int offset) const override {
        auto *view = editor();
        const auto value = contents(view);
        if (!view || !DocumentSession::utf16Boundary(value, offset)) return {};
        const auto byte = value.left(offset).toUtf8().size();
        const int x = int(view->send(SCI_POINTXFROMPOSITION, 0, byte));
        const int y = int(view->send(SCI_POINTYFROMPOSITION, 0, byte));
        const auto line = view->send(SCI_LINEFROMPOSITION, uptr_t(byte));
        const int height = qMax(1, int(view->send(SCI_TEXTHEIGHT, uptr_t(line))));
        const auto next = view->send(SCI_POSITIONAFTER, uptr_t(byte));
        const int nextX = int(view->send(SCI_POINTXFROMPOSITION, 0, next));
        return QRect(view->viewport()->mapToGlobal(QPoint(x, y)), QSize(qMax(1, nextX - x), height));
    }
    int offsetAtPoint(const QPoint &point) const override {
        auto *view = editor();
        if (!view) return -1;
        const auto local = view->viewport()->mapFromGlobal(point);
        if (!view->viewport()->rect().contains(local)) return -1;
        return byteToUtf16(contents(view), view->send(SCI_POSITIONFROMPOINTCLOSE, uptr_t(local.x()), local.y()));
    }
    void scrollToSubstring(int start, int end) override {
        auto *view = editor();
        const auto value = contents(view);
        if (!view || !DocumentSession::utf16Boundary(value, start) || !DocumentSession::utf16Boundary(value, end)) return;
        view->send(SCI_SCROLLRANGE, uptr_t(value.left(start).toUtf8().size()), sptr_t(value.left(end).toUtf8().size()));
    }
    QString attributes(int offset, int *start, int *end) const override {
        const auto value = contents(editor());
        if (!DocumentSession::utf16Boundary(value, offset)) { *start = *end = -1; return {}; }
        *start = 0; *end = value.size();
        return {};
    }
    void deleteText(int start, int end) override { replaceText(start, end, {}); }
    void insertText(int offset, const QString &value) override { replaceText(offset, offset, value); }
    void replaceText(int start, int end, const QString &value) override {
        replaceAccessibleText(editor(), start, end, value, false);
    }
};
}

void SourceEditor::installAccessibility() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    QAccessible::installFactory([](const QString &, QObject *object) -> QAccessibleInterface * {
        if (auto *view = qobject_cast<SourceEditor *>(object)) return new AccessibleSource(view);
        return nullptr;
    });
}

SourceEditor::SourceEditor(DocumentSession *session, QWidget *parent, Appearance *appearance)
    : ScintillaEditBase(parent), m_appearance(appearance ? appearance : new Appearance(this)), m_session(session) {
    Q_ASSERT(session);
    session->attach(this);
    setObjectName(QStringLiteral("sourceEditor"));
    setAccessibleName(QStringLiteral("Markdown 源文"));
    setFrameShape(QFrame::NoFrame);
    send(SCI_SETCODEPAGE, SC_CP_UTF8);
    send(SCI_SETWRAPMODE, SC_WRAP_WORD);
    send(SCI_SETMARGINWIDTHN, 1, 0);
    // The parent window handles file drops through the session command
    // gateway. Scintilla's own popup remains disabled.
    send(SCI_USEPOPUP, SC_POPUP_NEVER);
    setAcceptDrops(true);
    viewport()->setAcceptDrops(true);
    auto font = sourceFont();
    setFont(font);
    const QByteArray family = font.family().toUtf8();
    send(SCI_STYLESETFONT, STYLE_DEFAULT, reinterpret_cast<sptr_t>(family.constData()));
    const auto source = visualTokens().value(QStringLiteral("typography")).toObject().value(QStringLiteral("source")).toObject();
    // Scintilla's public size API is in points; shared tokens are logical pixels.
    const qreal points = source.value(QStringLiteral("size")).toDouble() * 72.0 / logicalDpiY();
    send(SCI_STYLESETSIZEFRACTIONAL, STYLE_DEFAULT, qRound(points * 100));
    connect(m_appearance, &Appearance::changed, this, &SourceEditor::applyAppearance);
    connect(m_appearance->preferences(), &Preferences::changed, this, [this](const QString &section) { if (section == "editor") applyPreferences(); });
    applyAppearance();
    applyPreferences();
    send(SCI_SETMARGINLEFT, 0, 16);
    setTextZoom(100);
    connect(session, &DocumentSession::stateChanged, this, [this] {
        send(SCI_SETREADONLY, !m_session || m_session->isReadOnly());
        QAccessible::State changed;
        changed.readOnly = true;
        changed.editable = true;
        QAccessibleStateChangeEvent event(this, changed);
        QAccessible::updateAccessibility(&event);
    });
    connect(session, &DocumentSession::changed, this, [this] {
        restyle();
        QAccessibleValueChangeEvent event(this, contents(this));
        QAccessible::updateAccessibility(&event);
    }, Qt::QueuedConnection);
    connect(session, &QObject::destroyed, this, [this] { send(SCI_SETREADONLY, true); });
    send(SCI_SETREADONLY, session->isReadOnly());
}

void SourceEditor::setTextZoom(int percent) {
    m_textZoom = qBound(50, percent, 200);
    const auto source = m_appearance->preferences()->section("editor");
    const auto size = source.value("fontSize").toDouble() * m_textZoom / 100.0;
    const auto points = qRound(size * 72.0 / logicalDpiY() * 100);
    for (const auto style : {STYLE_DEFAULT, STYLE_LINENUMBER, 0, 1, 2, 3}) send(SCI_STYLESETSIZEFRACTIONAL, uptr_t(style), points);
    send(SCI_SETEXTRAASCENT, 0); send(SCI_SETEXTRADESCENT, 0);
    const int extra = qMax(0, qRound(size * source.value("lineHeight").toDouble()) - int(send(SCI_TEXTHEIGHT, 0)));
    send(SCI_SETEXTRAASCENT, extra / 2); send(SCI_SETEXTRADESCENT, extra - extra / 2);
}

void SourceEditor::applyPreferences() {
    auto values = m_appearance->preferences()->section("editor"); values.remove("autoSave");
    if (values == m_editorPreferences) return; m_editorPreferences = values;
    const auto firstLine = send(SCI_DOCLINEFROMVISIBLE, uptr_t(send(SCI_GETFIRSTVISIBLELINE)));
    const auto horizontal = send(SCI_GETXOFFSET);
    auto font = sourceFont(values["fontSize"].toInt());
    if (!values["fontFamily"].toString().isEmpty()) font.setFamily(values["fontFamily"].toString());
    setFont(font);
    const auto family = font.family().toUtf8();
    for (const auto style : {STYLE_DEFAULT, STYLE_LINENUMBER, 0, 1, 2, 3}) send(SCI_STYLESETFONT, uptr_t(style), reinterpret_cast<sptr_t>(family.constData()));
    send(SCI_SETWRAPMODE, values["wordWrap"].toBool() ? SC_WRAP_WORD : SC_WRAP_NONE);
    send(SCI_SETMARGINWIDTHN, 0, values["lineNumbers"].toBool() ? visualTokens()["controls"].toObject()["lineNumberWidth"].toInt() : 0);
    send(SCI_SETTABWIDTH, values["indentWidth"].toInt()); send(SCI_SETINDENT, values["indentWidth"].toInt());
    send(SCI_SETUSETABS, !values["useSpaces"].toBool()); setTextZoom(m_textZoom);
    send(SCI_SETFIRSTVISIBLELINE, uptr_t(send(SCI_VISIBLEFROMDOCLINE, uptr_t(firstLine))));
    send(SCI_SETXOFFSET, values["wordWrap"].toBool() ? 0 : uptr_t(horizontal));
}

void SourceEditor::applyAppearance() {
    setStyleSheet(themedScrollBars(m_appearance));
    const auto ref = [](const QColor &color) { return color.red() | (color.green() << 8) | (color.blue() << 16); };
    send(SCI_STYLESETFORE, STYLE_DEFAULT, ref(m_appearance->color("text")));
    send(SCI_STYLESETBACK, STYLE_DEFAULT, ref(m_appearance->color("background")));
    send(SCI_STYLECLEARALL);
    m_editorPreferences = {};
    send(SCI_STYLESETFORE, STYLE_LINENUMBER, ref(m_appearance->color("textSecondary")));
    send(SCI_STYLESETBACK, STYLE_LINENUMBER, ref(m_appearance->color("background")));
    send(SCI_STYLESETFORE, 1, ref(m_appearance->color("syntaxHeading"))); send(SCI_STYLESETBOLD, 1, true);
    send(SCI_STYLESETFORE, 2, ref(m_appearance->color("syntaxCode")));
    send(SCI_STYLESETFORE, 3, ref(m_appearance->color("textSecondary")));
    send(SCI_SETCARETFORE, ref(m_appearance->color("text")));
    send(SCI_SETSELBACK, true, ref(m_appearance->accentColor("soft")));
    send(SCI_SETSELFORE, true, ref(m_appearance->color("text")));
    applyPreferences();
    restyle();
}
void SourceEditor::restyle() {
    if (!m_session || m_session->isClosing()) return;
    const auto snapshot = m_session->snapshot();
    if (snapshot.result != EditResult::Applied) return;
    const auto style = [this](qsizetype first, qsizetype length, int number) {
        send(SCI_STARTSTYLING, uptr_t(first)); send(SCI_SETSTYLING, uptr_t(length), number);
    };
    style(0, snapshot.utf8.size(), 0);
    qsizetype offset = 0; bool fenced = false;
    for (const auto &line : snapshot.utf8.split('\n')) {
        const auto trimmed = line.trimmed();
        if (trimmed.startsWith("```") || trimmed.startsWith("~~~")) { style(offset, line.size(), 2); fenced = !fenced; }
        else if (fenced) style(offset, line.size(), 2);
        else {
            int hashes = 0; while (hashes < line.size() && hashes < 7 && line[hashes] == '#') ++hashes;
            if (hashes > 0 && hashes <= 6 && line.size() > hashes && line[hashes] == ' ') style(offset, line.size(), 1);
            else if (line.startsWith('>')) style(offset, 1, 3);
            for (qsizetype start = line.indexOf('`'); start >= 0;) {
                const auto end = line.indexOf('`', start + 1); if (end < 0) break;
                style(offset + start, end - start + 1, 2); start = line.indexOf('`', end + 1);
            }
        }
        offset += line.size() + 1;
    }
}

bool SourceEditor::compositionPending() const {
    return m_inIme || m_preedit || send(SCI_GETUNDOTENTATIVE) >= 0;
}
DocumentSession *SourceEditor::session() const { return m_session; }
void SourceEditor::contextMenuEvent(QContextMenuEvent *event) {
    if (!m_session || m_session->isClosing()) return;
    SharedMenu menu(m_appearance, this); menu.setObjectName("sourceContextMenu");
    auto add = [&](const QString &text, const QString &symbol, bool enabled, auto callback) { auto *item = menu.addAction(themedIcon(symbol, m_appearance->color("text")), text); item->setEnabled(enabled); connect(item, &QAction::triggered, this, callback); return item; };
    const bool writable = !m_session->isReadOnly() && !m_session->isBusy() && !m_session->isComposing();
    add(QStringLiteral("撤销"), "undo", writable && send(SCI_CANUNDO), [this] { if (m_session) m_session->undo(); });
    add(QStringLiteral("重做"), "redo", writable && send(SCI_CANREDO), [this] { if (m_session) m_session->redo(); }); menu.addSeparator();
    const bool selected = !send(SCI_GETSELECTIONEMPTY);
    auto selectionCommand = [this](bool cut, bool paste) {
        if (!m_session) return; const auto snapshot = m_session->snapshot(); if (snapshot.result != EditResult::Applied) return;
        const auto start = qsizetype(send(SCI_GETSELECTIONSTART)), end = qsizetype(send(SCI_GETSELECTIONEND));
        const auto first = decodeUtf8Literal(snapshot.utf8.left(start)).size(); const auto text = decodeUtf8Literal(snapshot.utf8.mid(start, end - start));
        if (!paste) QApplication::clipboard()->setText(text);
        if (cut || paste) m_session->replace(int(first), int(first + text.size()), paste ? QApplication::clipboard()->text() : QString(), snapshot.revision);
    };
    add(QStringLiteral("剪切"), "cut", writable && selected, [selectionCommand] { selectionCommand(true, false); });
    add(QStringLiteral("复制"), "copy", selected, [selectionCommand] { selectionCommand(false, false); });
    add(QStringLiteral("粘贴"), "paste", writable, [this, selectionCommand] {
        const auto *mime = QApplication::clipboard()->mimeData();
        if (mime && mime->hasImage() && !mime->hasText()) emit imagePasteRequested(QApplication::clipboard()->image());
        else selectionCommand(false, true);
    }); menu.addSeparator();
    add(QStringLiteral("全选"), "select-all", true, [this] { send(SCI_SELECTALL); });
    menu.exec(event->reason() == QContextMenuEvent::Keyboard ? mapToGlobal(rect().center()) : event->globalPos()); event->accept();
}

void SourceEditor::inputMethodEvent(QInputMethodEvent *event) {
    if (!m_session || m_session->isClosing() || m_session->isBusy() || m_session->isReadOnly()) {
        if (m_session) m_session->reportRejection(m_session->isClosing() ? EditResult::Closing
            : m_session->isReadOnly() ? EditResult::ReadOnly : EditResult::Busy);
        event->accept(); return;
    }
    if (!DocumentSession::validText(event->commitString()) || !DocumentSession::validText(event->preeditString())) {
        m_session->reportRejection(EditResult::InvalidText); event->accept(); return;
    }
    {
        QScopedValueRollback<int> depth(m_inputDepth, m_inputDepth + 1);
        QScopedValueRollback<bool> ime(m_inIme, true);
        m_preedit = !event->preeditString().isEmpty();
        if (!event->commitString().isEmpty() && m_preedit) {
            QInputMethodEvent commit;
            commit.setCommitString(event->commitString(), event->replacementStart(), event->replacementLength());
            ScintillaEditBase::inputMethodEvent(&commit);
            QInputMethodEvent preedit(event->preeditString(), event->attributes());
            ScintillaEditBase::inputMethodEvent(&preedit);
        } else ScintillaEditBase::inputMethodEvent(event);
    }
    if (m_session) m_session->inputFinished();
}

void SourceEditor::keyPressEvent(QKeyEvent *event) {
    if (!m_session || m_session->isClosing() || m_session->isBusy()) { event->ignore(); return; }
    if (event->matches(QKeySequence::Undo)) { m_session->undo(); event->accept(); return; }
    if (event->matches(QKeySequence::Redo)) { m_session->redo(); event->accept(); return; }
    if (event->matches(QKeySequence::Paste)) {
        const auto *mime = QApplication::clipboard()->mimeData();
        if (mime && mime->hasImage() && !mime->hasText()) { emit imagePasteRequested(QApplication::clipboard()->image()); event->accept(); return; }
    }
    if (!DocumentSession::validText(event->text())) { m_session->reportRejection(EditResult::InvalidText); event->accept(); return; }
    {
        QScopedValueRollback<int> depth(m_inputDepth, m_inputDepth + 1);
        ScintillaEditBase::keyPressEvent(event);
    }
    if (m_session) m_session->inputFinished();
}
}
