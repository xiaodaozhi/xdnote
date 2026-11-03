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

namespace xdnote {
namespace {
QJsonObject mainTokens() { return visualTokens().value("windows").toObject().value("main").toObject(); }
int tabToken(const char *key) { return visualTokens().value("tabs").toObject().value(key).toInt(); }
QUuid tabId(QTabBar *tabs, int index) { return tabs->tabData(index).toMap().value("id").toUuid(); }
class WorkspaceTabs final : public QTabBar {
public:
    WorkspaceTabs(Appearance *appearance, QWidget *parent) : QTabBar(parent), m_appearance(appearance) {
        setFont(parent->font()); setAttribute(Qt::WA_MacShowFocusRect, false);
        setObjectName("workspaceTabs"); setAccessibleName(QStringLiteral("文档标签")); setExpanding(false); setMovable(true); setUsesScrollButtons(true); setElideMode(Qt::ElideMiddle); setDrawBase(false); setFixedHeight(mainTokens().value("titlebarHeight").toInt()); setFocusPolicy(Qt::StrongFocus);
        connect(appearance, &Appearance::changed, this, [this] { update(); }); connect(InputModality::forApplication(), &InputModality::changed, this, qOverload<>(&QWidget::update));
    }
    std::function<void(QUuid)> closeRequested;
protected:
    QSize tabSizeHint(int index) const override { return {qBound(tabToken("minWidth"), fontMetrics().horizontalAdvance(tabData(index).toMap().value("title").toString()) + 2 * tabToken("padding") + 16 + tabToken("closeHit"), tabToken("maxWidth")), height()}; }
    QSize minimumTabSizeHint(int) const override { return {tabToken("minWidth"), height()}; }
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this); painter.setFont(font()); painter.setRenderHint(QPainter::Antialiasing); painter.fillRect(rect(), m_appearance->color("sidebar"));
        for (int i = 0; i < count(); ++i) {
            const auto rect = tabRect(i); if (!rect.intersects(this->rect())) continue;
            const auto data = tabData(i).toMap(); const bool active = i == currentIndex();
            if (active) { painter.setPen(Qt::NoPen); painter.setBrush(m_appearance->color("background")); painter.drawRoundedRect(rect.adjusted(2, 6, -2, 0), 6, 6); }
            const int left = rect.x() + tabToken("padding"); const QRect icon(left, (height() - 16) / 2, 16, 16);
            themedIcon(data.value("dirty").toBool() ? "status-dirty" : "document", data.value("dirty").toBool() ? m_appearance->semantic("warning") : m_appearance->color("textSecondary")).paint(&painter, icon);
            painter.setPen(m_appearance->color(active ? "text" : "textSecondary")); const QRect textRect = rect.adjusted(tabToken("padding") + 24, 0, -tabToken("closeHit") - 8, 0);
            painter.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, fontMetrics().elidedText(data.value("title").toString(), Qt::ElideMiddle, textRect.width()));
            if (active && hasFocus() && InputModality::forApplication()->keyboardNavigation()) { painter.setBrush(Qt::NoBrush); painter.setPen(QPen(m_appearance->accentColor(), 2)); painter.drawRoundedRect(rect.adjusted(3, 3, -3, -3), 5, 5); }
        }
    }
    void keyPressEvent(QKeyEvent *event) override { if (event->key() == Qt::Key_Delete && currentIndex() >= 0) { InputModality::forApplication()->clear(); if (closeRequested) closeRequested(tabId(this, currentIndex())); event->accept(); } else QTabBar::keyPressEvent(event); }
private:
    Appearance *m_appearance;
};
}
WorkspaceWindow::WorkspaceWindow(Appearance *appearance, WindowGeometryStore *store) : ApplicationWindow(appearance) {
    setObjectName("workspaceWindow"); setAttribute(Qt::WA_DeleteOnClose); const auto tokens = mainTokens(); resize(tokens.value("defaultWidth").toInt(), tokens.value("defaultHeight").toInt()); setMinimumSize(tokens.value("minWidth").toInt(), tokens.value("minHeight").toInt()); setFont(uiFont(visualTokens().value("typography").toObject().value("ui").toObject().value("size").toInt()));
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
    m_header = new WindowTitleBar(windowAppearance(), this); setTitleBar(m_header); auto *tabs = new WorkspaceTabs(windowAppearance(), m_header); m_tabs = tabs; m_header->setCenter(tabs, tokens.value("sidebarWidth").toInt()); layout->addWidget(m_header);
    tabs->closeRequested = [this](QUuid id) { closePane(id); }; connect(tabs, &QTabBar::currentChanged, this, [this] { selectCurrent(); });
    m_warning = new QLabel(this); m_warning->setObjectName("workspaceNotice"); m_warning->setWordWrap(true); m_warning->setTextFormat(Qt::PlainText); m_warning->setContentsMargins(16, 8, 16, 8); m_warning->hide(); layout->addWidget(m_warning);
    auto *content = new QWidget(this); auto *row = new QHBoxLayout(content); row->setContentsMargins(0, 0, 0, 0); row->setSpacing(0); layout->addWidget(content, 1);
    m_sidebar = new QWidget(content); m_sidebar->setObjectName("workspaceSidebar"); m_sidebar->setFixedWidth(tokens.value("sidebarWidth").toInt()); auto *sidebar = new QVBoxLayout(m_sidebar); sidebar->setContentsMargins(0, 0, 0, 0); sidebar->setSpacing(0); row->addWidget(m_sidebar);
    auto *navigation = new QWidget(m_sidebar); navigation->setFixedHeight(tokens.value("toolbarHeight").toInt()); auto *tools = new QHBoxLayout(navigation); tools->setContentsMargins(16, 0, 8, 0); tools->setSpacing(4);
    m_rootLabel = new ElidedLabel(navigation); m_rootLabel->setFullText(QStringLiteral("文件")); tools->addWidget(m_rootLabel, 1);
    auto *directory = new SharedButton({}, "chooseWorkspace", windowAppearance(), navigation); directory->setSymbol("folder-open"); directory->setFixedWidth(32); directory->setAccessibleName(QStringLiteral("打开工作区目录")); directory->setToolTip(QStringLiteral("打开工作区目录")); tools->addWidget(directory); connect(directory, &QPushButton::clicked, this, &WorkspaceWindow::chooseDirectoryRequested);
    auto *refresh = new SharedButton({}, "refreshWorkspace", windowAppearance(), navigation); refresh->setSymbol("refresh"); refresh->setFixedWidth(32); refresh->setAccessibleName(QStringLiteral("刷新文件导航")); tools->addWidget(refresh); sidebar->addWidget(navigation);
    m_treeState = new QLabel(m_sidebar); m_treeState->setObjectName("workspaceTreeState"); m_treeState->setWordWrap(true); m_treeState->setTextFormat(Qt::PlainText); m_treeState->setContentsMargins(16, 8, 16, 8); m_treeState->setFont(uiFont(12)); sidebar->addWidget(m_treeState);
    m_tree = new WorkspaceTree(windowAppearance(), m_sidebar); sidebar->addWidget(m_tree, 1); connect(refresh, &QPushButton::clicked, m_tree, &WorkspaceTree::refresh); connect(m_tree, &WorkspaceTree::openRequested, this, &WorkspaceWindow::openRequested);
    connect(m_tree, &WorkspaceTree::stateChanged, this, [this] { m_treeState->setText(m_tree->message()); m_treeState->setVisible(!m_tree->message().isEmpty()); }); m_tree->setRootPath({});
    m_pages = new QStackedWidget(content); m_pages->setObjectName("workspacePages"); row->addWidget(m_pages, 1);
    m_welcome = new QWidget(m_pages); auto *welcome = new QVBoxLayout(m_welcome); welcome->setSpacing(16); welcome->addStretch(); auto *heading = new QLabel(QStringLiteral("开始你的写作"), m_welcome); heading->setFont(uiFont(24)); welcome->addWidget(heading, 0, Qt::AlignHCenter);
    auto *description = new QLabel(QStringLiteral("打开一篇文档，或选择一个工作区目录。"), m_welcome); welcome->addWidget(description, 0, Qt::AlignHCenter);
    auto *openFile = new SharedButton(QStringLiteral("打开文件…"), "workspaceOpenFile", windowAppearance(), m_welcome); openFile->setSymbol("document"); openFile->setProperty("primary", true); welcome->addWidget(openFile, 0, Qt::AlignHCenter); connect(openFile, &QPushButton::clicked, this, &WorkspaceWindow::chooseFileRequested);
    auto *openDirectory = new SharedButton(QStringLiteral("打开工作区…"), "workspaceOpenDirectory", windowAppearance(), m_welcome); openDirectory->setSymbol("folder-open"); openDirectory->setOutlined(true); welcome->addWidget(openDirectory, 0, Qt::AlignHCenter); connect(openDirectory, &QPushButton::clicked, this, &WorkspaceWindow::chooseDirectoryRequested); welcome->addStretch(); m_pages->addWidget(m_welcome);
    m_footers = new QStackedWidget(this); m_footers->setObjectName("workspaceFooters"); m_footers->setFixedHeight(tokens.value("statusbarHeight").toInt()); layout->addWidget(m_footers);
    m_emptyFooter = new QWidget(m_footers); auto *status = new QHBoxLayout(m_emptyFooter); status->setContentsMargins(16, 0, 16, 0); auto *label = new QLabel(QStringLiteral("请选择文档"), m_emptyFooter); label->setFont(uiFont(12)); status->addWidget(label); status->addStretch(); m_footers->addWidget(m_emptyFooter);
    m_emptyOpen = new QAction(QStringLiteral("打开文件…"), this); m_emptyOpen->setShortcut(QKeySequence::Open); addAction(m_emptyOpen); connect(m_emptyOpen, &QAction::triggered, this, &WorkspaceWindow::chooseFileRequested);
    m_emptyQuit = new QAction(QStringLiteral("退出 xdnote"), this); m_emptyQuit->setShortcut(QKeySequence::Quit); addAction(m_emptyQuit); connect(m_emptyQuit, &QAction::triggered, this, &WorkspaceWindow::quitRequested);
    m_geometry = new WindowGeometry(this, store, "main"); connect(m_geometry, &WindowGeometry::geometryWarning, this, &WorkspaceWindow::showNotice); connect(windowAppearance(), &Appearance::changed, this, &WorkspaceWindow::applyAppearance); applyAppearance(); setWindowTitle(QStringLiteral("xdnote")); updateSidebar();
}
void WorkspaceWindow::applyAppearance() {
    auto *appearance = windowAppearance(); auto palette = this->palette(); palette.setColor(QPalette::Window, appearance->color("background")); palette.setColor(QPalette::WindowText, appearance->color("text")); setPalette(palette);
    setStyleSheet(QStringLiteral("QWidget#workspaceWindow{background:%1;color:%2} QWidget#workspaceSidebar{background:%3;border-right:1px solid %4} QLabel{color:%2;background:transparent} QLabel#workspaceTreeState{color:%5} QStackedWidget#workspaceFooters{border-top:1px solid %4;background:%1} QLabel#workspaceNotice{background:%3;border-bottom:1px solid %4}").arg(appearance->color("background").name(), appearance->color("text").name(), appearance->color("sidebar").name(), appearance->color("border").name(), appearance->color("textSecondary").name())); applyNativeAppearance();
}
WorkspaceWindow::~WorkspaceWindow() { const auto list = panes(); for (auto *pane : list) { disconnect(pane, nullptr, this, nullptr); delete pane; } m_panes.clear(); }
QString WorkspaceWindow::rootPath() const { return m_tree->rootPath(); }
void WorkspaceWindow::setRootPath(const QString &path) { m_tree->setRootPath(path); m_rootLabel->setFullText(path.isEmpty() ? QStringLiteral("文件") : QFileInfo(path).fileName()); m_rootLabel->setToolTip(path); setWindowTitle(path.isEmpty() ? QStringLiteral("xdnote") : QFileInfo(path).fileName() + QStringLiteral(" — xdnote")); }
void WorkspaceWindow::showNotice(const QString &message) { m_warning->setText(message); m_warning->setVisible(!message.isEmpty()); }
int WorkspaceWindow::indexOf(const QUuid &id) const { for (int i = 0; i < m_tabs->count(); ++i) if (tabId(m_tabs, i) == id) return i; return -1; }
SingleFileWindow *WorkspaceWindow::activePane() const { return m_panes.value(tabId(m_tabs, m_tabs->currentIndex())); }
QList<SingleFileWindow *> WorkspaceWindow::panes() const { QList<SingleFileWindow *> result; for (int i = 0; i < m_tabs->count(); ++i) if (auto *pane = m_panes.value(tabId(m_tabs, i)).data()) result.append(pane); return result; }
bool WorkspaceWindow::addPane(SingleFileWindow *pane) {
    if (!pane || m_closingAll || m_panes.contains(pane->paneId()) || !pane->embedIn(m_pages)) return false;
    const auto id = pane->paneId(); m_panes.insert(id, pane); m_pages->addWidget(pane); auto *footer = pane->footer(); m_footers->addWidget(footer); footer->setStyleSheet(pane->styleSheet());
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
}
void WorkspaceWindow::activatePane(const QUuid &id) { const int index = indexOf(id); if (index < 0) return; m_tabs->setCurrentIndex(index); selectCurrent(); if (isMinimized()) showNormal(); show(); raise(); activateWindow(); }
void WorkspaceWindow::selectCurrent() { auto *pane = activePane(); m_pages->setCurrentWidget(pane ? static_cast<QWidget *>(pane) : m_welcome); m_footers->setCurrentWidget(pane ? pane->footer() : m_emptyFooter); m_emptyOpen->setEnabled(!pane); m_emptyQuit->setEnabled(!pane); if (pane) pane->activateDocument(); }
void WorkspaceWindow::closePane(const QUuid &id) { if (m_closingAll) return; if (auto *pane = m_panes.value(id).data()) { activatePane(id); pane->close(); } }
void WorkspaceWindow::closeEvent(QCloseEvent *event) { if (m_panes.isEmpty()) { m_closingAll = true; event->accept(); return; } event->ignore(); if (!m_closingAll) { m_closingAll = true; QTimer::singleShot(0, this, &WorkspaceWindow::advanceClose); } }
void WorkspaceWindow::advanceClose() { if (!m_closingAll) return; if (m_panes.isEmpty()) { close(); return; } const auto list = panes(); if (!list.isEmpty()) { auto *pane = list.first(); activatePane(pane->paneId()); pane->close(); } }
void WorkspaceWindow::resizeEvent(QResizeEvent *event) { ApplicationWindow::resizeEvent(event); updateSidebar(); }
void WorkspaceWindow::updateSidebar() { if (m_sidebar) m_sidebar->setVisible(m_sidebarWanted && (m_sidebarExplicit || width() >= mainTokens().value("compactBelowWidth").toInt())); }
bool WorkspaceWindow::event(QEvent *event) { const bool result = ApplicationWindow::event(event); if (event->type() == QEvent::WindowActivate && m_pages) if (auto *pane = activePane()) pane->activateDocument(); return result; }
}
