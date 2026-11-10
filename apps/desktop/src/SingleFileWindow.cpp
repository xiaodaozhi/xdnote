#include "SingleFileWindow.h"
#include "Appearance.h"
#include "SharedWidgets.h"
#include "SourceEditor.h"
#include "MarkdownPreview.h"
#include "WorkspaceResourceReader.h"
#include "WorkspacePath.h"
#include "ImageImport.h"
#include "FileService.h"
#include "VisualTokens.h"
#include "InputModality.h"
#include "Utf8Text.h"
#include "WindowGeometry.h"
#include "UiFonts.h"
#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QDialog>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFutureWatcher>
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
#include <QShowEvent>
#include <QProgressBar>
#include <QSplitter>
#include <QTextLayout>
#include <QGlyphRun>
#include <QRawFont>
#include <QtConcurrentRun>
#include <cmath>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#endif

namespace xdnote {
namespace {
int token(const char *section, const char *key) { return visualTokens().value(section).toObject().value(key).toInt(); }
QJsonObject geometryTokens() { return visualTokens().value("windows").toObject().value("standalone").toObject(); }
struct LinkTargetResult { bool success = false; QString url; QString message; };
LinkTargetResult prepareWorkspaceLink(const std::shared_ptr<WorkspaceResourceReader> &root,
    const QString &documentPath, const QString &documentIdentity, const QString &targetPath) {
    if (!root || !root->active()) return {false, {}, QStringLiteral("工作区授权已失效。")};
    const auto url = relativeWorkspaceUrl(documentPath, targetPath);
    if (!url.success) return {false, {}, QStringLiteral("所选文档不在当前工作区内。")};
    const auto source = root->read(documentPath, FileService::DiskByteLimit);
    if (!source.ok() || source.identity != documentIdentity) return {false, {}, QStringLiteral("无法核验当前文档的位置或身份。")};
    const auto target = root->read(targetPath, FileService::DiskByteLimit);
    if (!target.ok()) return {false, {}, QStringLiteral("无法读取或核验所选文档，请检查位置、权限和大小。")};
    const bool bom = target.bytes.startsWith(QByteArray::fromHex("efbbbf"));
    const auto utf8 = bom ? target.bytes.mid(3) : target.bytes;
    bool valid = false; decodeUtf8Literal(utf8, &valid);
    if (!valid || utf8.contains('\0') || utf8.size() > FileService::SourceLimit)
        return {false, {}, QStringLiteral("所选文档不是受支持的 UTF-8 Markdown 文件。")};
    const auto sourceAgain = root->read(documentPath, FileService::DiskByteLimit);
    const auto targetAgain = root->read(targetPath, FileService::DiskByteLimit);
    if (!root->active() || !sourceAgain.ok() || sourceAgain.identity != documentIdentity
        || !targetAgain.ok() || targetAgain.identity != target.identity || targetAgain.bytes != target.bytes)
        return {false, {}, QStringLiteral("工作区或所选文档已变化，请重新选择。")};
    return {true, url.path, {}};
}
void separator(QBoxLayout *layout, Appearance *appearance, QWidget *owner) {
    auto *line = new QWidget(owner); line->setFixedWidth(1); line->setFixedHeight(20);
    const auto refresh = [line, appearance] { line->setStyleSheet("background:" + appearance->color("border").name()); };
    QObject::connect(appearance, &Appearance::changed, line, refresh); refresh(); layout->addWidget(line);
}
int ask(SingleFileWindow *owner, const QString &name, const QString &title, const QString &text,
        const QList<QPair<QString, int>> &actions, const QString &widthToken = "unsavedConfirmation") {
    auto *appearance = owner->appearance();
    const QPointer<QWidget> caller(owner->focusWidget());
    const auto size = visualTokens().value("windows").toObject().value(widthToken).toObject();
    const int width = qMin(size.value("preferredWidth").toInt(), qMax(200, owner->screen()->availableGeometry().width() - 32));
    SharedDialog dialog(appearance, owner->window(), width); dialog.setObjectName(name); dialog.setWindowTitle(title);
    auto *layout = new QVBoxLayout(dialog.surface()); layout->setContentsMargins(24, 20, 24, 24); layout->setSpacing(16);
    auto *top = new QHBoxLayout;
    if (name == "operationCloseConfirmation") { auto *spinner = new SharedSpinner(appearance, &dialog); spinner->setRunning(true); top->addWidget(spinner); }
    else { auto *symbol = new QLabel(&dialog); symbol->setPixmap(themedIcon(name == "conflictDialog" || name == "reloadConfirmation" ? "warning" : "document", appearance->semantic("warning")).pixmap(QSize(24, 24), dialog.devicePixelRatioF())); top->addWidget(symbol); }
    top->addStretch(); auto *close = new SharedButton({}, "dialogClose", appearance, &dialog); close->setFixedWidth(32); close->setSymbol("close"); close->setAccessibleName(QStringLiteral("关闭对话框")); top->addWidget(close); QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::reject); layout->addLayout(top);
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
        row->addLayout(details, 1);
        if (owner->document()->dirty()) { auto *dirty = new QLabel(card); dirty->setObjectName("dialogDirtyIndicator"); dirty->setAccessibleName(QStringLiteral("有未保存的修改")); dirty->setPixmap(themedIcon("status-dirty", appearance->semantic("warning")).pixmap(QSize(12, 12), dialog.devicePixelRatioF())); row->addWidget(dirty); }
        layout->addWidget(card);
    }
    auto *buttons = new QHBoxLayout; buttons->setSpacing(8);
    auto add = [&](const QPair<QString, int> &entry) {
        auto *button = new SharedButton(entry.first, QStringLiteral("choice%1").arg(entry.second), appearance, &dialog);
        const bool destructive = entry.second == 2 && name == "reloadConfirmation";
        const bool primary = name == "conflictDialog" ? entry.second == 0 : entry.second == 1 || destructive;
        button->setProperty("primary", primary); button->setProperty("danger", destructive);
        button->setOutlined(!primary); buttons->addWidget(button);
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

SingleFileWindow::SingleFileWindow(FileManager *manager, Appearance *appearance, WindowGeometryStore *geometryStore)
    : ApplicationWindow(appearance), m_manager(manager), m_appearance(windowAppearance()) {
    setObjectName("standaloneWindow"); setAttribute(Qt::WA_DeleteOnClose); setAcceptDrops(true);
#ifdef Q_OS_WIN
    // Qt must also know this window has no system caption. Otherwise QWidget's
    // client-size constraints can add the removed caption height back at minimum.
    setWindowFlag(Qt::FramelessWindowHint);
#endif
    const auto size = geometryTokens(); resize(size.value("defaultWidth").toInt(), size.value("defaultHeight").toInt());
    setMinimumSize(size.value("minWidth").toInt(), size.value("minHeight").toInt());
    QFont ui = uiFont();
    ui.setPixelSize(visualTokens().value("typography").toObject().value("ui").toObject().value("size").toInt()); setFont(ui);
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
    m_header = new WindowTitleBar(m_appearance, this); setTitleBar(m_header); layout->addWidget(m_header);
    auto *toolbar = new QWidget(this); toolbar->setObjectName("documentToolbar"); toolbar->setFixedHeight(size.value("toolbarHeight").toInt());
    auto *bar = new QGridLayout(toolbar); bar->setContentsMargins(16, 0, 16, 0); bar->setHorizontalSpacing(16); bar->setColumnStretch(0, 1); bar->setColumnStretch(2, 1);
    auto *location = new QWidget(toolbar); auto *pathRow = new QHBoxLayout(location); pathRow->setContentsMargins(0, 0, 0, 0); pathRow->setSpacing(8);
    m_pathButton = new SharedButton({}, "fileLocation", m_appearance, location); m_pathButton->setSymbol("folder-open"); m_pathButton->setOutlined(true);
    m_pathButton->setAccessibleName(QStringLiteral("在工作区打开")); m_pathButton->setToolTip(QStringLiteral("在工作区继续处理此文档")); m_pathButton->setSymbol("workspace"); connect(m_pathButton, &QPushButton::clicked, this, [this] { if (m_embedded) emit sidebarRequested(); else emit workspaceRequested(); }); m_pathButton->setFixedWidth(token("controls", "iconButtonHit")); pathRow->addWidget(m_pathButton);
    auto *pathMenu = new SharedMenu(m_appearance, m_pathButton); pathMenu->setObjectName("pathMenu"); pathMenu->setTitle(QStringLiteral("文件位置"));
    auto *copyPath = pathMenu->addAction(QStringLiteral("复制完整路径")); copyPath->setObjectName("copyPath");
    auto *reveal = pathMenu->addAction(QStringLiteral("打开所在文件夹")); reveal->setObjectName("revealFolder");
    connect(copyPath, &QAction::triggered, this, [this] { if (m_document) emit copyPathRequested(m_document->path()); });
    connect(reveal, &QAction::triggered, this, [this] { if (m_document) emit revealRequested(QFileInfo(m_document->path()).path()); });
    m_path = new ElidedLabel(location); auto pathFont = ui; pathFont.setPixelSize(visualTokens().value("typography").toObject().value("secondary").toObject().value("size").toInt()); m_path->setFont(pathFont); m_path->setObjectName("documentPath"); m_path->setProperty("secondary", true); pathRow->addWidget(m_path, 1); bar->addWidget(location, 0, 0);
    auto *modeBox = new QWidget(toolbar); modeBox->setObjectName("modeSwitch"); modeBox->setFixedHeight(token("controls", "height")); auto *modeLayout = m_modeLayout = new QHBoxLayout(modeBox); modeLayout->setContentsMargins(0, 0, 0, 0); modeLayout->setSpacing(0);
    m_read = new SharedButton(QStringLiteral("阅读"), "readMode", m_appearance, modeBox); m_edit = new SharedButton(QStringLiteral("编辑"), "editMode", m_appearance, modeBox);
    m_split = new SharedButton(QStringLiteral("双栏"), "splitMode", m_appearance, modeBox); m_split->setCheckable(true); m_split->setFixedWidth(token("controls", "modeSegmentWidth")); m_split->hide();
    m_read->setCheckable(true); m_edit->setCheckable(true); auto *modes = new QButtonGroup(this); modes->setExclusive(true); modes->addButton(m_read, 0); modes->addButton(m_edit, 1); modes->addButton(m_split, 2);
    m_read->setFixedWidth(token("controls", "modeSegmentWidth")); m_edit->setFixedWidth(token("controls", "modeSegmentWidth"));
    modeLayout->addWidget(m_read); modeLayout->addWidget(m_edit); bar->addWidget(modeBox, 0, 1);
    auto *actions = new QWidget(toolbar); auto *right = new QHBoxLayout(actions); right->setContentsMargins(0, 0, 0, 0); right->setSpacing(8); right->addStretch();
    m_save = new SharedButton(QStringLiteral("保存"), "saveButton", m_appearance, actions); m_save->setProperty("primary", true); right->addWidget(m_save);
    auto *more = new SharedButton({}, "moreActions", m_appearance, actions); more->setSymbol("more"); more->setAccessibleName(QStringLiteral("文档操作")); more->setToolTip(QStringLiteral("文档操作")); more->setFixedWidth(token("controls", "iconButtonHit")); right->addWidget(more); bar->addWidget(actions, 0, 2);
    m_menu = new SharedMenu(m_appearance, more); m_menu->setObjectName("documentMenu"); m_menu->setTitle(QStringLiteral("文档操作"));
    auto action = [this](const QString &text, const QString &name, QKeySequence shortcut, auto callback) {
        auto *item = m_menu->addAction(text); item->setObjectName(name); if (!shortcut.isEmpty()) item->setShortcut(shortcut);
        item->setShortcutContext(Qt::WindowShortcut); addAction(item); connect(item, &QAction::triggered, InputModality::forApplication(), &InputModality::clear);
        connect(item, &QAction::triggered, this, callback); return item;
    };
    pathMenu->setParent(m_menu, pathMenu->windowFlags()); m_menu->addMenu(pathMenu);
    action(QStringLiteral("打开工作区…"), "openWorkspaceAction", {}, [this] { emit openDirectoryRequested(); });
    action(QStringLiteral("在工作区打开"), "transferWorkspaceAction", {}, [this] { emit workspaceRequested(); });
    action(QStringLiteral("打开…"), "openAction", QKeySequence::Open, [this] { chooseOpen(); });
    m_saveAction = action(QStringLiteral("保存"), "saveAction", QKeySequence::Save, [this] { if (m_document && !m_document->save() && m_document->conflict()) resolveConflict(); });
    m_saveAsAction = action(QStringLiteral("另存为…"), "saveAsAction", QKeySequence::SaveAs, [this] { chooseSaveAs(); });
    m_reloadAction = action(QStringLiteral("重新载入磁盘版本…"), "reloadAction", {}, [this] { reloadDisk(); });
    m_menu->addSeparator();
    m_undoAction = action(QStringLiteral("撤销"), "undoAction", QKeySequence::Undo, [this] { if (m_document) m_document->session()->undo(); });
    m_redoAction = action(QStringLiteral("重做"), "redoAction", QKeySequence::Redo, [this] { if (m_document) m_document->session()->redo(); });
    m_imageImportAction = action(QStringLiteral("插入图片…"), "insertImageAction", {}, [this] { chooseImage(); });
    m_linkInsertAction = action(QStringLiteral("插入工作区文档链接…"), "insertWorkspaceLinkAction", {}, [this] { chooseWorkspaceDocumentLink(); });
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
        item->setIcon(colorSwatch(QColor(entry.value("base").toString()))); accentGroup->addAction(item); m_accentActions.append(item);
        connect(item, &QAction::triggered, this, [this, accent = QString::fromLatin1(name)] { m_appearance->setPreferences(m_appearance->theme(), accent); applyAppearance(); });
    }
    m_menu->addSeparator(); action(QStringLiteral("关闭文档"), "closeAction", QKeySequence::Close, [this] { close(); });
    action(QStringLiteral("退出 xdnote"), "quitAction", QKeySequence::Quit, [this] { emit quitRequested(); }); more->setMenu(m_menu);
    connect(modes, &QButtonGroup::idClicked, this, [this](int mode) { setViewMode(mode == 2 ? ViewMode::Split : mode == 1 ? ViewMode::Source : ViewMode::Reading); }); connect(m_save, &QPushButton::clicked, m_saveAction, &QAction::trigger); layout->addWidget(toolbar);
    m_banner = new QWidget(this); m_banner->setObjectName("documentBanner"); auto *banner = new QHBoxLayout(m_banner); banner->setContentsMargins(16, 8, 16, 8); banner->setSpacing(8);
    m_noticeSymbol = new QLabel(m_banner); m_noticeSymbol->setFixedSize(20, 20); banner->addWidget(m_noticeSymbol);
    m_notice = new QLabel(m_banner); m_notice->setObjectName("documentNotice"); m_notice->setTextFormat(Qt::PlainText); m_notice->setWordWrap(true); m_notice->setMinimumWidth(0); m_notice->setTextInteractionFlags(Qt::TextSelectableByMouse); banner->addWidget(m_notice, 1);
    m_resolve = new SharedButton(QStringLiteral("处理"), "resolveConflict", m_appearance, m_banner); m_retry = new SharedButton(QStringLiteral("重试"), "retrySave", m_appearance, m_banner); m_later = new SharedButton(QStringLiteral("稍后"), "dismissNotice", m_appearance, m_banner);
    banner->addWidget(m_retry); banner->addWidget(m_resolve); banner->addWidget(m_later);
    m_noticeClose = new SharedButton({}, "closeNotice", m_appearance, m_banner); m_noticeClose->setSymbol("close"); m_noticeClose->setFixedWidth(32); m_noticeClose->setAccessibleName(QStringLiteral("关闭提示")); banner->addWidget(m_noticeClose);
    connect(m_noticeClose, &QPushButton::clicked, this, [this] {
        if (!m_navigationMessage.isEmpty() && m_notice->text() == m_navigationMessage) {
            m_navigationMessage.clear(); m_dismissedNotice.clear(); updateState(); return;
        }
        m_dismissedNotice = m_notice->text(); m_banner->hide();
    });
    connect(m_resolve, &QPushButton::clicked, this, [this] { if (m_document && m_document->conflict()) { if (m_document->dirty()) resolveConflict(); else reloadDisk(); } else if (!m_previewError.isEmpty() && m_document && !m_document->hasError()) m_preview->refresh(); else chooseSaveAs(); });
    connect(m_retry, &QPushButton::clicked, m_saveAction, &QAction::trigger);
    connect(m_later, &QPushButton::clicked, this, [this] { m_dismissedNotice = m_notice->text(); m_banner->hide(); }); layout->addWidget(m_banner);
    m_body = new QStackedWidget(this);
#ifdef Q_OS_WIN
    // Keep the vertical scrollbar clear of the native right-edge resize hit.
    m_body->setContentsMargins(0, 0, 8, 0);
#endif
    auto *welcome = new QWidget(m_body); auto *welcomeLayout = new QVBoxLayout(welcome); welcomeLayout->setContentsMargins(40, 40, 40, 40);
    welcomeLayout->setSpacing(16); welcomeLayout->addStretch();
    m_openSpinner = new SharedSpinner(m_appearance, welcome); welcomeLayout->addWidget(m_openSpinner, 0, Qt::AlignHCenter);
    m_emptySymbol = new QLabel(welcome); welcomeLayout->addWidget(m_emptySymbol, 0, Qt::AlignHCenter);
    m_emptyTitle = new QLabel(welcome); m_emptyTitle->setObjectName("openStateTitle"); m_emptyTitle->setFont(uiFont(24)); m_emptyTitle->setAlignment(Qt::AlignCenter); welcomeLayout->addWidget(m_emptyTitle);
    m_empty = new QLabel(welcome); m_empty->setObjectName("openStateDetails"); m_empty->setAlignment(Qt::AlignCenter); m_empty->setWordWrap(true); m_empty->setTextFormat(Qt::PlainText); welcomeLayout->addWidget(m_empty);
    m_welcomeOpen = new SharedButton(QStringLiteral("选择文件…"), "welcomeOpen", m_appearance, welcome); m_welcomeOpen->setSymbol("folder-open"); m_welcomeOpen->setProperty("primary", true); connect(m_welcomeOpen, &QPushButton::clicked, this, &SingleFileWindow::chooseOpen);
    m_cancelOpen = new SharedButton(QStringLiteral("取消打开"), "cancelOpen", m_appearance, welcome); m_cancelOpen->setOutlined(true); connect(m_cancelOpen, &QPushButton::clicked, this, [this] { if (!m_intent.isNull()) emit cancelOpenRequested(m_intent); m_intent = {}; m_openPath.clear(); updateState(); });
    auto *workspaceOpen = new SharedButton(QStringLiteral("打开工作区…"), "welcomeWorkspace", m_appearance, welcome); workspaceOpen->setSymbol("workspace"); connect(workspaceOpen, &QPushButton::clicked, this, &SingleFileWindow::openDirectoryRequested); welcomeLayout->addWidget(workspaceOpen, 0, Qt::AlignHCenter);
    welcomeLayout->addWidget(m_welcomeOpen, 0, Qt::AlignHCenter); welcomeLayout->addWidget(m_cancelOpen, 0, Qt::AlignHCenter); welcomeLayout->addStretch(); m_body->addWidget(welcome); layout->addWidget(m_body, 1);
    auto *footer = m_footer = new QWidget(this); footer->setObjectName("documentFooter"); footer->setFixedHeight(size.value("statusbarHeight").toInt());
    QFont secondary = ui; secondary.setPixelSize(visualTokens().value("typography").toObject().value("secondary").toObject().value("size").toInt()); footer->setFont(secondary);
    auto *foot = new QHBoxLayout(footer); foot->setContentsMargins(16, 0, 16, 0); foot->setSpacing(8);
    m_statusIcon = new QLabel(footer); m_statusIcon->setFixedSize(16, 16); foot->addWidget(m_statusIcon);
    m_statusIcon->setObjectName("documentStatusIcon");
    m_saveSpinner = new SharedSpinner(m_appearance, footer); m_saveSpinner->setFixedSize(16, 16); foot->addWidget(m_saveSpinner);
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
    const QHash<QString, QString> symbols{{"copyPath","copy"},{"revealFolder","folder-open"},{"openAction","folder-open"},{"saveAction","save"},{"saveAsAction","save-as"},{"reloadAction","refresh"},{"undoAction","undo"},{"redoAction","redo"},{"closeAction","close"},{"quitAction","focus-exit"},{"theme-system","monitor"},{"theme-light","sun"},{"theme-dark","moon"}};
    for (auto *item : findChildren<QAction *>()) if (symbols.contains(item->objectName())) item->setIcon(themedIcon(symbols.value(item->objectName()), m_appearance->color("text")));
    for (auto *menu : findChildren<QMenu *>()) if (menu->objectName() == "appearanceMenu" || menu->objectName() == "accentMenu") menu->menuAction()->setIcon(themedIcon("palette", m_appearance->color("text")));
#ifdef Q_OS_WIN
    applyNativeAppearance();
#endif
    if (m_footer && m_footer->parentWidget() != this) m_footer->setStyleSheet(styleSheet());
    update();
}
void SingleFileWindow::setLoading(const QString &path, const QUuid &intent) { m_intent = intent; m_openPath = path; m_openError.clear(); updateState(); }
void SingleFileWindow::showOpenError(const QString &path, const QString &message) { m_intent = {}; m_openPath = path; m_openError = message; updateState(); }
void SingleFileWindow::setDocument(FileDocument *document) {
    if (m_document) return;
    m_intent = {}; m_document = document;
    m_splitter = new QSplitter(Qt::Horizontal, m_body); m_splitter->setObjectName("documentSplit"); m_splitter->setChildrenCollapsible(false); m_splitter->setHandleWidth(1); m_body->addWidget(m_splitter);
    m_previewPane = new QWidget(m_splitter); auto *previewLayout = new QVBoxLayout(m_previewPane); previewLayout->setContentsMargins(0, 0, 0, 0); previewLayout->setSpacing(0);
    m_previewHeading = new QWidget(m_previewPane); m_previewHeading->setObjectName("formatToolbar"); m_previewHeading->setFixedHeight(visualTokens().value("windows").toObject().value("main").toObject().value("sourceToolbarHeight").toInt()); auto *headingLayout = new QHBoxLayout(m_previewHeading); headingLayout->setContentsMargins(16, 0, 16, 0); headingLayout->addWidget(new QLabel(QStringLiteral("预览"), m_previewHeading)); previewLayout->addWidget(m_previewHeading);
    m_previewStack = new QStackedWidget(m_previewPane); previewLayout->addWidget(m_previewStack, 1);
    m_preview = new MarkdownPreview(document->session(), m_previewStack, geometryTokens().value("readerPadding").toInt(), m_appearance); m_previewStack->addWidget(m_preview);
    connect(m_preview, &MarkdownPreview::copyTextRequested, this, &SingleFileWindow::copyTextRequested);
    m_previewFailure = new QWidget(m_previewStack); m_previewFailure->setObjectName("previewFailure"); auto *failure = new QVBoxLayout(m_previewFailure); failure->setContentsMargins(40, 40, 40, 40); failure->setSpacing(16); failure->addStretch();
    auto *failureIcon = new QLabel(m_previewFailure); failureIcon->setPixmap(themedIcon("error", m_appearance->semantic("error")).pixmap(40, 40)); failure->addWidget(failureIcon, 0, Qt::AlignHCenter);
    auto *failureTitle = new QLabel(QStringLiteral("无法显示预览"), m_previewFailure); failureTitle->setFont(uiFont(24)); failure->addWidget(failureTitle, 0, Qt::AlignHCenter);
    m_previewFailureText = new QLabel(m_previewFailure); m_previewFailureText->setTextFormat(Qt::PlainText); m_previewFailureText->setWordWrap(true); m_previewFailureText->setAlignment(Qt::AlignCenter); failure->addWidget(m_previewFailureText);
    auto *retryPreview = new SharedButton(QStringLiteral("重新生成预览"), "retryPreview", m_appearance, m_previewFailure); retryPreview->setSymbol("refresh"); retryPreview->setProperty("primary", true); failure->addWidget(retryPreview, 0, Qt::AlignHCenter); connect(retryPreview, &QPushButton::clicked, m_preview, &MarkdownPreview::refresh);
    auto *viewSource = new SharedButton(QStringLiteral("查看源文"), "viewSource", m_appearance, m_previewFailure); viewSource->setSymbol("code"); viewSource->setOutlined(true); failure->addWidget(viewSource, 0, Qt::AlignHCenter); connect(viewSource, &QPushButton::clicked, this, [this] { setViewMode(ViewMode::Source); }); failure->addStretch(); m_previewStack->addWidget(m_previewFailure);
    m_editorPage = new QWidget(m_splitter); auto *layout = new QVBoxLayout(m_editorPage); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
    auto *format = new QWidget(m_editorPage); format->setObjectName("formatToolbar"); format->setFixedHeight(visualTokens().value("windows").toObject().value("main").toObject().value("sourceToolbarHeight").toInt());
    auto *tools = new QHBoxLayout(format); tools->setContentsMargins(16, 0, 16, 0); tools->setSpacing(4); tools->addWidget(new QLabel("Markdown", format)); separator(tools, m_appearance, format);
    const QList<QPair<QString, QPair<QString, QString>>> formats{{"bold", {"**", "**"}}, {"italic", {"*", "*"}}, {"heading", {"## ", ""}}, {"link", {"[", "](url)"}}, {"checklist", {"- [ ] ", ""}}, {"code", {"`", "`"}}};
    const QStringList names{QStringLiteral("加粗"), QStringLiteral("斜体"), QStringLiteral("二级标题"), QStringLiteral("插入链接"), QStringLiteral("任务列表"), QStringLiteral("行内代码")};
    int index = 0;
    for (const auto &entry : formats) {
        auto *button = new SharedButton({}, QStringLiteral("format%1").arg(index), m_appearance, format); button->setSymbol(entry.first); button->setFixedWidth(token("controls", "iconButtonHit"));
        button->setAccessibleName(names[index]); button->setToolTip(names[index++]); m_formats.append(button);
        if (entry.first == "heading") {
            button->setAccessibleName(QStringLiteral("标题级别")); button->setToolTip(QStringLiteral("选择一级至六级标题"));
            auto *headings = new SharedMenu(m_appearance, button); headings->setObjectName("headingMenu");
            for (int level = 1; level <= 6; ++level) { auto *item = headings->addAction(QStringLiteral("%1 级标题").arg(level)); item->setData(level); item->setIcon(themedIcon("heading", m_appearance->color("text"))); connect(item, &QAction::triggered, this, [this, level] {
                if (!m_document || !m_editing || !m_editor) return;
                const auto snapshot = m_document->session()->snapshot(); if (snapshot.result != EditResult::Applied) return;
                const auto position = qsizetype(m_editor->send(SCI_GETCURRENTPOS)); const auto start = position > 0 ? snapshot.utf8.lastIndexOf('\n', position - 1) + 1 : 0;
                auto line = snapshot.utf8.mid(start); line = line.left(line.indexOf('\n') < 0 ? line.size() : line.indexOf('\n')); int prefix = 0; while (prefix < qMin(6, int(line.size())) && line[prefix] == '#') ++prefix;
                if (!prefix || prefix >= line.size() || line[prefix] != ' ') prefix = 0; else ++prefix;
                const auto first = decodeUtf8Literal(snapshot.utf8.left(start)).size();
                m_document->session()->replace(int(first), int(first + prefix), QString(level, '#') + ' ', snapshot.revision); m_editor->setFocus(Qt::OtherFocusReason);
            }); } button->setMenu(headings);
        } else connect(button, &QPushButton::clicked, this, [this, wrap = entry.second] { wrapSelection(wrap.first, wrap.second); }); tools->addWidget(button);
    }
    tools->addStretch(); layout->addWidget(format);
    m_editor = new SourceEditor(document->session(), m_editorPage, m_appearance); m_editor->setObjectName("sourceEditor");
    connect(m_editor, &SourceEditor::imagePasteRequested, this, &SingleFileWindow::importClipboardImage);
    m_editor->send(SCI_SETMARGINWIDTHN, 0, token("controls", "lineNumberWidth")); m_editor->send(SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
    const auto bytes = document->session()->snapshot().utf8; const int lf = int(bytes.indexOf('\n')), cr = int(bytes.indexOf('\r'));
    m_editor->send(SCI_SETEOLMODE, cr >= 0 && (lf < 0 || cr < lf) ? (lf == cr + 1 ? SC_EOL_CRLF : SC_EOL_CR) : SC_EOL_LF);
    connect(m_editor, &ScintillaEditBase::updateUi, this, [this] { updateCaret(); });
    layout->addWidget(m_editor, 1); m_splitter->insertWidget(0, m_editorPage); m_splitter->addWidget(m_previewPane); m_splitter->setSizes({500, 500});
    connect(document, &FileDocument::stateChanged, this, &SingleFileWindow::updateState); connect(document, &QObject::destroyed, this, &SingleFileWindow::updateState);
    connect(document, &FileDocument::operationFinished, this, [this](bool success) {
        const bool saving = m_observedSave; m_observedSave = false;
        if (saving && !success && m_document && m_document->hasError()) QTimer::singleShot(0, this, &SingleFileWindow::showSaveError);
        if (!m_closeAfterOperation) return; m_closeAfterOperation = false; if (success) QTimer::singleShot(0, this, [this] { close(); }); else emit closeCancelled();
    });
    connect(m_preview, &MarkdownPreview::previewError, this, [this](const QString &message) { m_previewError = message; updateState(); });
    connect(m_preview, &MarkdownPreview::previewReady, this, [this] { m_previewError.clear(); m_navigationMessage.clear(); updateState(); });
    connect(m_preview, &MarkdownPreview::fragmentNavigationMessage, this, [this](const QString &message) {
        m_navigationMessage = message; if (!message.isEmpty()) m_dismissedNotice.clear(); updateState();
    });
    m_documentUiReady = true; setEditing(false); updateState();
}
SingleFileWindow::~SingleFileWindow() {
    // QWidget's native close can deliver Hide from its base destructor. Stop
    // the controller while our UI members are still alive, before it can emit
    // geometryWarning back into an already destroyed derived object.
    if (m_localResourceRoot) m_localResourceRoot->revoke();
    if (m_geometry) { m_geometry->flush(); delete m_geometry; m_geometry = nullptr; }
    qApp->removeEventFilter(this);
    disconnect(m_appearance, nullptr, this, nullptr);
    if (m_document) disconnect(m_document, nullptr, this, nullptr);
    if (m_editor) disconnect(m_editor, nullptr, this, nullptr);
    if (m_preview) disconnect(m_preview, nullptr, this, nullptr);
    if (m_footer && m_footer->parentWidget() != this) delete m_footer;
}
QString SingleFileWindow::displayPath() const { return m_document ? m_document->path() : m_openPath; }
bool SingleFileWindow::canTransfer() const { return !m_imageImportPending && !m_prompting && !m_saveErrorDialog && m_document && !m_document->closed() && !m_document->saving() && !m_document->loading() && !m_document->session()->isBusy() && !m_document->session()->isComposing() && (!m_editor || !m_editor->dispatching()); }
bool SingleFileWindow::embedIn(QWidget *parent) {
    if (!parent || m_embedded || (m_document && !canTransfer())) return false;
    if (m_localResourceRoot) m_localResourceRoot->revoke();
    m_localResourceRoot.reset(); m_localResourcePath.clear();
    // Keep the view objects themselves: Scintilla selection/undo and the web
    // reader position are not reconstructed from a second text buffer.
    if (m_geometry) { m_geometry->flush(); delete m_geometry; m_geometry = nullptr; }
    const auto selectionStart = m_editor ? m_editor->send(SCI_GETANCHOR) : 0, selectionEnd = m_editor ? m_editor->send(SCI_GETCURRENTPOS) : 0;
    const auto firstLine = m_editor ? m_editor->send(SCI_GETFIRSTVISIBLELINE) : 0, xOffset = m_editor ? m_editor->send(SCI_GETXOFFSET) : 0;
    const auto reading = m_preview ? m_preview->readingPosition() : QPointF();
    hide(); m_header->hide(); setMinimumSize(0, 0); setParent(parent, Qt::Widget); resetNativeFrame(); m_embedded = true;
    m_modeLayout->removeWidget(m_read); m_modeLayout->removeWidget(m_edit); m_modeLayout->addWidget(m_edit); m_modeLayout->addWidget(m_split); m_modeLayout->addWidget(m_read); m_split->show();
    m_pathButton->setSymbol("sidebar"); m_pathButton->setAccessibleName(QStringLiteral("切换工作区侧栏")); m_pathButton->setToolTip(QStringLiteral("显示或收起文件导航"));
    if (auto *action = findChild<QAction *>("transferWorkspaceAction")) action->setVisible(false);
    if (m_editor) { m_editor->send(SCI_SETSEL, uptr_t(selectionStart), sptr_t(selectionEnd)); m_editor->send(SCI_SETFIRSTVISIBLELINE, uptr_t(firstLine)); m_editor->send(SCI_SETXOFFSET, uptr_t(xOffset)); }
    if (m_preview) m_preview->restoreReadingPosition(reading);
    updateState(); return true;
}

void SingleFileWindow::setWorkspaceResourceRoot(std::shared_ptr<WorkspaceResourceReader> root, const QString &path) {
    m_workspaceResourceRoot = std::move(root); m_workspaceResourcePath = path;
    syncPreviewResources();
    updateState();
}

void SingleFileWindow::syncPreviewResources() {
    if (!m_preview || !m_document || m_document->closed()) return;
    QString base;
    std::shared_ptr<WorkspaceResourceReader> root;
    if (m_embedded) { base = m_workspaceResourcePath; root = m_workspaceResourceRoot; }
    else {
        base = QFileInfo(m_document->path()).absolutePath();
        if (base != m_localResourcePath) {
            if (m_localResourceRoot) m_localResourceRoot->revoke();
            m_localResourcePath = base;
            m_localResourceRoot = WorkspaceResourceReader::open(base).reader;
        }
        root = m_localResourceRoot;
    }
    const auto relative = base.isEmpty() ? QString() : QDir(base).relativeFilePath(m_document->path());
    m_preview->setResourceContext(std::move(root), relative, m_document->identity());
}
void SingleFileWindow::activateDocument() { if (m_document) { m_dismissedNotice.clear(); m_document->checkExternal(); } if (m_editing && m_editor) m_editor->setFocus(Qt::OtherFocusReason); }
void SingleFileWindow::reportHostMessage(const QString &message) { m_hostMessage = message; m_dismissedNotice.clear(); updateState(); }
void SingleFileWindow::setEditing(bool editing) { setViewMode(editing ? ViewMode::Source : ViewMode::Reading); }
void SingleFileWindow::setViewMode(ViewMode mode) {
    if (!m_document || (!m_embedded && mode == ViewMode::Split) || (mode == ViewMode::Source && m_document->readOnly())) { updateState(); return; }
    m_mode = mode; m_editing = mode != ViewMode::Reading;
    m_body->setCurrentWidget(m_splitter); m_editorPage->setVisible(mode != ViewMode::Reading); m_previewPane->setVisible(mode != ViewMode::Source); m_previewHeading->setVisible(mode == ViewMode::Split);
    if (m_editing) m_editor->setFocus(Qt::OtherFocusReason); updateState();
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
    // Native events may synchronously notify the window while setDocument is
    // still constructing controls. The final call in setDocument applies the
    // accumulated state after all document widgets exist.
    if (m_document && !m_documentUiReady) return;
    syncPreviewResources();
    emit viewStateChanged();
    const bool ready = m_document && !m_document->closed(); const bool busy = ready && (m_document->saving() || m_document->loading());
    if (ready && m_document->saving()) m_observedSave = true;
    m_read->setEnabled(ready); m_edit->setEnabled(ready && !m_document->readOnly()); m_read->setChecked(m_mode == ViewMode::Reading); m_edit->setChecked(m_mode == ViewMode::Source); m_split->setEnabled(ready); m_split->setChecked(m_mode == ViewMode::Split);
    m_pathButton->setEnabled(ready); m_zoomButton->setEnabled(ready); m_save->setEnabled(ready && !busy && (m_document->dirty() || m_document->conflict()) && (!m_document->readOnly() || m_document->conflict()));
    m_save->setProperty("primary", m_editing && ready && m_document->dirty()); m_save->setText(busy ? QStringLiteral("处理中…") : QStringLiteral("保存")); m_save->update();
    m_saveSpinner->setVisible(busy); m_saveSpinner->setRunning(busy); m_statusIcon->setVisible(!busy);
    m_saveAction->setEnabled(m_save->isEnabled()); m_saveAsAction->setEnabled(ready && !busy); m_reloadAction->setEnabled(ready && !busy);
    m_imageImportAction->setEnabled(ready && m_editing && m_embedded && m_workspaceResourceRoot && !m_document->readOnly() && !busy && !m_imageImportPending);
    m_linkInsertAction->setEnabled(ready && m_editing && m_embedded && m_workspaceResourceRoot && !m_document->readOnly() && !busy && !m_linkInsertPending);
    m_undoAction->setEnabled(ready && m_editing && !m_document->readOnly() && m_editor && m_editor->send(SCI_CANUNDO)); m_redoAction->setEnabled(ready && m_editing && !m_document->readOnly() && m_editor && m_editor->send(SCI_CANREDO));
    for (auto *button : m_formats) button->setEnabled(ready && !m_document->readOnly()); updateCaret();
    if (!ready) {
        const bool loading = !m_intent.isNull(), error = !m_openError.isEmpty();
        m_openSpinner->setVisible(loading); m_openSpinner->setRunning(loading); m_emptySymbol->setVisible(!loading);
        m_emptySymbol->setPixmap(themedIcon(error ? "error" : "folder-open", error ? m_appearance->semantic("error") : m_appearance->color("textSecondary")).pixmap(QSize(40, 40), devicePixelRatioF()));
        m_emptyTitle->setText(loading ? QStringLiteral("正在打开文档") : error ? QStringLiteral("无法打开文件") : QStringLiteral("打开 Markdown 文档"));
        m_welcomeOpen->setVisible(!loading); m_cancelOpen->setVisible(loading);
        m_header->setTitle(m_openPath.isEmpty() ? QStringLiteral("打开 Markdown 文档") : QFileInfo(m_openPath).fileName(), false); m_path->setFullText(m_openPath.isEmpty() ? QString() : QFileInfo(m_openPath).path());
        m_empty->setText(loading ? QFileInfo(m_openPath).fileName() : error ? m_openError + "\n\n" + m_openPath : QStringLiteral("从阅读开始，随时切换编辑。"));
        m_status->setFullText(loading ? QStringLiteral("正在打开…") : error ? QStringLiteral("打开失败") : QStringLiteral("请选择文档")); m_statusIcon->clear(); m_encoding->clear();
        const auto notice = m_appearanceError.isEmpty() ? m_geometryError : m_appearanceError;
        m_notice->setText(notice); m_banner->setVisible(!notice.isEmpty()); m_resolve->hide(); m_retry->hide(); m_later->hide(); return;
    }
    m_header->setTitle(QFileInfo(m_document->path()).fileName(), m_document->dirty()); setWindowTitle(QFileInfo(m_document->path()).fileName() + QStringLiteral("[*] — xdnote")); setWindowModified(m_document->dirty());
    m_path->setFullText(QFileInfo(m_document->path()).path()); m_status->setFullText(m_document->statusText()); m_encoding->setText(m_document->bom() ? "UTF-8 BOM" : "UTF-8");
    const bool warning = m_document->dirty() || m_document->conflict(); const bool error = m_document->hasError();
    const auto statusColor = busy || (m_document->readOnly() && !error && !warning) ? m_appearance->color("textSecondary") : m_appearance->semantic(error ? "error" : warning ? "warning" : "success");
    m_statusIcon->setProperty("semanticColor", statusColor.name());
    m_statusIcon->setPixmap(themedIcon(busy ? "clock" : error ? "warning" : warning ? "status-dirty" : m_document->readOnly() ? "lock" : "check-circle", statusColor).pixmap(QSize(16, 16), devicePixelRatioF()));
    auto message = m_document->message(); if (message.isEmpty()) message = m_document->session()->rejection();
    if (message.isEmpty()) message = m_appearanceError;
    if (message.isEmpty()) message = m_geometryError;
    if (message.isEmpty()) message = m_navigationMessage;
    if (message.isEmpty()) message = m_hostMessage;
    if (message.isEmpty() && m_document->readOnly()) message = QStringLiteral("此文件为只读，阅读不受影响。");
    m_notice->setText(message); const bool previewOnly = !m_previewError.isEmpty() && message == m_previewError;
    const bool appearanceOnly = !message.isEmpty() && (message == m_appearanceError || message == m_geometryError);
    m_banner->setVisible(!message.isEmpty() && message != m_dismissedNotice);
    const auto semantic = m_appearance->semantic(error ? "error" : m_document->conflict() || m_document->readOnly() ? "warning" : "error");
    auto tint = m_appearance->color("background"); tint.setRed((tint.red() * 9 + semantic.red()) / 10); tint.setGreen((tint.green() * 9 + semantic.green()) / 10); tint.setBlue((tint.blue() * 9 + semantic.blue()) / 10);
    m_banner->setStyleSheet(QStringLiteral("QWidget#documentBanner{background:%1;border-bottom:1px solid %2}").arg(tint.name(), m_appearance->color("border").name()));
    m_noticeSymbol->setPixmap(themedIcon(m_document->readOnly() ? "lock" : error ? "error" : "warning", semantic).pixmap(QSize(20, 20), devicePixelRatioF()));
    m_previewFailureText->setText(m_previewError); m_previewStack->setCurrentWidget(m_previewError.isEmpty() ? static_cast<QWidget *>(m_preview) : m_previewFailure);
    m_resolve->setText(m_document->conflict() ? (m_document->dirty() ? QStringLiteral("处理冲突") : QStringLiteral("重新载入")) : previewOnly ? QStringLiteral("重试预览") : QStringLiteral("另存为…"));
    m_resolve->setVisible(!appearanceOnly && (previewOnly || error || m_document->conflict() || m_document->readOnly())); m_resolve->setEnabled(!busy);
    m_retry->setVisible(error && !m_document->conflict() && !m_document->readOnly() && m_document->lastSave().problem.code != FileError::HardLinks && m_document->lastSave().status != SaveStatus::Uncertain); m_retry->setEnabled(!busy);
    m_later->setVisible(m_document->conflict() && !m_document->dirty());
    static_cast<SharedButton *>(m_edit)->setSymbol(m_document->readOnly() ? "lock" : QString()); m_edit->setToolTip(m_document->readOnly() ? QStringLiteral("文件只读，可另存为副本后编辑") : QStringLiteral("编辑 Markdown 源文"));
}
void SingleFileWindow::chooseOpen() {
    const auto path = QFileDialog::getOpenFileName(window(), QStringLiteral("打开 Markdown 文件"), {}, QStringLiteral("Markdown 文档 (*.md *.markdown);;所有文件 (*)"));
    InputModality::forApplication()->clear(); if (!path.isEmpty()) emit openRequested(path);
}
void SingleFileWindow::chooseSaveAs() {
    if (!m_document || m_document->saving() || m_document->loading()) return;
    const auto path = QFileDialog::getSaveFileName(window(), QStringLiteral("另存为"), m_document->path(), QStringLiteral("Markdown 文档 (*.md *.markdown);;所有文件 (*)"));
    InputModality::forApplication()->clear(); if (!path.isEmpty()) m_document->saveAs(path);
}
void SingleFileWindow::showSaveError() {
    if (!m_document || m_document->closed() || !m_document->hasError() || m_saveErrorDialog) return;
    const auto result = m_document->lastSave();
    const bool uncertain = result.status == SaveStatus::Uncertain;
    const bool diskFull = result.problem.description().contains(QStringLiteral("空间不足"));
    const auto title = uncertain ? QStringLiteral("需要核对保存结果") : diskFull ? QStringLiteral("存储空间不足") : result.problem.code == FileError::Denied ? QStringLiteral("没有写入权限") : result.problem.code == FileError::InUse ? QStringLiteral("文件正在被占用") : result.problem.code == FileError::HardLinks ? QStringLiteral("请另存为独立文件") : QStringLiteral("未能保存文档");
    auto *dialog = new SharedDialog(m_appearance, window(), 480); m_saveErrorDialog = dialog; dialog->setObjectName("saveErrorDialog"); dialog->setWindowTitle(title); dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto *layout = new QVBoxLayout(dialog->surface()); layout->setContentsMargins(24, 20, 24, 24); layout->setSpacing(16);
    auto *top = new QHBoxLayout; auto *symbol = new QLabel(dialog); symbol->setPixmap(themedIcon(uncertain ? "warning" : "error", m_appearance->semantic(uncertain ? "warning" : "error")).pixmap(24, 24)); top->addWidget(symbol); top->addStretch();
    auto *close = new SharedButton({}, "dialogClose", m_appearance, dialog); close->setSymbol("close"); close->setAccessibleName(QStringLiteral("关闭对话框")); top->addWidget(close); connect(close, &QPushButton::clicked, dialog, &QDialog::reject); layout->addLayout(top);
    auto *heading = new QLabel(title, dialog); heading->setFont(uiFont(18)); heading->setTextFormat(Qt::PlainText); layout->addWidget(heading);
    auto *body = new QLabel(m_document->message(), dialog); body->setTextFormat(Qt::PlainText); body->setWordWrap(true); body->setTextInteractionFlags(Qt::TextSelectableByMouse);
    const int textHeight = QFontMetrics(body->font()).boundingRect(QRect(0, 0, 432, 100000), Qt::TextWordWrap, body->text()).height(), cap = qMax(80, screen()->availableGeometry().height() / 3);
    if (textHeight > cap) { auto *scroll = new QScrollArea(dialog); scroll->setFrameShape(QFrame::NoFrame); scroll->setWidgetResizable(true); body->setMinimumHeight(textHeight + 8); scroll->setWidget(body); scroll->setFixedHeight(cap); layout->addWidget(scroll); } else layout->addWidget(body);
    auto *file = new ElidedLabel(dialog); file->setObjectName("saveErrorFile"); file->setFullText(m_document->path()); layout->addWidget(file);
    if (uncertain) { auto *note = new QLabel(QStringLiteral("编辑内容仍保留在当前窗口。请先核对磁盘文件和恢复副本，再决定后续操作。"), dialog); note->setWordWrap(true); layout->addWidget(note); }
    auto *actions = new QHBoxLayout; actions->addStretch();
    if (!uncertain && result.problem.code != FileError::HardLinks && result.problem.code != FileError::Denied) {
        auto *retry = new SharedButton(QStringLiteral("重试"), "errorRetry", m_appearance, dialog); retry->setOutlined(true); actions->addWidget(retry);
        connect(retry, &QPushButton::clicked, this, [this, dialog] { dialog->accept(); QTimer::singleShot(0, this, [this] { if (m_document) m_document->save(); }); });
    }
    auto *saveAs = new SharedButton(QStringLiteral("另存为…"), "errorSaveAs", m_appearance, dialog); saveAs->setOutlined(true); actions->addWidget(saveAs); connect(saveAs, &QPushButton::clicked, this, [this, dialog] { dialog->accept(); QTimer::singleShot(0, this, &SingleFileWindow::chooseSaveAs); });
    auto *keep = new SharedButton(QStringLiteral("继续编辑"), "errorKeepEditing", m_appearance, dialog); keep->setProperty("primary", true); keep->setDefault(true); actions->addWidget(keep); connect(keep, &QPushButton::clicked, dialog, &QDialog::accept); layout->addLayout(actions);
    dialog->adjustSize(); dialog->open(); keep->setFocus(Qt::OtherFocusReason);
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

void SingleFileWindow::chooseWorkspaceDocumentLink() {
    const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("选择工作区文档"), m_workspaceResourcePath,
        QStringLiteral("Markdown 文档 (*.md *.markdown)"));
    if (!path.isEmpty()) insertWorkspaceDocumentLink(path);
}

void SingleFileWindow::insertWorkspaceDocumentLink(const QString &targetPath) {
    auto reject = [this](const QString &message) { reportHostMessage(message); emit workspaceLinkFinished(false, message); };
    if (m_linkInsertPending) { reject(QStringLiteral("正在检查所选文档，请稍候。")); return; }
    if (!m_document || !m_editor || !m_editing || !m_embedded || !m_workspaceResourceRoot || m_workspaceResourcePath.isEmpty()) {
        reject(QStringLiteral("请先在工作区打开文档并切换到编辑模式。")); return;
    }
    const auto suffix = QFileInfo(targetPath).suffix().toLower();
    if (!QDir::isAbsolutePath(targetPath) || (suffix != "md" && suffix != "markdown")) {
        reject(QStringLiteral("请选择工作区内的 Markdown 文档。")); return;
    }
    QPointer<FileDocument> document = m_document; QPointer<DocumentSession> session = document->session();
    if (!session || document->closed() || document->saving() || document->loading() || session->isReadOnly()
        || session->isBusy() || session->isComposing() || session->isClosing()) {
        reject(QStringLiteral("文档当前无法编辑，请完成正在进行的操作后重试。")); return;
    }
    const auto snapshot = session->snapshot();
    if (!document || !session || snapshot.result != EditResult::Applied || document != m_document) {
        reject(QStringLiteral("无法取得当前文档的编辑位置。")); return;
    }
    const auto anchor = qsizetype(m_editor->send(SCI_GETANCHOR)), caret = qsizetype(m_editor->send(SCI_GETCURRENTPOS));
    const auto start = qMin(anchor, caret), end = qMax(anchor, caret);
    if (start < 0 || end > snapshot.utf8.size()) { reject(QStringLiteral("当前编辑位置无效。")); return; }
    const auto before = snapshot.utf8.left(start), selected = snapshot.utf8.mid(start, end - start);
    const auto beforeText = decodeUtf8Literal(before), selectedText = decodeUtf8Literal(selected);
    if (beforeText.toUtf8() != before || selectedText.toUtf8() != selected) { reject(QStringLiteral("当前编辑位置无效。")); return; }
    auto label = selectedText.isEmpty() ? QFileInfo(targetPath).fileName() : selectedText;
    if (label.contains('\n') || label.contains('\r') || label.contains(QChar(0x2028)) || label.contains(QChar(0x2029))) {
        reject(QStringLiteral("请选择单行文字作为链接名称。")); return;
    }
    label.replace("\\", "\\\\").replace("[", "\\[").replace("]", "\\]");
    const auto root = m_workspaceResourceRoot; const auto rootPath = m_workspaceResourcePath;
    const auto relativeDocument = QDir(rootPath).relativeFilePath(document->path());
    const auto relativeTarget = QDir(rootPath).relativeFilePath(QDir::cleanPath(targetPath));
    const auto identity = document->identity(); const int first = int(beforeText.size()), last = int(beforeText.size() + selectedText.size());
    m_linkInsertPending = true; m_hostMessage.clear(); updateState();
    auto *worker = new QFutureWatcher<LinkTargetResult>(this);
    connect(worker, &QFutureWatcher<LinkTargetResult>::finished, this,
        [this, worker, document, session, snapshot, root, rootPath, identity, anchor, caret, first, last, label] {
        const auto result = worker->result(); worker->deleteLater(); m_linkInsertPending = false;
        auto message = result.message; bool inserted = false;
        if (result.success) {
            bool current = document && session && document == m_document && m_editor && m_editing
                && !document->closed() && !document->saving() && !document->loading() && document->identity() == identity
                && m_workspaceResourceRoot == root && m_workspaceResourcePath == rootPath && root->active()
                && !session->isReadOnly() && !session->isBusy() && !session->isComposing() && !session->isClosing();
            if (current) {
                const auto latest = session->snapshot();
                current = document && session && document == m_document && m_editor && m_editing && !document->closed()
                    && !document->saving() && !document->loading() && document->identity() == identity
                    && m_workspaceResourceRoot == root && m_workspaceResourcePath == rootPath && root->active()
                    && !session->isReadOnly() && !session->isBusy() && !session->isComposing() && !session->isClosing()
                    && latest.result == EditResult::Applied && latest.documentId == snapshot.documentId && latest.revision == snapshot.revision
                    && qsizetype(m_editor->send(SCI_GETANCHOR)) == anchor && qsizetype(m_editor->send(SCI_GETCURRENTPOS)) == caret;
            }
            if (current) {
                const auto edit = session->replace(first, last, QStringLiteral("[%1](%2)").arg(label, result.url), snapshot.revision);
                inserted = edit == EditResult::Applied;
                message = inserted ? QStringLiteral("文档链接已插入，文档修改尚未保存。")
                                   : QStringLiteral("无法插入文档链接：%1。").arg(resultDescription(edit));
            } else message = QStringLiteral("文档或插入位置已变化，未插入链接。请重新选择。");
        }
        if (message.isEmpty()) message = QStringLiteral("无法插入文档链接。");
        reportHostMessage(message); emit workspaceLinkFinished(inserted, message);
    });
    worker->setFuture(QtConcurrent::run([root, relativeDocument, identity, relativeTarget] {
        return prepareWorkspaceLink(root, relativeDocument, identity, relativeTarget);
    }));
}

void SingleFileWindow::chooseImage() {
    const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("选择图片"), {},
        QStringLiteral("图片 (*.png *.jpg *.jpeg *.gif *.bmp *.webp);;所有文件 (*)"));
    if (!path.isEmpty()) importImageFile(path);
}
void SingleFileWindow::importImageFile(const QString &path) { if (!path.isEmpty()) beginImageImport(path, {}); }
void SingleFileWindow::importClipboardImage(const QImage &image) { beginImageImport({}, image); }

void SingleFileWindow::beginImageImport(const QString &sourcePath, const QImage &bitmap) {
    auto reject = [this](const QString &message) { reportHostMessage(message); emit imageImportFinished(false, message); };
    if (m_imageImportPending) { reject(QStringLiteral("图片正在导入，请等待完成。")); return; }
    if (!m_document || !m_editor || !m_editing || !m_embedded || !m_workspaceResourceRoot || m_workspaceResourcePath.isEmpty()) {
        reject(QStringLiteral("请先在工作区打开文档并切换到编辑模式，再插入图片。")); return;
    }
    QPointer<FileDocument> document = m_document;
    QPointer<DocumentSession> session = document->session();
    if (!session || document->closed() || document->saving() || document->loading() || session->isReadOnly()
        || session->isBusy() || session->isComposing() || session->isClosing()) {
        reject(QStringLiteral("文档当前无法编辑，请完成正在进行的操作后重试。")); return;
    }
    const auto snapshot = session->snapshot();
    if (!document || !session || snapshot.result != EditResult::Applied || document != m_document) {
        reject(QStringLiteral("无法取得当前文档的编辑位置。")); return;
    }
    const auto anchor = qsizetype(m_editor->send(SCI_GETANCHOR));
    const auto caret = qsizetype(m_editor->send(SCI_GETCURRENTPOS));
    const auto start = qMin(anchor, caret), end = qMax(anchor, caret);
    if (start < 0 || end > snapshot.utf8.size()) { reject(QStringLiteral("当前编辑位置无效。")); return; }
    const auto before = snapshot.utf8.left(start), selected = snapshot.utf8.mid(start, end - start);
    const auto beforeText = decodeUtf8Literal(before), selectedText = decodeUtf8Literal(selected);
    if (beforeText.toUtf8() != before || selectedText.toUtf8() != selected) { reject(QStringLiteral("当前编辑位置无效。")); return; }
    const auto root = m_workspaceResourceRoot;
    const auto rootPath = m_workspaceResourcePath;
    const auto relativeDocument = QDir(rootPath).relativeFilePath(document->path());
    const auto identity = document->identity();
    const int first = int(beforeText.size()), last = int(beforeText.size() + selectedText.size());
    m_imageImportPending = true; m_hostMessage.clear(); updateState();
    auto *worker = new QFutureWatcher<ImageImportResult>(this);
    connect(worker, &QFutureWatcher<ImageImportResult>::finished, this,
        [this, worker, document, session, snapshot, root, rootPath, relativeDocument, identity, anchor, caret, first, last] {
        auto result = worker->future().takeResult(); worker->deleteLater();
        m_imageImportPending = false;
        auto &publication = result.publication;
        const auto actualPath = publication.path.isEmpty() ? QString() : QDir(rootPath).filePath(publication.path);
        const auto recoveryPath = publication.recoveryPath.isEmpty() ? QString() : QDir(rootPath).filePath(publication.recoveryPath);
        QString message = result.message;
        bool inserted = false;
        if (publication.status == WorkspaceWriteStatus::Committed || publication.status == WorkspaceWriteStatus::Reused) {
            const auto target = relativeWorkspaceUrl(relativeDocument, publication.path);
            bool current = document && session && document == m_document && m_editor && m_editing
                && !document->closed() && !document->saving() && !document->loading()
                && document->identity() == identity && m_workspaceResourceRoot == root && m_workspaceResourcePath == rootPath
                && root->active() && !session->isReadOnly() && !session->isBusy() && !session->isComposing() && !session->isClosing();
            if (current) {
                const auto latest = session->snapshot();
                current = document && session && document == m_document && m_editor && m_editing && !document->closed()
                    && !document->saving() && !document->loading() && document->identity() == identity
                    && !session->isReadOnly() && !session->isBusy() && !session->isComposing() && !session->isClosing()
                    && root->active() && m_workspaceResourceRoot == root && m_workspaceResourcePath == rootPath && latest.result == EditResult::Applied
                    && latest.documentId == snapshot.documentId && latest.revision == snapshot.revision
                    && qsizetype(m_editor->send(SCI_GETANCHOR)) == anchor && qsizetype(m_editor->send(SCI_GETCURRENTPOS)) == caret;
            }
            if (current && target.success) {
                const auto edit = session->replace(first, last, QStringLiteral("![图片](%1)").arg(target.path), snapshot.revision);
                inserted = edit == EditResult::Applied;
            }
            message = inserted
                ? QStringLiteral("图片已%1：%2。文档修改尚未保存。").arg(publication.status == WorkspaceWriteStatus::Reused ? QStringLiteral("复用") : QStringLiteral("导入"), actualPath)
                : QStringLiteral("图片位于 %1；文档或插入位置已变化，未插入引用。请检查后重试。").arg(actualPath);
        } else if (publication.status == WorkspaceWriteStatus::PublishedButUnverified) {
            message = QStringLiteral("图片写入结果无法确认，未插入引用。请检查：%1").arg(actualPath);
        }
        if (!recoveryPath.isEmpty()) message += QStringLiteral("\n请检查临时文件：%1").arg(recoveryPath);
        if (message.isEmpty()) message = QStringLiteral("图片导入失败，未插入引用。");
        publication.lease.reset();
        reportHostMessage(message); emit imageImportFinished(inserted, message);
    });
    worker->setFuture(QtConcurrent::run([root, relativeDocument, identity, sourcePath, bitmap] {
        return importWorkspaceImage(root, relativeDocument, identity, sourcePath, bitmap);
    }));
}

void SingleFileWindow::closeEvent(QCloseEvent *event) {
    if (m_imageImportPending) { reportHostMessage(QStringLiteral("图片正在导入，请等待完成后再关闭文档。")); event->ignore(); emit closeCancelled(); return; }
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
    const bool sourceTarget = m_editor && (widget == m_editor || m_editor->isAncestorOf(widget));
    if (event->type() == QEvent::DragEnter || (sourceTarget && event->type() == QEvent::DragMove)) {
        auto *drag = static_cast<QDragMoveEvent *>(event);
        if (drag->mimeData()->hasUrls()) drag->acceptProposedAction(); else drag->ignore();
        if (sourceTarget || drag->mimeData()->hasUrls()) return true;
    }
    if (event->type() == QEvent::Drop) {
        auto *drop = static_cast<QDropEvent *>(event); const auto urls = drop->mimeData()->urls();
        if (sourceTarget && urls.size() == 1 && urls.first().isLocalFile()) {
            const auto path = urls.first().toLocalFile();
            static const QStringList suffixes{"png", "jpg", "jpeg", "gif", "bmp", "webp"};
            if (suffixes.contains(QFileInfo(path).suffix().toLower())) {
                const auto point = widget->mapTo(m_editor, drop->position().toPoint());
                const auto position = m_editor->send(SCI_POSITIONFROMPOINTCLOSE, uptr_t(point.x()), sptr_t(point.y()));
                if (position >= 0) m_editor->send(SCI_SETSEL, uptr_t(position), position);
                importImageFile(path); drop->acceptProposedAction(); return true;
            }
        }
        if (!urls.isEmpty() && urls.size() <= 16) { for (const auto &url : urls) if (url.isLocalFile()) emit openRequested(url.toLocalFile()); drop->acceptProposedAction(); return true; }
        if (sourceTarget) { drop->ignore(); return true; }
    }
    if (widget == this && event->type() == QEvent::WindowActivate && m_document) { m_dismissedNotice.clear(); m_document->checkExternal(); }
    if (widget == this && event->type() == QEvent::WindowStateChange) { m_header->updateWindowState(); QTimer::singleShot(0, this, &SingleFileWindow::applyNativeAppearance); }
    if (widget == this && (event->type() == QEvent::Resize || event->type() == QEvent::ScreenChangeInternal)) QTimer::singleShot(0, this, &SingleFileWindow::updateState);
    return QWidget::eventFilter(target, event);
}
}
