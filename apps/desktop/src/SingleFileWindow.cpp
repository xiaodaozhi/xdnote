#include "SingleFileWindow.h"
#include "Appearance.h"
#include "SharedWidgets.h"
#include "SourceEditor.h"
#include "MarkdownPreview.h"
#include "VisualTokens.h"
#include "InputModality.h"
#include "Utf8Text.h"
#include "WindowGeometry.h"
#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QDialog>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QPainter>
#include <QScreen>
#include <QScrollArea>
#include <QStackedWidget>
#include <QSvgRenderer>
#include <QVBoxLayout>
#include <QWebEngineView>
#include <QWindow>

namespace xdnote {
namespace {
int token(const char *section, const char *key) { return visualTokens().value(section).toObject().value(key).toInt(); }
QJsonObject geometryTokens() { return visualTokens().value("windows").toObject().value("standalone").toObject(); }
void separator(QBoxLayout *layout, Appearance *appearance, QWidget *owner) {
    auto *line = new QWidget(owner); line->setFixedWidth(1); line->setFixedHeight(20);
    const auto refresh = [line, appearance] { line->setStyleSheet("background:" + appearance->color("border").name()); };
    QObject::connect(appearance, &Appearance::changed, line, refresh); refresh(); layout->addWidget(line);
}
int ask(SingleFileWindow *owner, const QString &name, const QString &title, const QString &text,
        const QList<QPair<QString, int>> &actions, const QString &widthToken = "unsavedConfirmation") {
    auto *appearance = owner->appearance();
    const QPointer<QWidget> caller(owner->focusWidget());
    QDialog dialog(owner); dialog.setObjectName(name); dialog.setWindowTitle(title);
    dialog.setFont(owner->font());
    dialog.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    const auto size = visualTokens().value("windows").toObject().value(widthToken).toObject();
    const int width = qMin(size.value("preferredWidth").toInt(), qMax(200, owner->screen()->availableGeometry().width() - 32));
    dialog.setFixedWidth(width);
    auto *layout = new QVBoxLayout(&dialog); layout->setContentsMargins(24, 24, 24, 24); layout->setSpacing(16);
    auto *symbol = new QLabel(&dialog); symbol->setPixmap(themedIcon("document", appearance->color("text")).pixmap(QSize(24, 24), dialog.devicePixelRatioF())); layout->addWidget(symbol);
    auto *heading = new QLabel(title, &dialog); heading->setTextFormat(Qt::PlainText); heading->setWordWrap(true);
    auto font = heading->font(); font.setBold(true); font.setPixelSize(visualTokens().value("typography").toObject().value("headings").toObject().value("dialog").toInt()); heading->setFont(font); layout->addWidget(heading);
    auto *body = new QLabel(text, &dialog); body->setTextFormat(Qt::PlainText); body->setWordWrap(true);
    body->setMinimumWidth(0); body->setTextInteractionFlags(Qt::TextSelectableByMouse);
    const int textHeight = QFontMetrics(body->font()).boundingRect(QRect(0, 0, width - 48, 100000), Qt::TextWordWrap, text).height();
    const int cap = qMax(80, owner->screen()->availableGeometry().height() / 3);
    if (textHeight > cap) {
        auto *scroll = new QScrollArea(&dialog); scroll->setFrameShape(QFrame::NoFrame); scroll->setWidgetResizable(true);
        body->setMinimumHeight(textHeight + 8); scroll->setWidget(body); scroll->setFixedHeight(cap); layout->addWidget(scroll);
    } else layout->addWidget(body);
    if (owner->document()) {
        auto *card = new QWidget(&dialog); card->setObjectName("dialogFile");
        auto *row = new QHBoxLayout(card); row->setContentsMargins(12, 10, 12, 10); row->setSpacing(12);
        auto *icon = new QLabel(card); icon->setPixmap(themedIcon("document", appearance->color("text")).pixmap(QSize(20, 20), dialog.devicePixelRatioF())); row->addWidget(icon);
        auto *details = new QVBoxLayout; details->setSpacing(4);
        auto *filename = new ElidedLabel(card); filename->setFullText(QFileInfo(owner->document()->path()).fileName()); details->addWidget(filename);
        auto *path = new ElidedLabel(card); auto pathFont = owner->font(); pathFont.setPixelSize(visualTokens().value("typography").toObject().value("secondary").toObject().value("size").toInt()); path->setFont(pathFont); path->setFullText(QFileInfo(owner->document()->path()).path()); path->setProperty("secondary", true); details->addWidget(path);
        row->addLayout(details, 1); layout->addWidget(card);
    }
    auto *buttons = new QHBoxLayout; buttons->setSpacing(8);
    auto add = [&](const QPair<QString, int> &entry) {
        auto *button = new SharedButton(entry.first, QStringLiteral("choice%1").arg(entry.second), appearance, &dialog);
        const bool destructive = entry.second == 2 && name == "reloadConfirmation";
        button->setProperty("primary", entry.second == 1 || destructive); button->setProperty("danger", destructive);
        button->setOutlined(entry.second == 0); buttons->addWidget(button);
        if (entry.second == 0) { button->setDefault(true); button->setFocus(Qt::OtherFocusReason); }
        QObject::connect(button, &QPushButton::clicked, &dialog, [&dialog, value = entry.second] { dialog.done(value); });
    };
    if (name == "unsavedConfirmation") {
        for (const auto &entry : actions) if (entry.second == 2) add(entry);
        buttons->addStretch();
        for (const int id : {0, 1}) for (const auto &entry : actions) if (entry.second == id) add(entry);
    } else {
        buttons->addStretch();
        for (const auto &entry : actions) if (entry.second == 0) add(entry);
        for (const auto &entry : actions) if (entry.second != 0) add(entry);
    }
    layout->addSpacing(4); layout->addLayout(buttons); dialog.adjustSize();
    const int result = dialog.exec(); InputModality::forApplication()->clear();
    if (caller && caller->isVisible() && caller->isEnabled()) caller->setFocus(Qt::PopupFocusReason);
    // Dialog teardown can perform another focus transition. Restore the target
    // after teardown too, without forcing activation over another application.
    QTimer::singleShot(0, owner, [caller] { if (caller && caller->isVisible() && caller->isEnabled()) caller->setFocus(Qt::PopupFocusReason); });
    return result;
}
}

class DocumentHeader final : public QWidget {
public:
    DocumentHeader(Appearance *appearance, QWidget *parent) : QWidget(parent), m_appearance(appearance) {
        setFixedHeight(geometryTokens().value("titlebarHeight").toInt()); setObjectName("documentHeader");
        auto refresh = [this] { m_logo.load(QStringLiteral(":/xdnote/design/assets/ui-v2/brand/logo-%1.svg").arg(m_appearance->scheme())); update(); };
        connect(appearance, &Appearance::changed, this, refresh); refresh();
    }
    void setTitle(const QString &title, bool dirty) {
        m_title = title; m_dirty = dirty; setToolTip(title); setAccessibleName("xdnote · " + title + (dirty ? QStringLiteral("，未保存") : QString())); update();
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this); painter.fillRect(rect(), m_appearance->color("sidebar"));
        const qreal height = visualTokens().value("brand").toObject().value("displayHeight").toDouble();
        const qreal width = height * m_logo.viewBoxF().width() / m_logo.viewBoxF().height();
        m_logo.render(&painter, QRectF(16, (this->height() - height) / 2, width, height));
        painter.setPen(m_appearance->color("border")); painter.drawLine(QPointF(32 + width, 14), QPointF(32 + width, this->height() - 14));
        const qreal left = 48 + width; QFontMetricsF metrics(font());
        const auto title = metrics.elidedText(m_title, Qt::ElideMiddle, qMax(0.0, this->width() - left - 32));
        const auto ink = metrics.tightBoundingRect(title);
        painter.setFont(font()); painter.setPen(m_appearance->color("text"));
        painter.drawText(QPointF(left, this->height() / 2.0 - ink.center().y()), title);
        if (m_dirty) { painter.setPen(Qt::NoPen); painter.setBrush(m_appearance->semantic("warning")); painter.drawEllipse(QPointF(left + metrics.horizontalAdvance(title) + 12, this->height() / 2.0), 3, 3); }
        painter.setPen(m_appearance->color("border")); painter.drawLine(0, this->height() - 1, this->width(), this->height() - 1);
    }
    void mousePressEvent(QMouseEvent *event) override { if (event->button() == Qt::LeftButton && window()->windowHandle()) window()->windowHandle()->startSystemMove(); }
    void mouseDoubleClickEvent(QMouseEvent *event) override { if (event->button() == Qt::LeftButton) window()->isMaximized() ? window()->showNormal() : window()->showMaximized(); }
private:
    Appearance *m_appearance;
    QSvgRenderer m_logo;
    QString m_title;
    bool m_dirty = false;
};

SingleFileWindow::SingleFileWindow(FileManager *manager, Appearance *appearance, WindowGeometryStore *geometryStore)
    : m_manager(manager), m_appearance(appearance ? appearance : new Appearance(this)) {
    setObjectName("standaloneWindow"); setAttribute(Qt::WA_DeleteOnClose); setAcceptDrops(true);
    const auto size = geometryTokens(); resize(size.value("defaultWidth").toInt(), size.value("defaultHeight").toInt());
    setMinimumSize(size.value("minWidth").toInt(), size.value("minHeight").toInt());
    QFont ui = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    ui.setPixelSize(visualTokens().value("typography").toObject().value("ui").toObject().value("size").toInt()); setFont(ui);
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
    m_header = new DocumentHeader(m_appearance, this); layout->addWidget(m_header);
    auto *toolbar = new QWidget(this); toolbar->setObjectName("documentToolbar"); toolbar->setFixedHeight(size.value("toolbarHeight").toInt());
    auto *bar = new QGridLayout(toolbar); bar->setContentsMargins(16, 0, 16, 0); bar->setHorizontalSpacing(16); bar->setColumnStretch(0, 1); bar->setColumnStretch(2, 1);
    auto *location = new QWidget(toolbar); auto *pathRow = new QHBoxLayout(location); pathRow->setContentsMargins(0, 0, 0, 0); pathRow->setSpacing(8);
    m_pathButton = new SharedButton({}, "fileLocation", m_appearance, location); m_pathButton->setSymbol("folder-open"); m_pathButton->setOutlined(true);
    m_pathButton->setAccessibleName(QStringLiteral("文件位置")); m_pathButton->setToolTip(QStringLiteral("文件位置与完整路径")); m_pathButton->setFixedWidth(token("controls", "iconButtonHit")); pathRow->addWidget(m_pathButton);
    auto *pathMenu = new SharedMenu(m_appearance, m_pathButton); pathMenu->setObjectName("pathMenu"); pathMenu->setTitle(QStringLiteral("文件位置"));
    auto *copyPath = pathMenu->addAction(QStringLiteral("复制完整路径")); copyPath->setObjectName("copyPath");
    auto *reveal = pathMenu->addAction(QStringLiteral("打开所在文件夹")); reveal->setObjectName("revealFolder");
    connect(copyPath, &QAction::triggered, this, [this] { if (m_document) emit copyPathRequested(m_document->path()); });
    connect(reveal, &QAction::triggered, this, [this] { if (m_document) emit revealRequested(QFileInfo(m_document->path()).path()); }); m_pathButton->setMenu(pathMenu);
    m_path = new ElidedLabel(location); auto pathFont = ui; pathFont.setPixelSize(visualTokens().value("typography").toObject().value("secondary").toObject().value("size").toInt()); m_path->setFont(pathFont); m_path->setObjectName("documentPath"); m_path->setProperty("secondary", true); pathRow->addWidget(m_path, 1); bar->addWidget(location, 0, 0);
    auto *modeBox = new QWidget(toolbar); modeBox->setObjectName("modeSwitch"); modeBox->setFixedHeight(token("controls", "height")); auto *modeLayout = new QHBoxLayout(modeBox); modeLayout->setContentsMargins(0, 0, 0, 0); modeLayout->setSpacing(0);
    m_read = new SharedButton(QStringLiteral("阅读"), "readMode", m_appearance, modeBox); m_edit = new SharedButton(QStringLiteral("编辑"), "editMode", m_appearance, modeBox);
    m_read->setCheckable(true); m_edit->setCheckable(true); auto *modes = new QButtonGroup(this); modes->setExclusive(true); modes->addButton(m_read, 0); modes->addButton(m_edit, 1);
    m_read->setFixedWidth(token("controls", "modeSegmentWidth")); m_edit->setFixedWidth(token("controls", "modeSegmentWidth"));
    modeLayout->addWidget(m_read); modeLayout->addWidget(m_edit); bar->addWidget(modeBox, 0, 1);
    auto *actions = new QWidget(toolbar); auto *right = new QHBoxLayout(actions); right->setContentsMargins(0, 0, 0, 0); right->setSpacing(8); right->addStretch();
    m_save = new SharedButton(QStringLiteral("保存"), "saveButton", m_appearance, actions); m_save->setProperty("primary", true); right->addWidget(m_save);
    auto *more = new SharedButton({}, "moreActions", m_appearance, actions); more->setSymbol("more"); more->setAccessibleName(QStringLiteral("文档操作")); more->setToolTip(QStringLiteral("文档操作")); more->setFixedWidth(token("controls", "iconButtonHit")); right->addWidget(more); bar->addWidget(actions, 0, 2);
    m_menu = new SharedMenu(m_appearance, more); m_menu->setObjectName("documentMenu"); m_menu->setTitle(QStringLiteral("文档操作"));
    auto action = [this](const QString &text, const QString &name, QKeySequence shortcut, auto callback) {
        auto *item = m_menu->addAction(text); item->setObjectName(name); if (!shortcut.isEmpty()) item->setShortcut(shortcut);
        addAction(item); connect(item, &QAction::triggered, InputModality::forApplication(), &InputModality::clear);
        connect(item, &QAction::triggered, this, callback); return item;
    };
    action(QStringLiteral("打开…"), "openAction", QKeySequence::Open, [this] { chooseOpen(); });
    m_saveAction = action(QStringLiteral("保存"), "saveAction", QKeySequence::Save, [this] { if (m_document && !m_document->save() && m_document->conflict()) resolveConflict(); });
    m_saveAsAction = action(QStringLiteral("另存为…"), "saveAsAction", QKeySequence::SaveAs, [this] { chooseSaveAs(); });
    m_reloadAction = action(QStringLiteral("重新载入磁盘版本…"), "reloadAction", {}, [this] { reloadDisk(); });
    m_menu->addSeparator();
    m_undoAction = action(QStringLiteral("撤销"), "undoAction", QKeySequence::Undo, [this] { if (m_document) m_document->session()->undo(); });
    m_redoAction = action(QStringLiteral("重做"), "redoAction", QKeySequence::Redo, [this] { if (m_document) m_document->session()->redo(); });
    m_menu->addSeparator();
    auto *appearanceMenu = new SharedMenu(m_appearance, m_menu); appearanceMenu->setTitle(QStringLiteral("外观")); appearanceMenu->setObjectName("appearanceMenu"); m_menu->addMenu(appearanceMenu);
    auto *themes = new QActionGroup(this); themes->setExclusive(true);
    const QList<QPair<QString, QString>> themeNames{{"system", QStringLiteral("跟随系统")}, {"light", QStringLiteral("浅色")}, {"dark", QStringLiteral("深色")}};
    for (const auto &entry : themeNames) {
        auto *item = appearanceMenu->addAction(entry.second); item->setObjectName("theme-" + entry.first); item->setData(entry.first); item->setCheckable(true); themes->addAction(item); m_themeActions.append(item);
        connect(item, &QAction::triggered, this, [this, theme = entry.first] { m_appearance->setPreferences(theme, m_appearance->accent()); applyAppearance(); });
    }
    auto *accents = new SharedMenu(m_appearance, appearanceMenu); accents->setTitle(QStringLiteral("强调色")); accents->setObjectName("accentMenu"); appearanceMenu->addMenu(accents);
    auto *accentGroup = new QActionGroup(this); accentGroup->setExclusive(true);
    for (const auto &name : {"graphite", "mist-blue", "cobalt", "mauve", "terracotta", "olive"}) {
        const auto entry = visualTokens().value("accents").toObject().value(name).toObject();
        auto *item = accents->addAction(entry.value("label").toString() + "  " + entry.value("base").toString()); item->setObjectName("accent-" + QString::fromLatin1(name)); item->setData(QString::fromLatin1(name)); item->setCheckable(true);
        item->setIcon(themedIcon("palette", QColor(entry.value("base").toString()))); accentGroup->addAction(item); m_accentActions.append(item);
        connect(item, &QAction::triggered, this, [this, accent = QString::fromLatin1(name)] { m_appearance->setPreferences(m_appearance->theme(), accent); applyAppearance(); });
    }
    m_menu->addSeparator(); action(QStringLiteral("关闭文档"), "closeAction", QKeySequence::Close, [this] { close(); });
    action(QStringLiteral("退出 xdnote"), "quitAction", QKeySequence::Quit, [this] { emit quitRequested(); }); more->setMenu(m_menu);
    connect(modes, &QButtonGroup::idClicked, this, [this](int mode) { setEditing(mode == 1); }); connect(m_save, &QPushButton::clicked, m_saveAction, &QAction::trigger); layout->addWidget(toolbar);
    m_banner = new QWidget(this); m_banner->setObjectName("documentBanner"); auto *banner = new QHBoxLayout(m_banner); banner->setContentsMargins(16, 8, 16, 8); banner->setSpacing(8);
    m_notice = new QLabel(m_banner); m_notice->setObjectName("documentNotice"); m_notice->setTextFormat(Qt::PlainText); m_notice->setWordWrap(true); m_notice->setMinimumWidth(0); m_notice->setTextInteractionFlags(Qt::TextSelectableByMouse); banner->addWidget(m_notice, 1);
    m_resolve = new SharedButton(QStringLiteral("处理"), "resolveConflict", m_appearance, m_banner); m_retry = new SharedButton(QStringLiteral("重试"), "retrySave", m_appearance, m_banner); m_later = new SharedButton(QStringLiteral("稍后"), "dismissNotice", m_appearance, m_banner);
    banner->addWidget(m_retry); banner->addWidget(m_resolve); banner->addWidget(m_later);
    connect(m_resolve, &QPushButton::clicked, this, [this] { if (m_document && m_document->conflict()) { if (m_document->dirty()) resolveConflict(); else reloadDisk(); } else if (!m_previewError.isEmpty() && m_document && !m_document->hasError()) m_preview->refresh(); else chooseSaveAs(); });
    connect(m_retry, &QPushButton::clicked, m_saveAction, &QAction::trigger);
    connect(m_later, &QPushButton::clicked, this, [this] { m_dismissedNotice = m_notice->text(); m_banner->hide(); }); layout->addWidget(m_banner);
    m_body = new QStackedWidget(this); auto *welcome = new QWidget(m_body); auto *welcomeLayout = new QVBoxLayout(welcome); welcomeLayout->setContentsMargins(40, 40, 40, 40);
    welcomeLayout->addStretch(); m_empty = new QLabel(welcome); m_empty->setAlignment(Qt::AlignCenter); m_empty->setWordWrap(true); m_empty->setTextFormat(Qt::PlainText); welcomeLayout->addWidget(m_empty);
    auto *welcomeOpen = new SharedButton(QStringLiteral("选择文件…"), "welcomeOpen", m_appearance, welcome); welcomeOpen->setSymbol("folder-open"); welcomeOpen->setOutlined(true); connect(welcomeOpen, &QPushButton::clicked, this, &SingleFileWindow::chooseOpen);
    welcomeLayout->addWidget(welcomeOpen, 0, Qt::AlignHCenter); welcomeLayout->addStretch(); m_body->addWidget(welcome); layout->addWidget(m_body, 1);
    auto *footer = new QWidget(this); footer->setObjectName("documentFooter"); footer->setFixedHeight(size.value("statusbarHeight").toInt());
    QFont secondary = ui; secondary.setPixelSize(visualTokens().value("typography").toObject().value("secondary").toObject().value("size").toInt()); footer->setFont(secondary);
    auto *foot = new QHBoxLayout(footer); foot->setContentsMargins(16, 0, 16, 0); foot->setSpacing(8);
    m_statusIcon = new QLabel(footer); m_statusIcon->setFixedSize(16, 16); foot->addWidget(m_statusIcon);
    m_status = new ElidedLabel(footer); m_status->setObjectName("documentStatus"); foot->addWidget(m_status, 1);
    m_cursor = new QLabel(footer); m_cursor->setObjectName("cursorPosition"); foot->addWidget(m_cursor); separator(foot, m_appearance, footer);
    m_encoding = new QLabel(footer); foot->addWidget(m_encoding); separator(foot, m_appearance, footer);
    m_zoomButton = new SharedButton("100%", "contentZoom", m_appearance, footer); m_zoomButton->setFixedHeight(size.value("statusbarHeight").toInt() - 2); m_zoomButton->setAccessibleName(QStringLiteral("正文缩放，100%"));
    auto *zoomMenu = new SharedMenu(m_appearance, m_zoomButton); zoomMenu->setTitle(QStringLiteral("正文缩放")); auto *zoomGroup = new QActionGroup(zoomMenu);
    for (const int percent : {50, 75, 100, 125, 150, 200}) {
        auto *item = zoomMenu->addAction(QString::number(percent) + '%'); item->setCheckable(true); item->setChecked(percent == 100); zoomGroup->addAction(item);
        connect(item, &QAction::triggered, this, [this, percent] { setContentZoom(percent); });
    }
    m_zoomButton->setMenu(zoomMenu); foot->addWidget(m_zoomButton); layout->addWidget(footer);
    connect(m_appearance, &Appearance::changed, this, [this] { m_appearanceError.clear(); applyAppearance(); updateState(); });
    connect(m_appearance, &Appearance::preferenceFailed, this, [this](const QString &message) { m_appearanceError = message; updateState(); });
    m_appearanceError = m_appearance->error(); setWindowTitle(QStringLiteral("xdnote")); qApp->installEventFilter(this); applyAppearance(); updateState();
    m_geometry = new WindowGeometry(this, geometryStore, QStringLiteral("standalone"));
    connect(m_geometry, &WindowGeometry::geometryWarning, this, [this](const QString &message) { m_geometryError = message; updateState(); });
}

void SingleFileWindow::applyAppearance() {
    auto palette = this->palette(); palette.setColor(QPalette::Window, m_appearance->color("background")); palette.setColor(QPalette::Base, m_appearance->color("surface"));
    palette.setColor(QPalette::Text, m_appearance->color("text")); palette.setColor(QPalette::WindowText, m_appearance->color("text")); palette.setColor(QPalette::ButtonText, m_appearance->color("text"));
    palette.setColor(QPalette::Highlight, m_appearance->accentColor("soft")); palette.setColor(QPalette::HighlightedText, m_appearance->accentColor("softForeground")); setPalette(palette);
    setStyleSheet(QStringLiteral("QWidget#standaloneWindow,QDialog,QScrollArea,QScrollArea>QWidget>QWidget{background:%1;color:%2} QLabel{color:%2;background:transparent} QLabel[secondary=\"true\"]{color:%3} QWidget#documentToolbar,QWidget#formatToolbar,QWidget#documentFooter{background:%1;border-bottom:1px solid %4} QWidget#documentFooter{border-top:1px solid %4;border-bottom:0} QWidget#modeSwitch{border:1px solid %4;border-radius:6px;background:%5} QWidget#dialogFile{border:1px solid %4;border-radius:6px;background:%6} QWidget#documentBanner{background:%5;border-bottom:1px solid %4}")
        .arg(m_appearance->color("background").name(), m_appearance->color("text").name(), m_appearance->color("textSecondary").name(), m_appearance->color("border").name(), m_appearance->color("sidebar").name(), m_appearance->color("surface").name()) + themedScrollBars(m_appearance));
    for (auto *item : m_themeActions) item->setChecked(item->data().toString() == m_appearance->theme());
    for (auto *item : m_accentActions) item->setChecked(item->data().toString() == m_appearance->accent());
    update();
}
void SingleFileWindow::setLoading(const QString &path, const QUuid &intent) { m_intent = intent; m_openPath = path; m_openError.clear(); updateState(); }
void SingleFileWindow::showOpenError(const QString &path, const QString &message) { m_intent = {}; m_openPath = path; m_openError = message; updateState(); }
void SingleFileWindow::setDocument(FileDocument *document) {
    if (m_document) return;
    m_intent = {}; m_document = document;
    m_preview = new MarkdownPreview(document->session(), m_body, geometryTokens().value("readerPadding").toInt(), m_appearance); m_body->addWidget(m_preview);
    m_editorPage = new QWidget(m_body); auto *layout = new QVBoxLayout(m_editorPage); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
    auto *format = new QWidget(m_editorPage); format->setObjectName("formatToolbar"); format->setFixedHeight(visualTokens().value("windows").toObject().value("main").toObject().value("sourceToolbarHeight").toInt());
    auto *tools = new QHBoxLayout(format); tools->setContentsMargins(16, 0, 16, 0); tools->setSpacing(4); tools->addWidget(new QLabel("Markdown", format)); separator(tools, m_appearance, format);
    const QList<QPair<QString, QPair<QString, QString>>> formats{{"bold", {"**", "**"}}, {"italic", {"*", "*"}}, {"heading", {"## ", ""}}, {"link", {"[", "](url)"}}, {"checklist", {"- [ ] ", ""}}, {"code", {"`", "`"}}};
    const QStringList names{QStringLiteral("加粗"), QStringLiteral("斜体"), QStringLiteral("二级标题"), QStringLiteral("插入链接"), QStringLiteral("任务列表"), QStringLiteral("行内代码")};
    int index = 0;
    for (const auto &entry : formats) {
        auto *button = new SharedButton({}, QStringLiteral("format%1").arg(index), m_appearance, format); button->setSymbol(entry.first); button->setFixedWidth(token("controls", "iconButtonHit"));
        button->setAccessibleName(names[index]); button->setToolTip(names[index++]); m_formats.append(button);
        connect(button, &QPushButton::clicked, this, [this, wrap = entry.second] { wrapSelection(wrap.first, wrap.second); }); tools->addWidget(button);
    }
    tools->addStretch(); layout->addWidget(format);
    m_editor = new SourceEditor(document->session(), m_editorPage, m_appearance); m_editor->setObjectName("sourceEditor");
    m_editor->send(SCI_SETMARGINWIDTHN, 0, token("controls", "lineNumberWidth")); m_editor->send(SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
    const auto bytes = document->session()->snapshot().utf8; const int lf = int(bytes.indexOf('\n')), cr = int(bytes.indexOf('\r'));
    m_editor->send(SCI_SETEOLMODE, cr >= 0 && (lf < 0 || cr < lf) ? (lf == cr + 1 ? SC_EOL_CRLF : SC_EOL_CR) : SC_EOL_LF);
    connect(m_editor, &ScintillaEditBase::updateUi, this, [this] { updateCaret(); });
    layout->addWidget(m_editor, 1); m_body->addWidget(m_editorPage);
    connect(document, &FileDocument::stateChanged, this, &SingleFileWindow::updateState); connect(document, &QObject::destroyed, this, &SingleFileWindow::updateState);
    connect(document, &FileDocument::operationFinished, this, [this](bool success) { if (!m_closeAfterOperation) return; m_closeAfterOperation = false; if (success) QTimer::singleShot(0, this, [this] { close(); }); else emit closeCancelled(); });
    connect(m_preview, &MarkdownPreview::previewError, this, [this](const QString &message) { m_previewError = message; updateState(); });
    connect(m_preview, &MarkdownPreview::previewReady, this, [this] { m_previewError.clear(); updateState(); }); setEditing(false); updateState();
}
void SingleFileWindow::setEditing(bool editing) {
    if (!m_document || (editing && m_document->readOnly())) { updateState(); return; }
    m_editing = editing; m_body->setCurrentWidget(editing ? m_editorPage : static_cast<QWidget *>(m_preview));
    if (editing) m_editor->setFocus(Qt::OtherFocusReason); updateState();
}
void SingleFileWindow::setContentZoom(int percent) {
    m_zoom = qBound(50, percent, 200); m_zoomButton->setText(QString::number(m_zoom) + '%');
    m_zoomButton->setAccessibleName(QStringLiteral("正文缩放，%1%").arg(m_zoom));
    if (m_preview) m_preview->view()->setZoomFactor(m_zoom / 100.0); if (m_editor) m_editor->setTextZoom(m_zoom);
    for (auto *action : m_zoomButton->menu()->actions()) action->setChecked(action->text() == m_zoomButton->text());
}
void SingleFileWindow::updateCaret() {
    m_cursor->setVisible(m_editing && m_editor);
    if (!m_editor || !m_editing) return;
    const auto position = m_editor->send(SCI_GETCURRENTPOS);
    m_cursor->setText(QStringLiteral("行 %1，列 %2").arg(m_editor->send(SCI_LINEFROMPOSITION, uptr_t(position)) + 1).arg(m_editor->send(SCI_GETCOLUMN, uptr_t(position)) + 1));
}
void SingleFileWindow::updateState() {
    const bool ready = m_document && !m_document->closed(); const bool busy = ready && (m_document->saving() || m_document->loading());
    m_read->setEnabled(ready); m_edit->setEnabled(ready && !m_document->readOnly()); m_read->setChecked(!m_editing); m_edit->setChecked(m_editing);
    m_pathButton->setEnabled(ready); m_zoomButton->setEnabled(ready); m_save->setEnabled(ready && !busy && (!m_document->readOnly() || m_document->conflict()));
    m_saveAction->setEnabled(m_save->isEnabled()); m_saveAsAction->setEnabled(ready && !busy); m_reloadAction->setEnabled(ready && !busy);
    m_undoAction->setEnabled(ready && m_editing && !m_document->readOnly() && m_editor && m_editor->send(SCI_CANUNDO)); m_redoAction->setEnabled(ready && m_editing && !m_document->readOnly() && m_editor && m_editor->send(SCI_CANREDO));
    for (auto *button : m_formats) button->setEnabled(ready && !m_document->readOnly()); updateCaret();
    if (!ready) {
        const bool loading = !m_intent.isNull(), error = !m_openError.isEmpty();
        m_header->setTitle(m_openPath.isEmpty() ? QStringLiteral("打开 Markdown 文档") : QFileInfo(m_openPath).fileName(), false); m_path->setFullText(m_openPath.isEmpty() ? QString() : QFileInfo(m_openPath).path());
        m_empty->setText(loading ? QStringLiteral("正在打开…\n") + QFileInfo(m_openPath).fileName() : error ? QStringLiteral("无法打开文件\n\n") + m_openError + "\n\n" + m_openPath : QStringLiteral("打开一个 Markdown 文件\n从阅读开始，随时切换编辑。"));
        m_status->setFullText(loading ? QStringLiteral("正在打开…") : error ? QStringLiteral("打开失败") : QStringLiteral("请选择文档")); m_statusIcon->clear(); m_encoding->clear();
        const auto notice = m_appearanceError.isEmpty() ? m_geometryError : m_appearanceError;
        m_notice->setText(notice); m_banner->setVisible(!notice.isEmpty()); m_resolve->hide(); m_retry->hide(); m_later->hide(); return;
    }
    m_header->setTitle(QFileInfo(m_document->path()).fileName(), m_document->dirty()); setWindowTitle(QFileInfo(m_document->path()).fileName() + QStringLiteral("[*] — xdnote")); setWindowModified(m_document->dirty());
    m_path->setFullText(QFileInfo(m_document->path()).path()); m_status->setFullText(m_document->statusText()); m_encoding->setText(m_document->bom() ? "UTF-8 BOM" : "UTF-8");
    const bool warning = m_document->dirty() || m_document->conflict(); const bool error = m_document->hasError();
    const auto statusColor = busy ? m_appearance->color("textSecondary") : m_appearance->semantic(error ? "error" : warning ? "warning" : "success");
    m_statusIcon->setPixmap(themedIcon(busy ? "clock" : error ? "warning" : warning ? "status-dirty" : m_document->readOnly() ? "lock" : "check-circle", statusColor).pixmap(QSize(16, 16), devicePixelRatioF()));
    auto message = m_document->message(); if (message.isEmpty()) message = m_document->session()->rejection();
    if (message.isEmpty()) message = m_previewError;
    if (message.isEmpty()) message = m_appearanceError;
    if (message.isEmpty()) message = m_geometryError;
    if (message.isEmpty() && m_document->readOnly()) message = QStringLiteral("此文件为只读，阅读不受影响。");
    m_notice->setText(message); const bool previewOnly = !m_previewError.isEmpty() && message == m_previewError;
    const bool appearanceOnly = !message.isEmpty() && (message == m_appearanceError || message == m_geometryError);
    m_banner->setVisible(!message.isEmpty() && (m_document->dirty() || message != m_dismissedNotice));
    m_resolve->setText(m_document->conflict() ? (m_document->dirty() ? QStringLiteral("处理冲突") : QStringLiteral("重新载入")) : previewOnly ? QStringLiteral("重试预览") : QStringLiteral("另存为…"));
    m_resolve->setVisible(!appearanceOnly && (previewOnly || error || m_document->conflict() || m_document->readOnly())); m_resolve->setEnabled(!busy);
    m_retry->setVisible(error && !m_document->conflict() && !m_document->readOnly() && m_document->lastSave().problem.code != FileError::HardLinks); m_retry->setEnabled(!busy);
    m_later->setVisible(m_document->conflict() && !m_document->dirty());
    static_cast<SharedButton *>(m_edit)->setSymbol(m_document->readOnly() ? "lock" : QString()); m_edit->setToolTip(m_document->readOnly() ? QStringLiteral("文件只读，可另存为副本后编辑") : QStringLiteral("编辑 Markdown 源文"));
}
void SingleFileWindow::chooseOpen() {
    const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("打开 Markdown 文件"), {}, QStringLiteral("Markdown 文档 (*.md *.markdown);;所有文件 (*)"));
    InputModality::forApplication()->clear(); if (!path.isEmpty()) emit openRequested(path);
}
void SingleFileWindow::chooseSaveAs() {
    if (!m_document || m_document->saving() || m_document->loading()) return;
    const auto path = QFileDialog::getSaveFileName(this, QStringLiteral("另存为"), m_document->path(), QStringLiteral("Markdown 文档 (*.md *.markdown);;所有文件 (*)"));
    InputModality::forApplication()->clear(); if (!path.isEmpty()) m_document->saveAs(path);
}
void SingleFileWindow::reloadDisk() {
    if (!m_document || m_document->saving() || m_document->loading()) return;
    if (m_document->dirty() && ask(this, "reloadConfirmation", QStringLiteral("使用磁盘上的版本？"), QStringLiteral("当前未保存的修改将被放弃。"), {{QStringLiteral("重新载入"), 2}, {QStringLiteral("取消"), 0}}, "externalConflict") != 2) return;
    m_document->reload();
}
void SingleFileWindow::resolveConflict() {
    if (!m_document || m_document->saving() || m_document->loading()) return;
    const int choice = ask(this, "conflictDialog", QStringLiteral("文件已被其他程序修改"), m_document->message(), {{QStringLiteral("保留当前编辑"), 0}, {QStringLiteral("重新载入"), 2}, {QStringLiteral("另存为…"), 1}}, "externalConflict");
    if (choice == 2) reloadDisk(); else if (choice == 1) chooseSaveAs();
}
void SingleFileWindow::wrapSelection(const QString &before, const QString &after) {
    if (!m_document || !m_editing || !m_editor) return;
    const auto snapshot = m_document->session()->snapshot(); if (snapshot.result != EditResult::Applied) return;
    const auto start = qsizetype(m_editor->send(SCI_GETSELECTIONSTART)), end = qsizetype(m_editor->send(SCI_GETSELECTIONEND));
    const auto first = decodeUtf8Literal(snapshot.utf8.left(start)), selected = decodeUtf8Literal(snapshot.utf8.mid(start, end - start));
    if (m_document->session()->replace(int(first.size()), int(first.size() + selected.size()), before + selected + after, snapshot.revision) == EditResult::Applied) {
        m_editor->send(SCI_SETSEL, uptr_t(start + before.toUtf8().size()), sptr_t(end + before.toUtf8().size())); m_editor->setFocus(Qt::OtherFocusReason);
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
        const auto revision = m_document->session()->revision(); m_prompting = true;
        const int choice = ask(this, "unsavedConfirmation", QStringLiteral("保存对文档的修改？"), QStringLiteral("未保存的修改会在关闭后丢失。"), {{QStringLiteral("保存"), 1}, {QStringLiteral("不保存"), 2}, {QStringLiteral("取消"), 0}});
        m_prompting = false;
        if (choice == 0 || !m_document || (choice == 2 && m_document->session()->revision() != revision)) { emit closeCancelled(); event->ignore(); return; }
        if (choice == 1) {
            m_closeAfterOperation = true; if (!m_document->save()) { m_closeAfterOperation = false; emit closeCancelled(); if (m_document->conflict()) resolveConflict(); }
            event->ignore(); return;
        }
    }
    if (m_manager->closeDocument(m_document)) event->accept(); else { event->ignore(); emit closeCancelled(); }
}
bool SingleFileWindow::eventFilter(QObject *target, QEvent *event) {
    auto *widget = qobject_cast<QWidget *>(target); if (!widget || (widget != this && !isAncestorOf(widget))) return QWidget::eventFilter(target, event);
    if (event->type() == QEvent::DragEnter) { auto *drag = static_cast<QDragEnterEvent *>(event); if (drag->mimeData()->hasUrls()) { drag->acceptProposedAction(); return true; } }
    if (event->type() == QEvent::Drop) {
        auto *drop = static_cast<QDropEvent *>(event); const auto urls = drop->mimeData()->urls();
        if (!urls.isEmpty() && urls.size() <= 16) { for (const auto &url : urls) if (url.isLocalFile()) emit openRequested(url.toLocalFile()); drop->acceptProposedAction(); return true; }
    }
    if (widget == this && event->type() == QEvent::WindowActivate && m_document) { m_dismissedNotice.clear(); m_document->checkExternal(); }
    if (widget == this && (event->type() == QEvent::Resize || event->type() == QEvent::ScreenChangeInternal)) QTimer::singleShot(0, this, &SingleFileWindow::updateState);
    return QWidget::eventFilter(target, event);
}
}
