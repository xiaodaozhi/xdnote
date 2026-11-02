#include "SingleFileWindow.h"
#include "SourceEditor.h"
#include "MarkdownPreview.h"
#include "VisualTokens.h"
#include "InputModality.h"
#include "Utf8Text.h"
#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyleOptionButton>
#include <QStylePainter>
#include <QSvgRenderer>
#include <QVBoxLayout>
#include <QWindow>
#include <QScrollArea>
#include <QScreen>

namespace xdnote {
namespace {
int token(const char *section, const char *key) { return visualTokens().value(section).toObject().value(key).toInt(); }
QJsonObject windowGeometry() { return visualTokens().value("windows").toObject().value("standalone").toObject(); }
QString color(const char *key) { return visualTokens().value("themes").toObject().value("light").toObject().value(key).toString(); }

class ActionButton final : public QPushButton {
public:
    ActionButton(const QString &text, const QString &name, QWidget *parent = nullptr) : QPushButton(text, parent) {
        setObjectName(name); setAccessibleName(text); setFocusPolicy(Qt::StrongFocus); setAutoDefault(false);
        setFixedHeight(token("controls", "height"));
        setMinimumWidth(token("controls", "iconButtonHit"));
        connect(InputModality::forApplication(), &InputModality::changed, this, [this] { update(); });
        connect(this, &QPushButton::pressed, this, [this] { m_keyboard = false; update(); });
    }
protected:
    void focusInEvent(QFocusEvent *event) override {
        m_keyboard = event->reason() == Qt::TabFocusReason || event->reason() == Qt::BacktabFocusReason;
        QPushButton::focusInEvent(event);
    }
    void focusOutEvent(QFocusEvent *event) override { m_keyboard = false; QPushButton::focusOutEvent(event); }
    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_Escape) {
            m_keyboard = false; update();
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) { click(); event->accept(); return; }
        QPushButton::keyPressEvent(event);
    }
    void paintEvent(QPaintEvent *) override {
        QStylePainter painter(this); QStyleOptionButton option; initStyleOption(&option);
        option.state &= ~QStyle::State_HasFocus;
        painter.drawControl(QStyle::CE_PushButton, option);
        if (m_keyboard && hasFocus() && InputModality::forApplication()->keyboardNavigation()) {
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(QColor(visualTokens().value("accents").toObject().value("graphite").toObject().value("base").toString()), token("controls", "focusRingWidth")));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(rect().adjusted(2, 2, -2, -2), token("radius", "control"), token("radius", "control"));
        }
    }
private:
    bool m_keyboard = false;
};

int ask(QWidget *owner, const QString &name, const QString &title, const QString &text,
        const QList<QPair<QString, int>> &actions, const QString &widthToken = "unsavedConfirmation") {
    QDialog dialog(owner); dialog.setObjectName(name); dialog.setWindowTitle(title);
    dialog.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    const auto size = visualTokens().value("windows").toObject().value(widthToken).toObject();
    dialog.setMinimumWidth(size.value("minWidth").toInt());
    dialog.resize(size.value("preferredWidth").toInt(), 180);
    auto *layout = new QVBoxLayout(&dialog); layout->setContentsMargins(24, 24, 24, 24); layout->setSpacing(16);
    auto *heading = new QLabel(title, &dialog); heading->setTextFormat(Qt::PlainText);
    auto headingFont = heading->font(); headingFont.setBold(true); headingFont.setPixelSize(18); heading->setFont(headingFont);
    auto *body = new QLabel(text, &dialog); body->setTextFormat(Qt::PlainText); body->setWordWrap(true);
    body->setTextInteractionFlags(Qt::TextSelectableByMouse); body->setMinimumWidth(0);
    layout->addWidget(heading);
    auto *scroll = new QScrollArea(&dialog); scroll->setFrameShape(QFrame::NoFrame); scroll->setWidgetResizable(true); scroll->setWidget(body);
    const int width = size.value("preferredWidth").toInt() - 48;
    const int textHeight = QFontMetrics(body->font()).boundingRect(QRect(0, 0, width, 100000), Qt::TextWordWrap, text).height();
    const int cap = qMax(80, int(owner->screen()->availableGeometry().height() * 0.5));
    body->setMinimumHeight(textHeight + 4); scroll->setFixedHeight(qMin(cap, textHeight + 8)); layout->addWidget(scroll);
    auto *buttons = new QDialogButtonBox(&dialog);
    for (const auto &action : actions) {
        auto *button = new ActionButton(action.first, QStringLiteral("choice%1").arg(action.second), &dialog);
        buttons->addButton(button, action.second == 0 ? QDialogButtonBox::RejectRole
            : action.second == 2 ? QDialogButtonBox::DestructiveRole : QDialogButtonBox::AcceptRole);
        if (action.second == 0) { button->setDefault(true); button->setFocus(Qt::OtherFocusReason); }
        QObject::connect(button, &QPushButton::clicked, &dialog, [&dialog, value = action.second] { dialog.done(value); });
    }
    layout->addWidget(buttons); dialog.adjustSize();
    return dialog.exec();
}
}

class DocumentHeader final : public QWidget {
public:
    explicit DocumentHeader(QWidget *parent) : QWidget(parent), m_logo(QStringLiteral(":/xdnote/design/assets/ui-v2/brand/logo-light.svg")) {
        setFixedHeight(windowGeometry().value("titlebarHeight").toInt()); setObjectName("documentHeader");
    }
    void setTitle(const QString &title, bool dirty) { m_title = title; m_dirty = dirty; setToolTip(title); setAccessibleName("xdnote · " + title); update(); }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this); painter.fillRect(rect(), QColor(color("sidebar")));
        const qreal height = visualTokens().value("brand").toObject().value("displayHeight").toDouble(28);
        const qreal width = height * m_logo.viewBoxF().width() / m_logo.viewBoxF().height();
        m_logo.render(&painter, QRectF(16, (this->height() - height) / 2, width, height));
        painter.setPen(QColor(color("border"))); painter.drawLine(QPointF(32 + width, 14), QPointF(32 + width, this->height() - 14));
        const qreal left = 48 + width;
        QFont titleFont = font(); titleFont.setPixelSize(visualTokens().value("typography").toObject().value("ui").toObject().value("size").toInt());
        painter.setFont(titleFont); QFontMetricsF metrics(titleFont);
        const auto title = metrics.elidedText(m_title, Qt::ElideMiddle, qMax(0.0, this->width() - left - 36));
        // Align actual visible glyphs, rather than unrelated font line boxes.
        const auto ink = metrics.tightBoundingRect(title);
        painter.setPen(QColor(color("text")));
        painter.drawText(QPointF(left, this->height() / 2.0 - ink.center().y()), title);
        if (m_dirty) { painter.setPen(Qt::NoPen); painter.setBrush(QColor(visualTokens().value("semantic").toObject().value("light").toObject().value("warning").toString())); painter.drawEllipse(QPointF(left + metrics.horizontalAdvance(title) + 12, this->height() / 2.0), 3, 3); }
        painter.setPen(QColor(color("border"))); painter.drawLine(0, this->height() - 1, this->width(), this->height() - 1);
    }
    void mousePressEvent(QMouseEvent *event) override { if (event->button() == Qt::LeftButton && window()->windowHandle()) window()->windowHandle()->startSystemMove(); }
    void mouseDoubleClickEvent(QMouseEvent *event) override { if (event->button() == Qt::LeftButton) window()->isMaximized() ? window()->showNormal() : window()->showMaximized(); }
private:
    QSvgRenderer m_logo;
    QString m_title = QStringLiteral("打开 Markdown 文档");
    bool m_dirty = false;
};

SingleFileWindow::SingleFileWindow(FileManager *manager) : m_manager(manager) {
    setObjectName("standaloneWindow"); setAttribute(Qt::WA_DeleteOnClose); setAcceptDrops(true);
    const auto size = windowGeometry(); resize(size.value("defaultWidth").toInt(), size.value("defaultHeight").toInt());
    setMinimumSize(size.value("minWidth").toInt(), size.value("minHeight").toInt());
    QFont ui = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    ui.setPixelSize(visualTokens().value("typography").toObject().value("ui").toObject().value("size").toInt()); setFont(ui);
    setStyleSheet(QStringLiteral("QWidget#standaloneWindow,QDialog{background:%1;color:%2} QPushButton{padding:0 12px;border:0;border-radius:6px;background:transparent;color:%2} QPushButton:hover{background:%3} QPushButton:checked,QPushButton[primary=\"true\"]{background:#374151;color:white} QPushButton:disabled{color:%4} QMenu{background:%1;color:%2;padding:6px} QMenu::item{padding:7px 24px} QMenu::item:selected{background:%3}")
        .arg(color("background"), color("text"), color("codeSurface"), color("textSecondary")));
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
    m_header = new DocumentHeader(this); layout->addWidget(m_header);
    auto *toolbar = new QWidget(this); toolbar->setObjectName("documentToolbar"); toolbar->setFixedHeight(size.value("toolbarHeight").toInt());
    auto *bar = new QGridLayout(toolbar); bar->setContentsMargins(16, 0, 16, 0); bar->setHorizontalSpacing(16);
    bar->setColumnStretch(0, 1); bar->setColumnStretch(2, 1);
    m_path = new QLabel(toolbar); m_path->setTextFormat(Qt::PlainText); m_path->setMinimumWidth(0); m_path->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); bar->addWidget(m_path, 0, 0);
    auto *modeBox = new QWidget(toolbar); auto *modeLayout = new QHBoxLayout(modeBox); modeLayout->setContentsMargins(0, 0, 0, 0); modeLayout->setSpacing(0);
    m_read = new ActionButton(QStringLiteral("阅读"), "readMode", toolbar); m_read->setCheckable(true);
    m_edit = new ActionButton(QStringLiteral("编辑"), "editMode", toolbar); m_edit->setCheckable(true);
    auto *modes = new QButtonGroup(this); modes->setExclusive(true); modes->addButton(m_read, 0); modes->addButton(m_edit, 1);
    m_read->setFixedWidth(token("controls", "modeSegmentWidth")); m_edit->setFixedWidth(token("controls", "modeSegmentWidth"));
    modeLayout->addWidget(m_read); modeLayout->addWidget(m_edit); bar->addWidget(modeBox, 0, 1);
    auto *actions = new QWidget(toolbar); auto *right = new QHBoxLayout(actions); right->setContentsMargins(0, 0, 0, 0); right->setSpacing(8); right->addStretch();
    m_save = new ActionButton(QStringLiteral("保存"), "saveButton", toolbar); m_save->setProperty("primary", true); right->addWidget(m_save);
    auto *more = new ActionButton(QStringLiteral("⋯"), "moreActions", toolbar); more->setAccessibleName(QStringLiteral("文档操作")); more->setToolTip(QStringLiteral("文档操作")); more->setStyleSheet("QPushButton::menu-indicator{image:none;width:0}"); right->addWidget(more); bar->addWidget(actions, 0, 2);
    m_menu = new QMenu(this);
    auto action = [this](const QString &text, QKeySequence shortcut, auto callback) {
        auto *item = m_menu->addAction(text); if (!shortcut.isEmpty()) item->setShortcut(shortcut);
        addAction(item); connect(item, &QAction::triggered, this, callback); return item;
    };
    action(QStringLiteral("打开…"), QKeySequence::Open, [this] { chooseOpen(); });
    m_saveAction = action(QStringLiteral("保存"), QKeySequence::Save, [this] { if (m_document && !m_document->save()) { if (m_document->conflict()) resolveConflict(); } });
    m_saveAsAction = action(QStringLiteral("另存为…"), QKeySequence::SaveAs, [this] { chooseSaveAs(); });
    action(QStringLiteral("重新载入磁盘版本…"), {}, [this] { reloadDisk(); });
    m_menu->addSeparator();
    m_undoAction = action(QStringLiteral("撤销"), QKeySequence::Undo, [this] { if (m_document) m_document->session()->undo(); });
    m_redoAction = action(QStringLiteral("重做"), QKeySequence::Redo, [this] { if (m_document) m_document->session()->redo(); });
    m_menu->addSeparator();
    action(QStringLiteral("关闭文档"), QKeySequence::Close, [this] { close(); });
    action(QStringLiteral("退出 xdnote"), QKeySequence::Quit, [this] { emit quitRequested(); });
    more->setMenu(m_menu);
    connect(modes, &QButtonGroup::idClicked, this, [this](int mode) { setEditing(mode == 1); });
    connect(m_save, &QPushButton::clicked, m_saveAction, &QAction::trigger);
    layout->addWidget(toolbar);
    m_banner = new QWidget(this); auto *banner = new QHBoxLayout(m_banner); banner->setContentsMargins(16, 8, 16, 8);
    m_notice = new QLabel(m_banner); m_notice->setObjectName("documentNotice"); m_notice->setTextFormat(Qt::PlainText);
    m_notice->setWordWrap(true); m_notice->setMinimumWidth(0); m_notice->setTextInteractionFlags(Qt::TextSelectableByMouse);
    banner->addWidget(m_notice, 1);
    m_resolve = new ActionButton(QStringLiteral("处理"), "resolveConflict", m_banner); banner->addWidget(m_resolve);
    connect(m_resolve, &QPushButton::clicked, this, [this] { if (m_document && m_document->conflict()) resolveConflict(); else chooseSaveAs(); });
    layout->addWidget(m_banner);
    m_body = new QStackedWidget(this);
    auto *welcome = new QWidget(m_body); auto *welcomeLayout = new QVBoxLayout(welcome); welcomeLayout->setContentsMargins(40, 40, 40, 40);
    welcomeLayout->addStretch(); m_empty = new QLabel(QStringLiteral("打开一个 Markdown 文件\n从阅读开始，随时切换编辑。"), welcome);
    m_empty->setAlignment(Qt::AlignCenter); m_empty->setWordWrap(true); m_empty->setTextFormat(Qt::PlainText); welcomeLayout->addWidget(m_empty);
    auto *welcomeOpen = new ActionButton(QStringLiteral("选择文件…"), "welcomeOpen", welcome); connect(welcomeOpen, &QPushButton::clicked, this, &SingleFileWindow::chooseOpen);
    welcomeLayout->addWidget(welcomeOpen, 0, Qt::AlignHCenter); welcomeLayout->addStretch(); m_body->addWidget(welcome); layout->addWidget(m_body, 1);
    auto *footer = new QWidget(this); footer->setFixedHeight(size.value("statusbarHeight").toInt()); auto *foot = new QHBoxLayout(footer);
    QFont secondary = ui; secondary.setPixelSize(visualTokens().value("typography").toObject().value("secondary").toObject().value("size").toInt()); footer->setFont(secondary);
    foot->setContentsMargins(16, 0, 16, 0); m_status = new QLabel(footer); m_status->setObjectName("documentStatus"); foot->addWidget(m_status, 1);
    m_encoding = new QLabel(footer); foot->addWidget(m_encoding); layout->addWidget(footer);
    setWindowTitle(QStringLiteral("xdnote")); qApp->installEventFilter(this); updateState();
}
void SingleFileWindow::setLoading(const QString &path, const QUuid &intent) {
    m_intent = intent; m_header->setTitle(QFileInfo(path).fileName(), false); m_path->setText(QFileInfo(path).path());
    m_empty->setText(QStringLiteral("正在打开…\n") + QFileInfo(path).fileName()); setWindowTitle(QStringLiteral("正在打开 — xdnote"));
    m_status->setText(QStringLiteral("正在打开…"));
}
void SingleFileWindow::showOpenError(const QString &path, const QString &message) {
    m_intent = {}; m_empty->setText(QStringLiteral("无法打开文件\n\n") + message + "\n\n" + path); m_status->setText(QStringLiteral("打开失败"));
}
void SingleFileWindow::setDocument(FileDocument *document) {
    if (m_document) return;
    m_intent = {}; m_document = document;
    m_preview = new MarkdownPreview(document->session(), m_body, windowGeometry().value("readerPadding").toInt()); m_body->addWidget(m_preview);
    m_editorPage = new QWidget(m_body); auto *layout = new QVBoxLayout(m_editorPage); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
    auto *format = new QWidget(m_editorPage); format->setFixedHeight(visualTokens().value("windows").toObject().value("main").toObject().value("sourceToolbarHeight").toInt());
    auto *tools = new QHBoxLayout(format); tools->setContentsMargins(16, 0, 16, 0); tools->setSpacing(8); tools->addWidget(new QLabel("Markdown", format));
    const QList<QPair<QString, QPair<QString, QString>>> formats{{"B", {"**", "**"}}, {"I", {"*", "*"}}, {"H", {"## ", ""}}, {QStringLiteral("链接"), {"[", "](url)"}}, {QStringLiteral("任务"), {"- [ ] ", ""}}, {QStringLiteral("代码"), {"`", "`"}}};
    int index = 0;
    const QStringList formatNames{QStringLiteral("加粗"), QStringLiteral("斜体"), QStringLiteral("二级标题"), QStringLiteral("插入链接"), QStringLiteral("任务列表"), QStringLiteral("行内代码")};
    for (const auto &entry : formats) {
        auto *button = new ActionButton(entry.first, QStringLiteral("format%1").arg(index++), format);
        button->setAccessibleName(formatNames[index - 1]); button->setToolTip(formatNames[index - 1]);
        connect(button, &QPushButton::clicked, this, [this, wrap = entry.second] { wrapSelection(wrap.first, wrap.second); }); tools->addWidget(button);
    }
    tools->addStretch(); layout->addWidget(format);
    m_editor = new SourceEditor(document->session(), m_editorPage); m_editor->setObjectName("sourceEditor");
    m_editor->send(SCI_SETMARGINWIDTHN, 0, token("controls", "lineNumberWidth")); m_editor->send(SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
    const auto bytes = document->session()->snapshot().utf8;
    const int firstLf = int(bytes.indexOf('\n')), firstCr = int(bytes.indexOf('\r'));
    m_editor->send(SCI_SETEOLMODE, firstCr >= 0 && (firstLf < 0 || firstCr < firstLf) ? (firstLf == firstCr + 1 ? SC_EOL_CRLF : SC_EOL_CR) : SC_EOL_LF);
    layout->addWidget(m_editor, 1); m_body->addWidget(m_editorPage);
    connect(document, &FileDocument::stateChanged, this, &SingleFileWindow::updateState);
    connect(document, &QObject::destroyed, this, &SingleFileWindow::updateState);
    connect(document, &FileDocument::operationFinished, this, [this](bool success) {
        if (!m_closeAfterOperation) return;
        m_closeAfterOperation = false;
        if (success) QTimer::singleShot(0, this, [this] { close(); }); else emit closeCancelled();
    });
    connect(m_preview, &MarkdownPreview::previewError, this, [this](const QString &message) { m_previewError = message; updateState(); });
    connect(m_preview, &MarkdownPreview::previewReady, this, [this] { m_previewError.clear(); updateState(); });
    setEditing(false); updateState();
}
void SingleFileWindow::setEditing(bool editing) {
    if (!m_document || (editing && m_document->readOnly())) { updateState(); return; }
    m_editing = editing; m_body->setCurrentWidget(editing ? m_editorPage : static_cast<QWidget *>(m_preview));
    if (editing) m_editor->setFocus(Qt::OtherFocusReason);
    updateState();
}
void SingleFileWindow::updateState() {
    const bool ready = m_document && !m_document->closed();
    const bool busy = ready && (m_document->saving() || m_document->loading());
    m_read->setEnabled(ready); m_edit->setEnabled(ready && !m_document->readOnly()); m_read->setChecked(!m_editing); m_edit->setChecked(m_editing);
    m_save->setEnabled(ready && !busy && (!m_document->readOnly() || m_document->conflict()));
    m_saveAction->setEnabled(m_save->isEnabled()); m_saveAsAction->setEnabled(ready && !busy);
    m_undoAction->setEnabled(ready && m_editing && !m_document->readOnly()); m_redoAction->setEnabled(m_undoAction->isEnabled());
    if (!ready) { m_banner->hide(); m_status->setText(QStringLiteral("请选择文档")); return; }
    const auto path = m_document->path();
    m_header->setTitle(QFileInfo(path).fileName(), m_document->dirty());
    setWindowTitle(QFileInfo(path).fileName() + QStringLiteral("[*] — xdnote")); setWindowModified(m_document->dirty());
    const auto parent = QFileInfo(path).path(); m_path->setText(fontMetrics().elidedText(parent, Qt::ElideMiddle, qMax(40, m_path->width())));
    m_path->setToolTip(parent); m_path->setAccessibleName(parent); m_status->setText(m_document->statusText());
    m_encoding->setText(m_document->bom() ? QStringLiteral("UTF-8 BOM    100%") : QStringLiteral("UTF-8    100%"));
    auto message = m_document->message();
    if (message.isEmpty() && m_document->session()) message = m_document->session()->rejection();
    if (message.isEmpty()) message = m_previewError;
    m_notice->setText(message); m_banner->setVisible(!message.isEmpty()); m_resolve->setEnabled(!busy);
}
void SingleFileWindow::chooseOpen() {
    const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("打开 Markdown 文件"), {}, QStringLiteral("Markdown 文档 (*.md *.markdown);;所有文件 (*)"));
    if (!path.isEmpty()) emit openRequested(path);
}
void SingleFileWindow::chooseSaveAs() {
    if (!m_document || m_document->saving() || m_document->loading()) return;
    const auto path = QFileDialog::getSaveFileName(this, QStringLiteral("另存为"), m_document->path(), QStringLiteral("Markdown 文档 (*.md *.markdown);;所有文件 (*)"));
    if (!path.isEmpty()) m_document->saveAs(path);
}
void SingleFileWindow::reloadDisk() {
    if (!m_document || m_document->saving() || m_document->loading()) return;
    if (m_document->dirty()) {
        const auto choice = ask(this, "reloadConfirmation", QStringLiteral("重新载入磁盘版本？"), QStringLiteral("当前未保存的编辑将被磁盘内容替换。"), {{QStringLiteral("重新载入"), 2}, {QStringLiteral("取消"), 0}}, "externalConflict");
        if (choice != 2) return;
    }
    m_document->reload();
}
void SingleFileWindow::resolveConflict() {
    if (!m_document || m_document->saving() || m_document->loading()) return;
    const auto choice = ask(this, "conflictDialog", QStringLiteral("文件在外部发生变化"), m_document->message(),
        {{QStringLiteral("保留当前编辑"), 0}, {QStringLiteral("重新载入"), 2}, {QStringLiteral("另存为…"), 1}}, "externalConflict");
    if (choice == 2) reloadDisk(); else if (choice == 1) chooseSaveAs();
}
void SingleFileWindow::wrapSelection(const QString &before, const QString &after) {
    if (!m_document || !m_editing || !m_editor) return;
    const auto snapshot = m_document->session()->snapshot(); if (snapshot.result != EditResult::Applied) return;
    const auto start = qsizetype(m_editor->send(SCI_GETSELECTIONSTART)), end = qsizetype(m_editor->send(SCI_GETSELECTIONEND));
    const auto first = decodeUtf8Literal(snapshot.utf8.left(start));
    const auto selected = decodeUtf8Literal(snapshot.utf8.mid(start, end - start));
    if (m_document->session()->replace(int(first.size()), int(first.size() + selected.size()), before + selected + after, snapshot.revision) == EditResult::Applied) {
        m_editor->send(SCI_SETSEL, uptr_t(start + before.toUtf8().size()), sptr_t(end + before.toUtf8().size()));
        m_editor->setFocus(Qt::OtherFocusReason);
    }
}
void SingleFileWindow::closeEvent(QCloseEvent *event) {
    if (m_prompting) { event->ignore(); return; }
    if (!m_document) { if (!m_intent.isNull()) emit cancelOpenRequested(m_intent); event->accept(); return; }
    if (m_document->closed()) { event->accept(); return; }
    if (m_document->session()->isComposing()) { m_document->session()->reportRejection(EditResult::Composing); event->ignore(); emit closeCancelled(); return; }
    if (m_document->saving() || m_document->loading()) {
        m_prompting = true;
        const int choice = ask(this, "operationCloseConfirmation", QStringLiteral("文件操作尚未结束"), QStringLiteral("可以等待操作结束后关闭，或取消本次关闭。"), {{QStringLiteral("等待后关闭"), 1}, {QStringLiteral("取消关闭"), 0}});
        m_prompting = false; m_closeAfterOperation = choice == 1;
        if (!m_closeAfterOperation) emit closeCancelled();
        else if (!m_document->saving() && !m_document->loading()) { m_closeAfterOperation = false; QTimer::singleShot(0, this, [this] { close(); }); }
        event->ignore(); return;
    }
    if (m_document->dirty()) {
        const auto revision = m_document->session()->revision();
        m_prompting = true;
        const int choice = ask(this, "unsavedConfirmation", QStringLiteral("保存对文档的更改？"), QFileInfo(m_document->path()).fileName() + QStringLiteral(" 尚有未保存的编辑。"),
            {{QStringLiteral("保存"), 1}, {QStringLiteral("不保存"), 2}, {QStringLiteral("取消"), 0}});
        m_prompting = false;
        if (choice == 0 || !m_document || (choice == 2 && m_document->session()->revision() != revision)) { emit closeCancelled(); event->ignore(); return; }
        if (choice == 1) {
            m_closeAfterOperation = true;
            if (!m_document->save()) { m_closeAfterOperation = false; emit closeCancelled(); if (m_document->conflict()) resolveConflict(); }
            event->ignore(); return;
        }
    }
    if (m_manager->closeDocument(m_document)) event->accept(); else { event->ignore(); emit closeCancelled(); }
}
bool SingleFileWindow::eventFilter(QObject *target, QEvent *event) {
    auto *widget = qobject_cast<QWidget *>(target);
    if (!widget || (widget != this && !isAncestorOf(widget))) return QWidget::eventFilter(target, event);
    if (event->type() == QEvent::DragEnter) {
        auto *drag = static_cast<QDragEnterEvent *>(event);
        if (drag->mimeData()->hasUrls()) { drag->acceptProposedAction(); return true; }
    }
    if (event->type() == QEvent::Drop) {
        auto *drop = static_cast<QDropEvent *>(event);
        const auto urls = drop->mimeData()->urls();
        if (!urls.isEmpty() && urls.size() <= 16) { for (const auto &url : urls) if (url.isLocalFile()) emit openRequested(url.toLocalFile()); drop->acceptProposedAction(); return true; }
    }
    if (event->type() == QEvent::WindowActivate && m_document) m_document->checkExternal();
    if (widget == this && event->type() == QEvent::Resize) QTimer::singleShot(0, this, &SingleFileWindow::updateState);
    return QWidget::eventFilter(target, event);
}
}
