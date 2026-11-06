#include "WorkspaceWindow.h"
#include "WorkspaceTree.h"
#include "SingleFileWindow.h"
#include "SourceEditor.h"
#include "Appearance.h"
#include "SharedWidgets.h"
#include "WindowGeometry.h"
#include "VisualTokens.h"
#include "InputModality.h"
#include "UiFonts.h"
#include "NavigationStore.h"
#include "WorkspaceNavigation.h"
#include <QTabBar>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QStackedWidget>
#include <QLabel>
#include <QPainter>
#include <QFileInfo>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QAction>
#include <QLineEdit>
#include <QDir>
#include <QFocusEvent>
#include <QSignalBlocker>
#include <QListWidget>
#include <QPainterPath>
#include <QMouseEvent>

namespace xdnote {
namespace {
QJsonObject mainTokens() { return visualTokens().value("windows").toObject().value("main").toObject(); }
int tabToken(const char *key) { return visualTokens().value("tabs").toObject().value(key).toInt(); }
int navToken(const char *key) { return visualTokens().value("navigation").toObject().value(key).toInt(); }
QUuid tabId(QTabBar *tabs, int index) { return tabs->tabData(index).toMap().value("id").toUuid(); }
class WorkspaceTabs final : public QTabBar {
public:
    WorkspaceTabs(Appearance *appearance, QWidget *parent) : QTabBar(parent), m_appearance(appearance) {
        setFont(parent->font()); setAttribute(Qt::WA_MacShowFocusRect, false); setMouseTracking(true);
        setObjectName("workspaceTabs"); setAccessibleName(QStringLiteral("文档标签")); setExpanding(false); setMovable(true); setUsesScrollButtons(true); setElideMode(Qt::ElideMiddle); setDrawBase(false); setFixedHeight(mainTokens().value("titlebarHeight").toInt()); setFocusPolicy(Qt::StrongFocus);
        auto refresh = [this] { auto palette = this->palette(); palette.setColor(QPalette::ButtonText, m_appearance->color("text")); palette.setColor(QPalette::WindowText, m_appearance->color("text")); setPalette(palette); setStyleSheet(QStringLiteral("QTabBar#workspaceTabs QToolButton{background:%1;color:%2;border:0;border-bottom:1px solid %4} QTabBar#workspaceTabs QToolButton:hover{background:%3}").arg(m_appearance->color("sidebar").name(), m_appearance->color("text").name(), m_appearance->color("codeSurface").name(), m_appearance->color("border").name())); update(); }; connect(appearance, &Appearance::changed, this, refresh); refresh(); connect(InputModality::forApplication(), &InputModality::changed, this, qOverload<>(&QWidget::update));
    }
    std::function<void(QUuid)> closeRequested;
    std::function<void()> layoutChanged;
    int preferredWidth() const { int width = 0; for (int i = 0; i < count(); ++i) width += tabSizeHint(i).width(); return width; }
protected:
    QSize tabSizeHint(int index) const override { return {qBound(tabToken("minWidth"), fontMetrics().horizontalAdvance(tabData(index).toMap().value("title").toString()) + 2 * tabToken("padding") + 16 + tabToken("closeHit"), tabToken("maxWidth")), height()}; }
    QSize minimumTabSizeHint(int) const override { return {tabToken("minWidth"), height()}; }
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this); painter.setFont(font()); painter.setRenderHint(QPainter::Antialiasing); painter.fillRect(rect(), m_appearance->color("sidebar"));
        const int border = visualTokens().value("controls").toObject().value("borderWidth").toInt(); painter.fillRect(QRect(0, height() - border, width(), border), m_appearance->color("border"));
        for (int i = 0; i < count(); ++i) {
            const auto rect = tabRect(i); if (!rect.intersects(this->rect())) continue;
            const auto data = tabData(i).toMap(); const bool active = i == currentIndex();
            const auto surface = QRectF(rect).adjusted(2 + border / 2.0, tabToken("topInset") + border / 2.0, -2 - border / 2.0, 0); const qreal radius = tabToken("cornerRadius");
            if (active || i == m_hover) {
                QPainterPath outline; outline.moveTo(surface.left(), height()); outline.lineTo(surface.left(), surface.top() + radius); outline.quadTo(surface.left(), surface.top(), surface.left() + radius, surface.top()); outline.lineTo(surface.right() - radius, surface.top()); outline.quadTo(surface.right(), surface.top(), surface.right(), surface.top() + radius); outline.lineTo(surface.right(), height());
                painter.fillPath(outline, m_appearance->color(active ? "background" : "codeSurface"));
                if (active) { painter.setPen(QPen(m_appearance->color("border"), border)); painter.setBrush(Qt::NoBrush); painter.drawPath(outline); }
                else painter.fillRect(QRect(rect.left(), height() - border, rect.width(), border), m_appearance->color("border"));
            }
            if (!active && i + 1 != currentIndex()) { painter.setPen(QPen(m_appearance->color("border"), border)); painter.drawLine(QPointF(rect.right() + .5, tabToken("separatorInset")), QPointF(rect.right() + .5, height() - tabToken("separatorInset"))); }
            const int left = rect.x() + tabToken("padding"); const QRect icon(left, (height() - 16) / 2, 16, 16);
            themedIcon(data.value("dirty").toBool() ? "status-dirty" : "document", data.value("dirty").toBool() ? m_appearance->semantic("warning") : m_appearance->color("textSecondary")).paint(&painter, icon);
            painter.setPen(m_appearance->color(active ? "text" : "textSecondary")); const QRect textRect = rect.adjusted(tabToken("padding") + 24, 0, -tabToken("closeHit") - 8, 0);
            painter.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, fontMetrics().elidedText(data.value("title").toString(), Qt::ElideMiddle, textRect.width()));
            if (active && hasFocus() && InputModality::forApplication()->keyboardNavigation()) { painter.setBrush(Qt::NoBrush); painter.setPen(QPen(m_appearance->accentColor(), 2)); painter.drawRoundedRect(rect.adjusted(3, 3, -3, -3), 5, 5); }
        }
    }
    void keyPressEvent(QKeyEvent *event) override { if (event->key() == Qt::Key_Delete && currentIndex() >= 0) { InputModality::forApplication()->clear(); if (closeRequested) closeRequested(tabId(this, currentIndex())); event->accept(); } else QTabBar::keyPressEvent(event); }
    void mouseMoveEvent(QMouseEvent *event) override { const auto previous = m_hover; m_hover = tabAt(event->position().toPoint()); if (previous != m_hover) update(); QTabBar::mouseMoveEvent(event); }
    void leaveEvent(QEvent *event) override { m_hover = -1; update(); QTabBar::leaveEvent(event); }
    void tabLayoutChange() override { QTabBar::tabLayoutChange(); if (layoutChanged) layoutChanged(); }
private:
    Appearance *m_appearance;
    int m_hover = -1;
};
class WorkspaceTabStrip final : public QWidget {
public:
    WorkspaceTabStrip(Appearance *appearance, QWidget *parent) : QWidget(parent) {
        setObjectName("workspaceTabStrip"); setMinimumWidth(0); setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        tabs = new WorkspaceTabs(appearance, this); add = new SharedButton({}, "workspaceAddTab", appearance, this); add->setSymbol("plus"); add->setFixedSize(32, 32); add->setAccessibleName(QStringLiteral("打开文档标签")); add->setToolTip(QStringLiteral("打开 Markdown 文档…")); tabs->layoutChanged = [this] { arrange(); };
    }
    WorkspaceTabs *tabs;
    SharedButton *add;
    ~WorkspaceTabStrip() override { tabs->layoutChanged = {}; }
protected:
    void resizeEvent(QResizeEvent *event) override { QWidget::resizeEvent(event); arrange(); }
private:
    void arrange() {
        if (m_arranging) return; m_arranging = true;
        const int tabWidth = qMin(tabs->preferredWidth(), qMax(0, width() - add->width() - 12)); tabs->setGeometry(0, 0, tabWidth, height()); add->move(tabWidth + 4, (height() - add->height()) / 2); m_arranging = false;
    }
    bool m_arranging = false;
};
}
WorkspaceWindow::WorkspaceWindow(Appearance *appearance, WindowGeometryStore *store, NavigationStore *navigation) : ApplicationWindow(appearance), m_navigation(navigation ? navigation : new NavigationStore(this)) {
    setObjectName("workspaceWindow"); setAttribute(Qt::WA_DeleteOnClose); const auto tokens = mainTokens(); resize(tokens.value("defaultWidth").toInt(), tokens.value("defaultHeight").toInt()); setMinimumSize(tokens.value("minWidth").toInt(), tokens.value("minHeight").toInt()); setFont(uiFont(visualTokens().value("typography").toObject().value("ui").toObject().value("size").toInt()));
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
    m_header = new WindowTitleBar(windowAppearance(), this); setTitleBar(m_header); auto *tabStrip = new WorkspaceTabStrip(windowAppearance(), m_header); auto *tabs = tabStrip->tabs; m_tabs = tabs; connect(tabStrip->add, &QPushButton::clicked, this, &WorkspaceWindow::chooseFileRequested); m_header->setCenter(tabStrip, tokens.value("sidebarWidth").toInt()); layout->addWidget(m_header);
    tabs->closeRequested = [this](QUuid id) { closePane(id); }; connect(tabs, &QTabBar::currentChanged, this, [this] { m_navigationVisible = false; if (m_search) { const QSignalBlocker block(m_search); m_search->clear(); m_searchDelay.stop(); m_tree->cancelSearch(); } selectCurrent(); }); connect(tabs, &QTabBar::tabBarClicked, this, [this](int index) { if (index >= 0) activatePane(tabId(m_tabs, index)); });
    m_warning = new QLabel(this); m_warning->setObjectName("workspaceNotice"); m_warning->setWordWrap(true); m_warning->setTextFormat(Qt::PlainText); m_warning->setContentsMargins(16, 8, 16, 8); m_warning->hide(); layout->addWidget(m_warning);
    auto *content = new QWidget(this); auto *row = new QHBoxLayout(content); row->setContentsMargins(0, 0, 0, 0); row->setSpacing(0); layout->addWidget(content, 1);
    m_sidebar = new QWidget(content); m_sidebar->setObjectName("workspaceSidebar"); m_sidebar->setFixedWidth(tokens.value("sidebarWidth").toInt()); auto *sidebarRow = new QHBoxLayout(m_sidebar); sidebarRow->setContentsMargins(0, 0, 0, 0); sidebarRow->setSpacing(0); auto *navigationColumn = new QWidget(m_sidebar); sidebarRow->addWidget(navigationColumn, 1); auto *sidebarBorder = new SharedSeparator(Qt::Vertical, windowAppearance(), m_sidebar); sidebarBorder->setObjectName("workspaceSidebarBorder"); sidebarRow->addWidget(sidebarBorder); auto *sidebar = new QVBoxLayout(navigationColumn); sidebar->setContentsMargins(0, 0, 0, 0); sidebar->setSpacing(0); row->addWidget(m_sidebar);
    auto *selector = new QWidget(m_sidebar); selector->setFixedHeight(tokens.value("toolbarHeight").toInt()); auto *tools = new QHBoxLayout(selector); tools->setContentsMargins(navToken("sidebarInset"), 0, navToken("sidebarInset"), 0);
    m_rootLabel = new SharedButton(QStringLiteral("我的文档"), "workspaceSelector", windowAppearance(), selector); m_rootLabel->setProperty("navigationRow", true); auto titleFont = font(); titleFont.setWeight(QFont::DemiBold); m_rootLabel->setFont(titleFont); tools->addWidget(m_rootLabel);
    auto *workspaceMenu = new SharedMenu(windowAppearance(), m_rootLabel); auto *choose = workspaceMenu->addAction(QStringLiteral("打开工作区目录…")); choose->setObjectName("chooseWorkspace"); connect(choose, &QAction::triggered, this, &WorkspaceWindow::chooseDirectoryRequested);
    auto *refresh = workspaceMenu->addAction(QStringLiteral("刷新文件导航")); refresh->setObjectName("refreshWorkspace"); m_rootLabel->setMenu(workspaceMenu); sidebar->addWidget(selector);
    auto *shortcuts = new QWidget(m_sidebar); auto *shortcutsLayout = new QVBoxLayout(shortcuts); shortcutsLayout->setContentsMargins(navToken("sidebarInset"), 0, navToken("sidebarInset"), 0); shortcutsLayout->setSpacing(4);
    m_searchBox = new QWidget(shortcuts); m_searchBox->setObjectName("workspaceSearchBox"); m_searchBox->setFixedHeight(navToken("rowHeight")); auto *searchLayout = new QHBoxLayout(m_searchBox); searchLayout->setContentsMargins(8, 0, 8, 0); searchLayout->setSpacing(6);
    auto *searchIcon = new QLabel(m_searchBox); searchIcon->setObjectName("workspaceSearchIcon"); searchIcon->setFixedSize(16, 16); searchLayout->addWidget(searchIcon);
    m_search = new QLineEdit(m_searchBox); m_search->setObjectName("workspaceSearch"); m_search->setFont(font()); m_search->setPlaceholderText(QStringLiteral("搜索文档")); m_search->setAccessibleName(QStringLiteral("搜索工作区文档文件名")); m_search->setToolTip(QStringLiteral("按文件名搜索工作区，不检索正文")); m_search->setClearButtonEnabled(true); m_search->installEventFilter(this); searchLayout->addWidget(m_search, 1);
    auto *hint = new QLabel(QKeySequence(Qt::CTRL | Qt::Key_K).toString(QKeySequence::NativeText), m_searchBox); hint->setObjectName("workspaceSearchHint"); hint->setFont(uiFont(10)); searchLayout->addWidget(hint); shortcutsLayout->addWidget(m_searchBox);
    auto navButton = [this, shortcutsLayout, shortcuts](const QString &text, const QString &name, const QString &symbol) { auto *button = new SharedButton(text, name, windowAppearance(), shortcuts); button->setProperty("navigationRow", true); button->setSymbol(symbol); button->setFixedHeight(navToken("rowHeight")); shortcutsLayout->addWidget(button); return button; };
    shortcutsLayout->addSpacing(8); m_recent = navButton(QStringLiteral("最近打开"), "workspaceRecent", "clock"); m_favorites = navButton(QStringLiteral("收藏"), "workspaceFavorites", "star"); sidebar->addWidget(shortcuts);
    connect(m_recent, &QPushButton::clicked, this, [this] { showNavigation(false); }); connect(m_favorites, &QPushButton::clicked, this, [this] { showNavigation(true); });
    auto *folderSection = new QWidget(m_sidebar); folderSection->setObjectName("workspaceFolderSection"); auto *folderLayout = new QHBoxLayout(folderSection); folderLayout->setContentsMargins(navToken("sidebarInset") + 4, navToken("sectionGap"), navToken("sidebarInset"), 4); auto *folderLabel = new QLabel(QStringLiteral("文件夹"), folderSection); folderLabel->setProperty("secondary", true); folderLabel->setFont(uiFont(12)); folderLayout->addWidget(folderLabel, 1);
    auto *directory = new SharedButton({}, "addWorkspaceFolder", windowAppearance(), folderSection); directory->setSymbol("plus"); directory->setOutlined(true); directory->setFixedWidth(32); directory->setAccessibleName(QStringLiteral("打开其他工作区目录")); directory->setToolTip(QStringLiteral("打开工作区目录")); folderLayout->addWidget(directory); connect(directory, &QPushButton::clicked, this, &WorkspaceWindow::chooseDirectoryRequested); sidebar->addWidget(folderSection);
    m_treeState = new QLabel(m_sidebar); m_treeState->setObjectName("workspaceTreeState"); m_treeState->setWordWrap(true); m_treeState->setTextFormat(Qt::PlainText); m_treeState->setContentsMargins(16, 8, 16, 8); m_treeState->setFont(uiFont(12)); sidebar->addWidget(m_treeState);
    m_tree = new WorkspaceTree(windowAppearance(), m_sidebar); sidebar->addWidget(m_tree, 1); m_tree->setContentsMargins(8, 0, 8, 0); connect(refresh, &QAction::triggered, m_tree, &WorkspaceTree::refresh); connect(m_tree, &WorkspaceTree::openRequested, this, &WorkspaceWindow::openRequested);
    connect(m_tree, &WorkspaceTree::stateChanged, this, [this] { m_treeState->setText(m_tree->message()); m_treeState->setVisible(!m_tree->message().isEmpty()); }); m_tree->setRootPath({});
    m_pages = new QStackedWidget(content); m_pages->setObjectName("workspacePages"); row->addWidget(m_pages, 1);
    auto *bottom = new QWidget(m_sidebar); bottom->setFixedHeight(tokens.value("toolbarHeight").toInt()); auto *bottomLayout = new QHBoxLayout(bottom); bottomLayout->setContentsMargins(navToken("sidebarInset"), 0, navToken("sidebarInset"), 0); bottomLayout->setSpacing(8);
    auto *settings = new SharedButton(QStringLiteral("设置"), "workspaceSettings", windowAppearance(), bottom); settings->setProperty("navigationRow", true); settings->setSymbol("settings"); settings->setToolTip(QStringLiteral("外观设置：主题和强调色")); settings->setMenu(createAppearanceMenu(windowAppearance(), settings)); bottomLayout->addWidget(settings, 1);
    auto *divider = new QWidget(bottom); divider->setObjectName("sidebarSettingsDivider"); divider->setFixedSize(1, 20); bottomLayout->addWidget(divider); m_theme = new SharedButton({}, "workspaceTheme", windowAppearance(), bottom); m_theme->setFixedWidth(32); bottomLayout->addWidget(m_theme); sidebar->addWidget(bottom);
    connect(m_theme, &QPushButton::clicked, this, [this] { windowAppearance()->setPreferences(windowAppearance()->dark() ? "light" : "dark", windowAppearance()->accent()); });
    setTabOrder(m_rootLabel, m_search); setTabOrder(m_search, m_recent); setTabOrder(m_recent, m_favorites); setTabOrder(m_favorites, m_tree); setTabOrder(m_tree, settings); setTabOrder(settings, m_theme);
    m_welcome = new QWidget(m_pages); auto *welcome = new QVBoxLayout(m_welcome); welcome->setSpacing(16); welcome->addStretch(); auto *heading = new QLabel(QStringLiteral("开始你的写作"), m_welcome); heading->setFont(uiFont(24)); welcome->addWidget(heading, 0, Qt::AlignHCenter);
    auto *description = new QLabel(QStringLiteral("打开一篇文档，或选择一个工作区目录。"), m_welcome); welcome->addWidget(description, 0, Qt::AlignHCenter);
    auto *openFile = new SharedButton(QStringLiteral("打开文件…"), "workspaceOpenFile", windowAppearance(), m_welcome); openFile->setSymbol("document"); openFile->setProperty("primary", true); welcome->addWidget(openFile, 0, Qt::AlignHCenter); connect(openFile, &QPushButton::clicked, this, &WorkspaceWindow::chooseFileRequested);
    auto *openDirectory = new SharedButton(QStringLiteral("打开工作区…"), "workspaceOpenDirectory", windowAppearance(), m_welcome); openDirectory->setSymbol("folder-open"); openDirectory->setOutlined(true); welcome->addWidget(openDirectory, 0, Qt::AlignHCenter); connect(openDirectory, &QPushButton::clicked, this, &WorkspaceWindow::chooseDirectoryRequested); welcome->addStretch(); m_pages->addWidget(m_welcome);
    m_navigationPage = new WorkspaceNavigation(windowAppearance(), m_navigation, m_pages); m_pages->addWidget(m_navigationPage); connect(m_navigationPage, &WorkspaceNavigation::openRequested, this, &WorkspaceWindow::openRequested); connect(m_navigationPage, &WorkspaceNavigation::clearSearchRequested, m_search, &QLineEdit::clear);
    m_searchDelay.setSingleShot(true); m_searchDelay.setInterval(navToken("searchDelayMs")); connect(&m_searchDelay, &QTimer::timeout, this, &WorkspaceWindow::startSearch);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &text) { m_searchDelay.stop(); m_tree->cancelSearch(); if (text.trimmed().isEmpty()) { m_navigationVisible = false; selectCurrent(); } else { m_navigationVisible = true; m_navigationPage->showPage(WorkspaceNavigation::Page::Search, rootPath()); m_navigationPage->showSearch(text.trimmed(), {}, {}, true); selectCurrent(); m_searchDelay.start(); } });
    connect(m_tree, &WorkspaceTree::searchFinished, this, [this](const QString &query, const QStringList &paths, const QString &warning) { if (m_navigationVisible && query == m_search->text().trimmed()) m_navigationPage->showSearch(query, paths, warning); });
    auto *searchShortcut = new QAction(this); searchShortcut->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_K)); addAction(searchShortcut); connect(searchShortcut, &QAction::triggered, this, [this] { m_sidebarWanted = true; m_sidebarExplicit = true; updateSidebar(); m_search->setFocus(Qt::ShortcutFocusReason); m_search->selectAll(); });
    m_favoriteAction = new QAction(QStringLiteral("添加到收藏"), this); m_favoriteAction->setObjectName("favoriteDocument"); m_favoriteAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D)); addAction(m_favoriteAction);
    connect(m_favoriteAction, &QAction::triggered, this, [this] { if (m_navigationVisible) m_navigationPage->toggleSelectedFavorite(); else if (auto *pane = activePane()) if (pane->document()) m_navigation->setFavorite(pane->document()->path(), !m_navigation->favorite(pane->document()->path())); });
    connect(m_navigation, &NavigationStore::changed, this, [this] { updateNavigationSelection(); if (m_warning->property("noticeSource") == "navigation" && m_navigation->error().isEmpty()) showNotice({}); });
    connect(m_navigation, &NavigationStore::storageFailed, this, [this](const QString &message) { showNotice(message); m_warning->setProperty("noticeSource", "navigation"); }); connect(windowAppearance(), &Appearance::preferenceFailed, this, [this](const QString &message) { showNotice(message); m_warning->setProperty("noticeSource", "appearance"); });
    connect(m_navigationPage->findChild<QListWidget *>("navigationResults"), &QListWidget::currentRowChanged, this, &WorkspaceWindow::updateNavigationSelection);
    connect(InputModality::forApplication(), &InputModality::changed, this, [this] { if (m_search->hasFocus()) applyAppearance(); });
    m_footers = new QStackedWidget(this); m_footers->setObjectName("workspaceFooters"); m_footers->setFixedHeight(tokens.value("statusbarHeight").toInt()); layout->addWidget(m_footers);
    m_emptyFooter = new QWidget(m_footers); m_emptyFooter->setObjectName("emptyWorkspaceFooter"); auto *status = new QHBoxLayout(m_emptyFooter); status->setContentsMargins(16, 0, 16, 0); auto *label = new QLabel(QStringLiteral("请选择文档"), m_emptyFooter); label->setFont(uiFont(12)); status->addWidget(label); status->addStretch(); m_footers->addWidget(m_emptyFooter);
    m_emptyOpen = new QAction(QStringLiteral("打开文件…"), this); m_emptyOpen->setShortcut(QKeySequence::Open); addAction(m_emptyOpen); connect(m_emptyOpen, &QAction::triggered, this, &WorkspaceWindow::chooseFileRequested);
    m_emptyQuit = new QAction(QStringLiteral("退出 xdnote"), this); m_emptyQuit->setShortcut(QKeySequence::Quit); addAction(m_emptyQuit); connect(m_emptyQuit, &QAction::triggered, this, &WorkspaceWindow::quitRequested);
    m_geometry = new WindowGeometry(this, store, "main"); connect(m_geometry, &WindowGeometry::geometryWarning, this, &WorkspaceWindow::showNotice); connect(windowAppearance(), &Appearance::changed, this, [this] { if (m_warning->property("noticeSource") == "appearance") showNotice({}); applyAppearance(); }); applyAppearance(); setWindowTitle(QStringLiteral("xdnote")); updateSidebar(); updateNavigationSelection(); if (!m_navigation->error().isEmpty()) { showNotice(m_navigation->error()); m_warning->setProperty("noticeSource", "navigation"); }
}
void WorkspaceWindow::applyAppearance() {
    auto *appearance = windowAppearance(); auto palette = this->palette(); palette.setColor(QPalette::Window, appearance->color("background")); palette.setColor(QPalette::WindowText, appearance->color("text")); setPalette(palette);
    setStyleSheet(QStringLiteral("QWidget#workspaceWindow{background:%1;color:%2} QWidget#workspaceSidebar{background:%3} QLabel{color:%2;background:transparent} QLabel#workspaceTreeState,QLabel[secondary=\"true\"],QLabel#workspaceSearchHint{color:%5} QStackedWidget#workspaceFooters{border:0;background:%1} QWidget#emptyWorkspaceFooter{border-top:1px solid %4} QLabel#workspaceNotice{background:%3;border-bottom:1px solid %4} QWidget#sidebarSettingsDivider{background:%4} QLineEdit#workspaceSearch{background:transparent;color:%2;border:0;outline:0;padding:0}").arg(appearance->color("background").name(), appearance->color("text").name(), appearance->color("sidebar").name(), appearance->color("border").name(), appearance->color("textSecondary").name()));
    const bool outline = m_searchTab && m_search->hasFocus() && InputModality::forApplication()->keyboardNavigation(); m_searchBox->setProperty("keyboardOutline", outline); m_searchBox->setStyleSheet(QStringLiteral("QWidget#workspaceSearchBox{background:%1;border:%3px solid %2;border-radius:6px}").arg(appearance->color("surface").name(), outline ? appearance->accentColor().name() : appearance->color("border").name()).arg(visualTokens().value("controls").toObject().value(outline ? "focusRingWidth" : "borderWidth").toInt()));
    auto searchPalette = m_search->palette(); searchPalette.setColor(QPalette::PlaceholderText, appearance->color("textSecondary")); m_search->setPalette(searchPalette); m_search->setAttribute(Qt::WA_MacShowFocusRect, false); findChild<QLabel *>("workspaceSearchIcon")->setPixmap(themedIcon("search", appearance->color("textSecondary")).pixmap(16, 16));
    m_theme->setSymbol(appearance->dark() ? "moon" : "sun"); m_theme->setAccessibleName(appearance->dark() ? QStringLiteral("切换到浅色模式") : QStringLiteral("切换到深色模式")); m_theme->setToolTip(m_theme->accessibleName()); applyNativeAppearance();
}
WorkspaceWindow::~WorkspaceWindow() {
    m_tearingDown = true;
    m_searchDelay.stop(); m_tree->cancelSearch(); m_search->removeEventFilter(this);
    // Native hide/focus notifications and child-model teardown can still emit
    // while QWidget's base destructor runs. Disconnect while our members live.
    disconnect(InputModality::forApplication(), nullptr, this, nullptr); disconnect(m_navigation, nullptr, this, nullptr);
    for (auto *child : findChildren<QObject *>()) disconnect(child, nullptr, this, nullptr);
    if (m_geometry) { m_geometry->flush(); delete m_geometry; m_geometry = nullptr; }
    disconnect(m_tabs, nullptr, this, nullptr); disconnect(windowAppearance(), nullptr, this, nullptr);
    const auto list = panes(); m_panes.clear();
    for (auto *pane : list) { disconnect(pane, nullptr, this, nullptr); delete pane; }
}
QString WorkspaceWindow::rootPath() const { return m_tree->rootPath(); }
void WorkspaceWindow::setRootPath(const QString &path) { m_searchDelay.stop(); m_search->clear(); m_tree->setRootPath(path); auto title = QFileInfo(rootPath()).fileName(); if (title.isEmpty()) title = path.isEmpty() ? QStringLiteral("我的文档") : QDir::toNativeSeparators(rootPath()); m_rootLabel->setText(title); m_rootLabel->setAccessibleName(title + QStringLiteral("，选择工作区")); m_rootLabel->setToolTip(path); for (auto *pane : panes()) pane->setWorkspaceRoot(rootPath()); setWindowTitle(path.isEmpty() ? QStringLiteral("xdnote") : title + QStringLiteral(" — xdnote")); }
void WorkspaceWindow::showNotice(const QString &message) { m_warning->setProperty("noticeSource", QVariant()); m_warning->setText(message); m_warning->setVisible(!message.isEmpty()); }
int WorkspaceWindow::indexOf(const QUuid &id) const { for (int i = 0; i < m_tabs->count(); ++i) if (tabId(m_tabs, i) == id) return i; return -1; }
SingleFileWindow *WorkspaceWindow::activePane() const { return m_panes.value(tabId(m_tabs, m_tabs->currentIndex())); }
QList<SingleFileWindow *> WorkspaceWindow::panes() const { QList<SingleFileWindow *> result; for (int i = 0; i < m_tabs->count(); ++i) if (auto *pane = m_panes.value(tabId(m_tabs, i)).data()) result.append(pane); return result; }
bool WorkspaceWindow::addPane(SingleFileWindow *pane) {
    if (!pane || m_closingAll || m_panes.contains(pane->paneId()) || !pane->embedIn(m_pages)) return false;
    const auto id = pane->paneId(); pane->setWorkspaceRoot(rootPath()); m_panes.insert(id, pane); m_pages->addWidget(pane); auto *footer = pane->footer(); m_footers->addWidget(footer); footer->setStyleSheet(pane->styleSheet());
    if (auto *menu = pane->findChild<QMenu *>("documentMenu")) { auto *before = pane->findChild<QAction *>("closeAction"); menu->insertAction(before, m_favoriteAction); }
    const int index = m_tabs->addTab(QString()); m_tabs->setTabData(index, QVariantMap{{"id", id}});
    auto *close = new SharedButton({}, "tabClose", windowAppearance(), m_tabs); close->setSymbol("close"); close->setFixedSize(tabToken("closeHit"), tabToken("closeHit")); close->setAccessibleName(QStringLiteral("关闭文档")); m_tabs->setTabButton(index, QTabBar::RightSide, close); connect(close, &QPushButton::clicked, this, [this, id] { closePane(id); });
    connect(pane, &SingleFileWindow::sidebarRequested, this, [this] { m_sidebarWanted = !m_sidebar->isVisible(); m_sidebarExplicit = true; updateSidebar(); });
    connect(pane, &SingleFileWindow::viewStateChanged, this, [this, id] { refreshPane(id); });
    connect(pane, &SingleFileWindow::closeCancelled, this, [this] { m_closingAll = false; emit closeCancelled(); });
    connect(pane, &QObject::destroyed, this, [this, id] { m_panes.remove(id); const int index = indexOf(id); if (index >= 0) m_tabs->removeTab(index); selectCurrent(); if (m_closingAll) QTimer::singleShot(0, this, &WorkspaceWindow::advanceClose); });
    refreshPane(id); activatePane(id); return true;
}
void WorkspaceWindow::refreshPane(const QUuid &id) {
    auto *pane = m_panes.value(id).data(); const int index = indexOf(id); if (!pane || index < 0) return;
    auto title = QFileInfo(pane->displayPath()).fileName(); if (title.isEmpty()) title = QStringLiteral("打开文档"); const bool dirty = pane->document() && pane->document()->dirty();
    m_tabs->setTabData(index, QVariantMap{{"id", id}, {"title", title}, {"dirty", dirty}}); m_tabs->setTabText(index, title + (dirty ? QStringLiteral("，未保存") : QString())); m_tabs->setTabToolTip(index, pane->displayPath()); if (auto *close = m_tabs->tabButton(index, QTabBar::RightSide)) close->setAccessibleName(QStringLiteral("关闭 %1").arg(title)); m_tabs->update();
    if (pane == activePane() && !m_navigationVisible) { m_tree->selectPath(pane->displayPath()); updateNavigationSelection(); }
}
void WorkspaceWindow::activatePane(const QUuid &id) { const int index = indexOf(id); if (index < 0) return; m_navigationVisible = false; { const QSignalBlocker block(m_search); m_search->clear(); } m_searchDelay.stop(); m_tree->cancelSearch(); m_tabs->setCurrentIndex(index); selectCurrent(); if (isMinimized()) showNormal(); show(); raise(); activateWindow(); }
void WorkspaceWindow::selectCurrent() { auto *pane = activePane(); m_pages->setCurrentWidget(m_navigationVisible ? static_cast<QWidget *>(m_navigationPage) : pane ? static_cast<QWidget *>(pane) : m_welcome); m_footers->setCurrentWidget(pane && !m_navigationVisible ? pane->footer() : m_emptyFooter); m_emptyOpen->setEnabled(!pane || m_navigationVisible); m_emptyQuit->setEnabled(!pane || m_navigationVisible); updateNavigationSelection(); if (pane && !m_navigationVisible) { m_tree->selectPath(pane->displayPath()); pane->activateDocument(); } }
void WorkspaceWindow::showNavigation(bool favorites) { { const QSignalBlocker block(m_search); m_search->clear(); } m_searchDelay.stop(); m_tree->cancelSearch(); m_navigationVisible = true; m_navigationPage->showPage(favorites ? WorkspaceNavigation::Page::Favorites : WorkspaceNavigation::Page::Recent, rootPath()); selectCurrent(); }
void WorkspaceWindow::startSearch() { const auto query = m_search->text().trimmed(); if (!query.isEmpty()) m_tree->searchFiles(query); }
void WorkspaceWindow::updateNavigationSelection() { const bool recent = m_navigationVisible && m_navigationPage->page() == WorkspaceNavigation::Page::Recent, favorites = m_navigationVisible && m_navigationPage->page() == WorkspaceNavigation::Page::Favorites; m_recent->setProperty("navigationSelected", recent); m_favorites->setProperty("navigationSelected", favorites); m_recent->update(); m_favorites->update(); if (!m_favoriteAction) return; const auto *pane = activePane(); const auto path = m_navigationVisible ? m_navigationPage->selectedPath() : pane && pane->document() ? pane->document()->path() : QString(); m_favoriteAction->setText(m_navigation->favorite(path) ? QStringLiteral("取消收藏") : QStringLiteral("添加到收藏")); m_favoriteAction->setEnabled(!path.isEmpty()); }
void WorkspaceWindow::closePane(const QUuid &id) { if (m_closingAll) return; if (auto *pane = m_panes.value(id).data()) { activatePane(id); pane->close(); } }
void WorkspaceWindow::closeEvent(QCloseEvent *event) { if (m_panes.isEmpty()) { m_closingAll = true; event->accept(); return; } event->ignore(); if (!m_closingAll) { m_closingAll = true; QTimer::singleShot(0, this, &WorkspaceWindow::advanceClose); } }
void WorkspaceWindow::advanceClose() { if (!m_closingAll) return; if (m_panes.isEmpty()) { close(); return; } const auto list = panes(); if (!list.isEmpty()) { auto *pane = list.first(); activatePane(pane->paneId()); pane->close(); } }
void WorkspaceWindow::resizeEvent(QResizeEvent *event) { ApplicationWindow::resizeEvent(event); updateSidebar(); }
void WorkspaceWindow::updateSidebar() { if (m_sidebar) m_sidebar->setVisible(m_sidebarWanted && (m_sidebarExplicit || width() >= mainTokens().value("compactBelowWidth").toInt())); }
bool WorkspaceWindow::event(QEvent *event) { const bool result = ApplicationWindow::event(event); if (!m_tearingDown && !m_navigationVisible && event->type() == QEvent::WindowActivate && m_pages) if (auto *pane = activePane()) if (pane->document()) pane->document()->checkExternal(); return result; }
bool WorkspaceWindow::eventFilter(QObject *target, QEvent *event) { if (target == m_search) { if (event->type() == QEvent::FocusIn) { const auto reason = static_cast<QFocusEvent *>(event)->reason(); m_searchTab = reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason; applyAppearance(); } else if (event->type() == QEvent::FocusOut || event->type() == QEvent::MouseButtonPress) { m_searchTab = false; applyAppearance(); } else if (event->type() == QEvent::KeyPress) { const auto key = static_cast<QKeyEvent *>(event)->key(); if (key == Qt::Key_Escape) { m_search->clear(); m_searchTab = false; applyAppearance(); return true; } if (key == Qt::Key_Return || key == Qt::Key_Enter) { m_searchTab = false; applyAppearance(); } } } return ApplicationWindow::eventFilter(target, event); }
}
