#include "WorkspaceWindow.h"
#include "WorkspaceSearch.h"
#include "WorkspaceTree.h"
#include "WorkspaceResourceReader.h"
#include "WorkspaceReferenceController.h"
#include "ResourceMaintenanceController.h"
#include "DocumentOutline.h"
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
#include <QProxyStyle>
#include <QStyleOptionTab>
#include <QApplication>
#include <QElapsedTimer>
#include <QToolButton>
#include <QEasingCurve>
#include <algorithm>

namespace xdnote {
namespace {
QJsonObject mainTokens() { return visualTokens().value("windows").toObject().value("main").toObject(); }
int tabToken(const char *key) { return visualTokens().value("tabs").toObject().value(key).toInt(); }
int tabContentY(int rowHeight, int itemHeight) { const int top = tabToken("topInset"); return top + (rowHeight - top - itemHeight) / 2; }
int navToken(const char *key) { return visualTokens().value("navigation").toObject().value(key).toInt(); }
QUuid tabId(QTabBar *tabs, int index) { return tabs->tabData(index).toMap().value("id").toUuid(); }
class WorkspaceTabStyle final : public QProxyStyle {
public:
    QRect subElementRect(SubElement element, const QStyleOption *option, const QWidget *widget) const override {
        if (element == SE_TabBarTabRightButton) if (const auto *tab = qstyleoption_cast<const QStyleOptionTab *>(option)) {
            // QTabBar places buttons again on selection, scrolling and drag
            // frames, not only on tabLayoutChange. Give every path one rect.
            return {QPoint(tab->rect.right() + 1 - tabToken("closeInset") - tab->rightButtonSize.width(),
                           tab->rect.top() + tabContentY(tab->rect.height(), tab->rightButtonSize.height())), tab->rightButtonSize};
        }
        return QProxyStyle::subElementRect(element, option, widget);
    }
};
class TabDragSurface final : public QWidget {
public:
    TabDragSurface(QWidget*p):QWidget(p){setAttribute(Qt::WA_TransparentForMouseEvents);setFocusPolicy(Qt::NoFocus);hide();}
    std::function<void(QPainter&)> draw;
protected:
    void paintEvent(QPaintEvent*)override{QPainter p(this);p.setRenderHint(QPainter::Antialiasing);if(draw)draw(p);}
};
class WorkspaceTabs final : public QTabBar {
public:
    WorkspaceTabs(Appearance *appearance, QWidget *parent) : QTabBar(parent), m_appearance(appearance) {
        auto *placement = new WorkspaceTabStyle; placement->setParent(this); setStyle(placement);
        setFont(parent->font()); setAttribute(Qt::WA_MacShowFocusRect, false); setMouseTracking(true);
        setObjectName("workspaceTabs"); setAccessibleName(QStringLiteral("文档标签")); setExpanding(false); setMovable(false); setUsesScrollButtons(true); setElideMode(Qt::ElideMiddle); setDrawBase(false); setFixedHeight(mainTokens().value("titlebarHeight").toInt()); setFocusPolicy(Qt::StrongFocus);
        auto refresh = [this] { auto palette = this->palette(); palette.setColor(QPalette::ButtonText, m_appearance->color("text")); palette.setColor(QPalette::WindowText, m_appearance->color("text")); setPalette(palette); setStyleSheet(QStringLiteral("QTabBar#workspaceTabs QToolButton{background:%1;color:%2;border:0;border-bottom:1px solid %4} QTabBar#workspaceTabs QToolButton:hover{background:%3}").arg(m_appearance->color("sidebar").name(), m_appearance->color("text").name(), m_appearance->color("controlHover").name(), m_appearance->color("border").name())); update(); }; connect(appearance, &Appearance::changed, this, refresh); refresh(); connect(InputModality::forApplication(), &InputModality::changed, this, qOverload<>(&QWidget::update));
        m_ghost=new TabDragSurface(this);m_ghost->setObjectName("workspaceTabDragSurface");m_ghost->draw=[this](QPainter&p){const int i=dragIndex();if(i<0)return;paintTab(p,i,QRect(4,0,m_ghost->width()-8,height()),true);};m_animation.setInterval(16);connect(&m_animation,&QTimer::timeout,this,[this]{animate();});
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
        const int moving = dragIndex();
        if ((m_dragging || m_settling) && moving >= 0) {
            painter.setPen(QPen(m_appearance->color("border"),1,Qt::DashLine)); painter.setBrush(m_appearance->accentColor("soft"));
            painter.drawRoundedRect(tabRect(moving).adjusted(2,tabToken("topInset"),-2,-2),6,6);
        }
        for (int i = 0; i < count(); ++i) {
            if ((m_dragging || m_settling) && i == moving) continue;
            auto rect = tabRect(i);rect.translate(qRound(slide(i)),0);if (!rect.intersects(this->rect())) continue;
            paintTab(painter,i,rect,false);
        }
    }
    void paintTab(QPainter &painter,int i,const QRect &rect,bool raised){
            painter.setFont(font());
            const auto data = tabData(i).toMap(); const bool active = i == currentIndex();
            const int border=1;
            const auto surface = QRectF(rect).adjusted(border / 2.0, tabToken("topInset") + border / 2.0, -border / 2.0, 0); const qreal radius = tabToken("cornerRadius");
            if(raised){paintPopupShell(painter,QRectF(rect).adjusted(.5,3.5,-.5,-3.5),m_appearance->color("surface"),m_appearance->color("border"),6,4);}
            else if (active || i == m_hover) {
                QPainterPath outline; outline.moveTo(surface.left(), height()); outline.lineTo(surface.left(), surface.top() + radius); outline.quadTo(surface.left(), surface.top(), surface.left() + radius, surface.top()); outline.lineTo(surface.right() - radius, surface.top()); outline.quadTo(surface.right(), surface.top(), surface.right(), surface.top() + radius); outline.lineTo(surface.right(), height());
                painter.fillPath(outline, m_appearance->color(active ? "background" : "codeSurface"));
                if (active) { painter.setPen(QPen(m_appearance->color("border"), border)); painter.setBrush(Qt::NoBrush); painter.drawPath(outline); }
                else painter.fillRect(QRect(rect.left(), height() - border, rect.width(), border), m_appearance->color("border"));
            }
            if (!active && i + 1 != currentIndex()) { painter.setPen(QPen(m_appearance->color("border"), border)); painter.drawLine(QPointF(rect.right() + .5, tabToken("separatorInset")), QPointF(rect.right() + .5, height() - tabToken("separatorInset"))); }
            const int left = rect.x() + tabToken("padding"); const QRect icon(left, tabContentY(height(), 16), 16, 16);
            themedIcon(data.value("dirty").toBool() ? "status-dirty" : data.value("symbol", "document").toString(), data.value("dirty").toBool() ? m_appearance->semantic("warning") : m_appearance->color("textSecondary")).paint(&painter, icon);
            painter.setPen(m_appearance->color(active ? "text" : "textSecondary")); const QRect textRect = rect.adjusted(tabToken("padding") + 24, tabToken("topInset"), -tabToken("closeHit") - tabToken("closeInset") - 4, 0);
            painter.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, fontMetrics().elidedText(data.value("title").toString(), Qt::ElideMiddle, textRect.width()));
            if(raised)themedIcon("close",m_appearance->color("textSecondary")).paint(&painter,QRect(rect.right()+1-tabToken("closeInset")-tabToken("closeHit")/2-6,tabContentY(height(),12),12,12));
            if (!raised&&active && hasFocus() && InputModality::forApplication()->keyboardNavigation()) { painter.setBrush(Qt::NoBrush); painter.setPen(QPen(m_appearance->accentColor(), 2)); painter.drawRoundedRect(rect.adjusted(3, 3, -3, -3), 5, 5); }
    }
    void keyPressEvent(QKeyEvent *event) override { if(event->key()==Qt::Key_Escape&&(m_dragging||!m_pressId.isNull())){finishDrag(true);event->accept();return;}if (event->key() == Qt::Key_Delete && currentIndex() >= 0) { InputModality::forApplication()->clear(); if (closeRequested) closeRequested(tabId(this, currentIndex())); event->accept(); } else QTabBar::keyPressEvent(event); }
    void mousePressEvent(QMouseEvent*e)override{if(e->button()==Qt::LeftButton){finishDrag(false);const int i=tabAt(e->position().toPoint());if(i>=0){m_pressId=tabId(this,i);m_press=e->position().toPoint();m_grabOffset=m_press.x()-tabRect(i).left();m_original.clear();for(int j=0;j<count();++j)m_original.append(tabId(this,j));}}QTabBar::mousePressEvent(e);}
    void mouseMoveEvent(QMouseEvent *event) override {
        const auto point=event->position().toPoint();m_lastMouse=point;const auto previous=m_hover;m_hover=tabAt(point);if(previous!=m_hover)update();
        if(event->buttons().testFlag(Qt::LeftButton)&&!m_pressId.isNull()){
            if(!m_dragging&&(point-m_press).manhattanLength()>=QApplication::startDragDistance()){m_dragId=m_pressId;m_dragging=true;m_settling=false;grabMouse();setCursor(Qt::ClosedHandCursor);m_ghost->show();m_ghost->raise();m_animation.start();}
            if(m_dragging){const int current=dragIndex();if(current<0){finishDrag(false);return;}m_followX=qBound(0,point.x()-m_grabOffset,qMax(0,width()-tabRect(current).width()));int target=tabAt(point);if(target>=0&&target!=current&&((target>current&&point.x()>tabRect(target).center().x())||(target<current&&point.x()<tabRect(target).center().x()))){QHash<QUuid,qreal> before;for(int j=0;j<count();++j)before.insert(tabId(this,j),tabRect(j).left()+slide(j));moveTab(current,target);m_slides.clear();for(int j=0;j<count();++j)m_slides.insert(tabId(this,j),before.value(tabId(this,j),tabRect(j).left())-tabRect(j).left());m_slideClock.restart();}syncDrag();event->accept();return;}
        }QTabBar::mouseMoveEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent*e)override{if(e->button()==Qt::LeftButton&&(m_dragging||!m_pressId.isNull())){const bool dragged=m_dragging;finishDrag(false);if(dragged){e->accept();return;}}QTabBar::mouseReleaseEvent(e);}
    bool event(QEvent*e)override{if((e->type()==QEvent::WindowDeactivate||e->type()==QEvent::UngrabMouse)&&m_dragging)finishDrag(true);return QTabBar::event(e);}
    void leaveEvent(QEvent *event) override { m_hover = -1; update(); QTabBar::leaveEvent(event); }
    void tabLayoutChange() override { QTabBar::tabLayoutChange(); if (layoutChanged) layoutChanged();if(m_ghost)syncDrag(); }
private:
    int dragIndex()const{for(int i=0;i<count();++i)if(tabId(const_cast<WorkspaceTabs*>(this),i)==m_dragId)return i;return -1;}
    qreal slide(int i)const{if(!m_slideClock.isValid())return 0;const qreal t=qBound(0.0,m_slideClock.elapsed()/160.0,1.0);return m_slides.value(tabId(const_cast<WorkspaceTabs*>(this),i))*(1-QEasingCurve(QEasingCurve::OutCubic).valueForProgress(t));}
    void syncDrag(){const int index=dragIndex();for(int i=0;i<count();++i)if(auto*b=tabButton(i,QTabBar::RightSide)){auto r=tabRect(i);b->move(r.right()+1-tabToken("closeInset")-b->width()+qRound(slide(i)),tabContentY(height(),b->height()));if(i==index&&(m_dragging||m_settling))b->hide();else b->setVisible(r.intersects(rect()));}if(index>=0&&(m_dragging||m_settling)){m_ghost->setGeometry(qRound(m_followX)-4,0,tabRect(index).width()+8,height());m_ghost->raise();m_ghost->update();}update();}
    void finishDrag(bool cancel){const bool active=m_dragging;m_dragging=false;m_pressId={};if(mouseGrabber()==this)releaseMouse();unsetCursor();if(cancel){for(int i=0;i<m_original.size();++i)for(int j=i;j<count();++j)if(tabId(this,j)==m_original[i]){moveTab(j,i);break;}m_slides.clear();m_settling=false;m_ghost->hide();}else if(active&&dragIndex()>=0){m_settleStart=m_followX;m_settling=true;m_settleClock.restart();m_animation.start();}else if(!m_settling){m_ghost->hide();}syncDrag();}
    void animate(){if(m_dragging&&++m_autoScrollTicks%10==0){for(auto*b:findChildren<QToolButton*>())if(b->isVisible()&&((m_lastMouse.x()<20&&b->arrowType()==Qt::LeftArrow)||(m_lastMouse.x()>width()-36&&b->arrowType()==Qt::RightArrow)))b->click();}if(m_settling){const int i=dragIndex();const qreal t=qBound(0.0,m_settleClock.elapsed()/120.0,1.0);if(i<0||t>=1){m_settling=false;m_ghost->hide();}else m_followX=m_settleStart+(tabRect(i).left()-m_settleStart)*QEasingCurve(QEasingCurve::OutCubic).valueForProgress(t);}if(!m_dragging&&!m_settling&&(!m_slideClock.isValid()||m_slideClock.elapsed()>=160)){m_slides.clear();m_animation.stop();}syncDrag();}
    Appearance *m_appearance;
    int m_hover = -1;
    TabDragSurface*m_ghost=nullptr;
    QTimer m_animation;
    QElapsedTimer m_slideClock,m_settleClock;
    QHash<QUuid,qreal>m_slides;
    QVector<QUuid>m_original;
    QUuid m_pressId,m_dragId;
    QPoint m_press,m_lastMouse;
    qreal m_followX=0,m_settleStart=0;
    int m_grabOffset=0,m_autoScrollTicks=0;
    bool m_dragging=false,m_settling=false;
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
        const int tabWidth = qMin(tabs->preferredWidth(), qMax(0, width() - add->width() - 12)); tabs->setGeometry(0, 0, tabWidth, height()); add->move(tabWidth + 4, tabContentY(height(), add->height())); m_arranging = false;
    }
    bool m_arranging = false;
};
}
WorkspaceWindow::WorkspaceWindow(Appearance *appearance, WindowGeometryStore *store, NavigationStore *navigation, FileManager *manager) : ApplicationWindow(appearance), m_navigation(navigation ? navigation : new NavigationStore(this)) {
    m_references = new WorkspaceReferenceController(manager, this);
    m_maintenance = new ResourceMaintenanceController(manager, this);
    setObjectName("workspaceWindow"); setAttribute(Qt::WA_DeleteOnClose); const auto tokens = mainTokens(); resize(tokens.value("defaultWidth").toInt(), tokens.value("defaultHeight").toInt()); setMinimumSize(tokens.value("minWidth").toInt(), tokens.value("minHeight").toInt()); setFont(uiFont(visualTokens().value("typography").toObject().value("ui").toObject().value("size").toInt()));
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
    m_header = new WindowTitleBar(windowAppearance(), this); setTitleBar(m_header); auto *tabStrip = new WorkspaceTabStrip(windowAppearance(), m_header); auto *tabs = tabStrip->tabs; m_tabs = tabs; connect(tabStrip->add, &QPushButton::clicked, this, &WorkspaceWindow::chooseFileRequested); m_header->setCenter(tabStrip, tokens.value("sidebarWidth").toInt() - visualTokens()["controls"].toObject()["borderWidth"].toInt()); layout->addWidget(m_header);
    tabs->closeRequested = [this](QUuid id) { closePane(id); }; connect(tabs, &QTabBar::currentChanged, this, [this] { m_navigationVisible = false; if (m_search) { const QSignalBlocker block(m_search); m_search->clear(); m_searchDelay.stop(); m_tree->cancelSearch(); } selectCurrent(); }); connect(tabs, &QTabBar::tabBarClicked, this, [this](int index) { if (index >= 0) activatePane(tabId(m_tabs, index)); });
    m_warning = new QLabel(this); m_warning->setObjectName("workspaceNotice"); m_warning->setWordWrap(true); m_warning->setTextFormat(Qt::PlainText); m_warning->setContentsMargins(16, 8, 16, 8); m_warning->hide(); layout->addWidget(m_warning);
    auto *content = new QWidget(this); auto *row = new QHBoxLayout(content); row->setContentsMargins(0, 0, 0, 0); row->setSpacing(0); layout->addWidget(content, 1);
    m_sidebar = new QWidget(content); m_sidebar->setObjectName("workspaceSidebar"); m_sidebar->setFixedWidth(tokens.value("sidebarWidth").toInt()); auto *sidebarRow = new QHBoxLayout(m_sidebar); sidebarRow->setContentsMargins(0, 0, 0, 0); sidebarRow->setSpacing(0);m_sidebarViews=new QStackedWidget(m_sidebar);sidebarRow->addWidget(m_sidebarViews,1);auto *navigationColumn = new QWidget(m_sidebarViews);m_sidebarViews->addWidget(navigationColumn);m_outline=new DocumentOutline(windowAppearance(),m_sidebarViews);m_sidebarViews->addWidget(m_outline);connect(m_outline,&DocumentOutline::closeRequested,this,[this]{m_outlineWanted=false;m_sidebarViews->setCurrentIndex(0);});connect(m_outline,&DocumentOutline::headingActivated,this,[this](const QString&id,const QUuid&document,quint64 revision){auto*p=activePane();if(!p||!p->document())return;const auto snapshot=p->document()->session()->snapshot();if(snapshot.documentId==document&&snapshot.revision==revision)p->navigateToHeading(id);});auto *sidebarBorder = new SharedSeparator(Qt::Vertical, windowAppearance(), m_sidebar); sidebarBorder->setObjectName("workspaceSidebarBorder"); sidebarRow->addWidget(sidebarBorder); auto *sidebar = new QVBoxLayout(navigationColumn); sidebar->setContentsMargins(0, 0, 0, 0); sidebar->setSpacing(0); row->addWidget(m_sidebar);
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
    auto *settings = new SharedButton(QStringLiteral("设置"), "workspaceSettings", windowAppearance(), bottom); settings->setProperty("navigationRow", true); settings->setSymbol("settings"); settings->setToolTip(QStringLiteral("打开设置")); connect(settings, &QPushButton::clicked, this, &WorkspaceWindow::settingsRequested); bottomLayout->addWidget(settings, 1);
    auto *divider = new QWidget(bottom); divider->setObjectName("sidebarSettingsDivider"); divider->setFixedSize(1, 20); bottomLayout->addWidget(divider); m_theme = new SharedButton({}, "workspaceTheme", windowAppearance(), bottom); m_theme->setFixedWidth(32); bottomLayout->addWidget(m_theme); sidebar->addWidget(bottom);
    connect(m_theme, &QPushButton::clicked, this, [this] { windowAppearance()->setPreferences(windowAppearance()->dark() ? "light" : "dark", windowAppearance()->accent()); });
    setTabOrder(m_rootLabel, m_search); setTabOrder(m_search, m_recent); setTabOrder(m_recent, m_favorites); setTabOrder(m_favorites, m_tree); setTabOrder(m_tree, settings); setTabOrder(settings, m_theme);
    m_welcome = new QWidget(m_pages); auto *welcome = new QVBoxLayout(m_welcome); welcome->setSpacing(16); welcome->addStretch(); auto *heading = new QLabel(QStringLiteral("开始你的写作"), m_welcome); heading->setFont(uiFont(24)); welcome->addWidget(heading, 0, Qt::AlignHCenter);
    auto *description = new QLabel(QStringLiteral("打开一篇文档，或选择一个工作区目录。"), m_welcome); welcome->addWidget(description, 0, Qt::AlignHCenter);
    auto *openFile = new SharedButton(QStringLiteral("打开文件…"), "workspaceOpenFile", windowAppearance(), m_welcome); openFile->setSymbol("document"); openFile->setProperty("primary", true); welcome->addWidget(openFile, 0, Qt::AlignHCenter); connect(openFile, &QPushButton::clicked, this, &WorkspaceWindow::chooseFileRequested);
    auto *openDirectory = new SharedButton(QStringLiteral("打开工作区…"), "workspaceOpenDirectory", windowAppearance(), m_welcome); openDirectory->setSymbol("folder-open"); openDirectory->setOutlined(true); welcome->addWidget(openDirectory, 0, Qt::AlignHCenter); connect(openDirectory, &QPushButton::clicked, this, &WorkspaceWindow::chooseDirectoryRequested); welcome->addStretch(); m_pages->addWidget(m_welcome);
    m_navigationPage = new WorkspaceNavigation(windowAppearance(), m_navigation, m_pages); m_pages->addWidget(m_navigationPage); connect(m_navigationPage, &WorkspaceNavigation::openRequested, this, &WorkspaceWindow::openRequested); connect(m_navigationPage, &WorkspaceNavigation::clearSearchRequested, m_search, &QLineEdit::clear);
    m_fullSearch=new WorkspaceSearch(windowAppearance(),manager,m_sidebarViews);m_sidebarViews->addWidget(m_fullSearch);connect(m_fullSearch,&WorkspaceSearch::backRequested,this,[this]{m_sidebarViews->setCurrentIndex(0);});connect(m_fullSearch,&WorkspaceSearch::locationRequested,this,&WorkspaceWindow::referenceSourceRequested);
    auto *fullSearch=new SharedButton(QStringLiteral("全文搜索"),"workspaceFullSearchButton",windowAppearance(),m_sidebar);fullSearch->setSymbol("search");fullSearch->setOutlined(true);sidebar->insertWidget(3,fullSearch);auto openFullSearch=[this]{setFocusMode(false);m_sidebarWanted=true;m_sidebarExplicit=true;updateSidebar();m_sidebarViews->setCurrentWidget(m_fullSearch);m_fullSearch->focusQuery();};connect(fullSearch,&QPushButton::clicked,this,openFullSearch);auto *fullSearchAction=new QAction(this);fullSearchAction->setObjectName("fullSearchAction");fullSearchAction->setShortcut(QKeySequence(Qt::CTRL|Qt::SHIFT|Qt::Key_F));addAction(fullSearchAction);connect(fullSearchAction,&QAction::triggered,this,openFullSearch);
    m_searchDelay.setSingleShot(true); m_searchDelay.setInterval(navToken("searchDelayMs")); connect(&m_searchDelay, &QTimer::timeout, this, &WorkspaceWindow::startSearch);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &text) { m_searchDelay.stop(); m_tree->cancelSearch(); if (text.trimmed().isEmpty()) { m_navigationVisible = false; selectCurrent(); } else { m_navigationVisible = true; m_navigationPage->showPage(WorkspaceNavigation::Page::Search, rootPath()); m_navigationPage->showSearch(text.trimmed(), {}, {}, true); selectCurrent(); m_searchDelay.start(); } });
    connect(m_tree, &WorkspaceTree::searchFinished, this, [this](const QString &query, const QStringList &paths, const QString &warning) { if (m_navigationVisible && query == m_search->text().trimmed()) m_navigationPage->showSearch(query, paths, warning); });
    auto *searchShortcut = new QAction(this); searchShortcut->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_K)); addAction(searchShortcut); connect(searchShortcut, &QAction::triggered, this, [this] { setFocusMode(false); m_outlineWanted = false; m_sidebarViews->setCurrentIndex(0); m_sidebarWanted = true; m_sidebarExplicit = true; updateSidebar(); m_search->setFocus(Qt::ShortcutFocusReason); m_search->selectAll(); });
    auto*focusShortcut=new QAction(this);focusShortcut->setObjectName("workspaceFocusAction");focusShortcut->setShortcut(QKeySequence(Qt::Key_F11));addAction(focusShortcut);connect(focusShortcut,&QAction::triggered,this,[this]{setFocusMode(!m_focusMode);});
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
    delete m_maintenance; m_maintenance = nullptr;
    delete m_references; m_references = nullptr;
    if (m_resourceRoot) m_resourceRoot->revoke();
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
void WorkspaceWindow::setRootPath(const QString &path) {
    if (m_maintenance->busy()) { showNotice(QStringLiteral("正在维护资源，请等待完成后切换工作区。")); return; }
    m_searchDelay.stop(); m_search->clear();
    if (m_resourceRoot) m_resourceRoot->revoke();
    m_tree->setRootPath(path);
    m_resourceRoot = path.isEmpty() ? nullptr : WorkspaceResourceReader::open(m_tree->rootPath()).reader;
    m_references->setRoot(m_resourceRoot, m_tree->rootPath());
    m_fullSearch->setRoot(m_resourceRoot);
    for (auto *pane : panes()) {
        pane->setWorkspaceRoot(rootPath());
        pane->setWorkspaceResourceRoot(m_resourceRoot, rootPath());
    }
    auto title = QFileInfo(rootPath()).fileName();
    if (title.isEmpty()) title = path.isEmpty() ? QStringLiteral("我的文档") : QDir::toNativeSeparators(rootPath());
    m_rootLabel->setText(title); m_rootLabel->setAccessibleName(title + QStringLiteral("，选择工作区"));
    m_rootLabel->setToolTip(path); setWindowTitle(path.isEmpty() ? QStringLiteral("xdnote") : title + QStringLiteral(" — xdnote"));
}
void WorkspaceWindow::showNotice(const QString &message) { m_warning->setProperty("noticeSource", QVariant()); m_warning->setText(message); m_warning->setVisible(!message.isEmpty()); }
int WorkspaceWindow::indexOf(const QUuid &id) const { for (int i = 0; i < m_tabs->count(); ++i) if (tabId(m_tabs, i) == id) return i; return -1; }
SingleFileWindow *WorkspaceWindow::activePane() const { return m_panes.value(tabId(m_tabs, m_tabs->currentIndex())); }
QList<SingleFileWindow *> WorkspaceWindow::panes() const { QList<SingleFileWindow *> result; for (int i = 0; i < m_tabs->count(); ++i) if (auto *pane = m_panes.value(tabId(m_tabs, i)).data()) result.append(pane); return result; }
bool WorkspaceWindow::addPane(SingleFileWindow *pane) {
    if (!pane || m_closingAll || m_panes.contains(pane->paneId()) || !pane->embedIn(m_pages)) return false;
    pane->setWorkspaceRoot(rootPath());
    pane->setWorkspaceResourceRoot(m_resourceRoot, rootPath());
    connect(pane, &SingleFileWindow::referencesRequested, this, [this, pane](QMenu *menu) { populateReferences(menu, pane); });
    const auto id = pane->paneId(); m_panes.insert(id, pane); m_pages->addWidget(pane); auto *footer = pane->footer(); m_footers->addWidget(footer); footer->setStyleSheet(pane->styleSheet());
    if (auto *menu = pane->findChild<QMenu *>("documentMenu")) { auto *before = pane->findChild<QAction *>("closeAction"); menu->insertAction(before, m_favoriteAction); }
    const int index = m_tabs->addTab(QString()); m_tabs->setTabData(index, QVariantMap{{"id", id}});
    installTabClose(index, id);
    connect(pane, &SingleFileWindow::sidebarRequested, this, [this] { m_sidebarWanted = !m_sidebar->isVisible(); m_sidebarExplicit = true; updateSidebar(); });
    connect(pane,&SingleFileWindow::outlineRequested,this,[this,pane]{if(pane!=activePane())return;m_outlineWanted=!m_outlineWanted;if(m_outlineWanted){m_sidebarWanted=true;m_sidebarExplicit=true;m_outline->setSession(pane->document()?pane->document()->session():nullptr);}m_sidebarViews->setCurrentIndex(m_outlineWanted?1:0);updateSidebar();});connect(pane,&SingleFileWindow::focusModeChanged,this,[this,pane](bool enabled){if(pane==activePane())setFocusMode(enabled);});pane->setFocusMode(m_focusMode);
    connect(pane, &SingleFileWindow::viewStateChanged, this, [this, id] { refreshPane(id); });
    connect(pane, &SingleFileWindow::closeCancelled, this, [this] { m_closingAll = false; emit closeCancelled(); });
    connect(pane, &QObject::destroyed, this, [this, id] { m_panes.remove(id); const int index = indexOf(id); if (index >= 0) m_tabs->removeTab(index); selectCurrent(); if (m_closingAll) QTimer::singleShot(0, this, &WorkspaceWindow::advanceClose); });
    refreshPane(id); activatePane(id); return true;
}
void WorkspaceWindow::populateReferences(QMenu *menu, SingleFileWindow *pane) {
    menu->clear(); qDeleteAll(menu->findChildren<QMenu *>(QString(), Qt::FindDirectChildrenOnly));
    const auto caption = [](QString text) {
        for (auto &ch : text) if (ch.category() == QChar::Other_Control) ch = ' ';
        if (text.size() > 110) text = text.left(107) + QStringLiteral("…");
        return text.replace('&', "&&");
    };
    const auto label = [&](QMenu *target, const QString &text) { auto *action = target->addAction(caption(text)); action->setToolTip(text); action->setEnabled(false); };
    menu->setToolTipsVisible(true);
    auto *move = menu->addAction(QStringLiteral("移动或重命名资源…")); move->setObjectName("resourceMaintenanceAction");
    move->setEnabled(m_resourceRoot && m_resourceRoot->active() && !m_maintenance->busy());
    connect(move, &QAction::triggered, this, [this, pane] { m_maintenance->show(pane->document() ? m_references->relativePath(pane->document()->path()) : QString{}); });
    auto *stopMaintenance = menu->addAction(QStringLiteral("停止后续资源维护")); stopMaintenance->setEnabled(m_maintenance->busy());
    connect(stopMaintenance, &QAction::triggered, m_maintenance, &ResourceMaintenanceController::cancel);
    menu->addSeparator();
    label(menu, m_references->statusText());
    auto *refresh = menu->addAction(QStringLiteral("重新检查工作区")); refresh->setObjectName("refreshReferencesAction");
    refresh->setEnabled(m_resourceRoot && m_resourceRoot->active()); connect(refresh, &QAction::triggered, m_references, &WorkspaceReferenceController::refresh);
    auto *cancel = menu->addAction(QStringLiteral("取消检查")); cancel->setEnabled(m_references->running());
    connect(cancel, &QAction::triggered, m_references, &WorkspaceReferenceController::cancel);
    const auto generation = m_references->generation();
    if (!pane->document() || !m_references->current(generation)) return;
    const auto path = m_references->relativePath(pane->document()->path());
    if (path.isEmpty()) { label(menu, QStringLiteral("当前文档不在此工作区目录内，无法显示它的引用关系。")); return; }
    const auto &result = m_references->result();
    const auto submenu = [&](const QString &title, const QString &name) {
        auto *child = new SharedMenu(windowAppearance(), menu); child->setTitle(title); child->setObjectName(name); child->setToolTipsVisible(true); menu->addMenu(child); return child;
    };
    const auto targetStatus = [](WorkspaceReadError error) {
        switch (error) {
        case WorkspaceReadError::None: return QStringLiteral("目标存在");
        case WorkspaceReadError::Missing: return QStringLiteral("目标不存在");
        case WorkspaceReadError::Denied: return QStringLiteral("没有读取权限");
        case WorkspaceReadError::OutsideOrLink: return QStringLiteral("链接或范围受阻");
        case WorkspaceReadError::Unsupported: return QStringLiteral("当前平台不支持");
        default: return QStringLiteral("未能完成检查");
        }
    };
    auto *outbound = submenu(QStringLiteral("当前文档引用"), "outboundReferencesMenu");
    int shown = 0;
    for (const auto &reference : result.index.outbound(path)) {
        if (++shown > 200) { label(outbound, QStringLiteral("引用较多，仅显示前 200 项")); break; }
        const auto status = reference.target.success ? targetStatus(result.targets.value(reference.target.path).error) : QStringLiteral("目标不在支持范围内");
        const auto target = reference.target.success ? reference.target.path + (reference.target.hasFragment ? '#' + reference.target.fragment : QString()) : reference.reference.target;
        auto *action = outbound->addAction(caption(target + QStringLiteral(" — ") + status));
        action->setToolTip(target); action->setData(reference.reference.target);
        action->setEnabled(reference.reference.kind == MarkdownReference::Kind::Link);
        connect(action, &QAction::triggered, pane, [this, pane, generation, reference] {
            if (!m_references->current(generation)) { pane->reportHostMessage(QStringLiteral("引用已变化，请重新打开引用检查菜单。")); return; }
            pane->activateDocumentLink(reference.reference.target, reference.revision);
        });
    }
    if (!shown) label(outbound, QStringLiteral("当前快照没有识别到引用"));
    auto *inbound = submenu(QStringLiteral("引用当前文档的位置"), "inboundReferencesMenu"); shown = 0;
    for (const auto &reference : result.index.inbound(path)) {
        if (++shown > 200) { label(inbound, QStringLiteral("引用较多，仅显示前 200 项")); break; }
        const auto source = std::find_if(result.index.sources.cbegin(), result.index.sources.cend(), [&](const auto &item) { return item.path == reference.sourcePath; });
        if (source == result.index.sources.cend()) continue;
        auto *action = inbound->addAction(caption(QStringLiteral("%1，第 %2 行").arg(reference.sourcePath).arg(reference.sourceLine)));
        connect(action, &QAction::triggered, this, [this, reference, sha = source->textSha256, generation] {
            if (!m_references->current(generation)) { showNotice(QStringLiteral("引用快照已变化，请重新打开检查菜单。")); return; }
            emit referenceSourceRequested(reference.sourcePath, reference.sourceIdentity, sha, reference.reference.usage.start, reference.reference.usage.end);
        });
    }
    if (!shown) label(inbound, QStringLiteral("当前扫描中没有识别到入站引用"));
    auto *problems = submenu(QStringLiteral("缺失或受阻的目标"), "referenceProblemsMenu"); shown = 0;
    for (const auto &reference : result.index.references) {
        const auto state = result.targets.value(reference.target.path); const auto error = state.error;
        const auto fragmentProblem = result.fragmentProblems.value(referenceDiagnosticKey(reference));
        if (reference.target.success && error == WorkspaceReadError::None && state.diagnostic.isEmpty() && fragmentProblem.isEmpty()) continue;
        if (++shown > 200) { label(problems, QStringLiteral("问题较多，仅显示前 200 项")); break; }
        const auto message = QStringLiteral("%1，第 %2 行：%3 — %4").arg(reference.sourcePath).arg(reference.sourceLine)
            .arg(reference.reference.target.left(80), !fragmentProblem.isEmpty() ? fragmentProblem : !state.diagnostic.isEmpty() ? state.diagnostic : reference.target.success ? targetStatus(error) : QStringLiteral("不支持的目标"));
        auto *action = problems->addAction(caption(message)); action->setToolTip(message);
        const auto source = std::find_if(result.index.sources.cbegin(), result.index.sources.cend(), [&](const auto &item) { return item.path == reference.sourcePath; });
        if (source != result.index.sources.cend()) connect(action, &QAction::triggered, this, [this, reference, sha = source->textSha256, generation] {
            if (!m_references->current(generation)) { showNotice(QStringLiteral("引用已变化，请重新检查后定位源文。")); return; }
            emit referenceSourceRequested(reference.sourcePath, reference.sourceIdentity, sha, reference.reference.usage.start, reference.reference.usage.end);
        });
    }
    if (!shown) label(problems, QStringLiteral("当前扫描中没有发现缺失或受阻目标"));
    auto *candidates = submenu(QStringLiteral("未引用图片候选"), "unreferencedImagesMenu");
    label(candidates, result.index.complete ? QStringLiteral("不自动删除；工作区外仍可能引用这些图片") : QStringLiteral("扫描不完整，候选清单仅供参考；不自动删除"));
    shown = 0;
    for (const auto &image : result.imageCandidates) if (result.index.inbound(image).isEmpty()) {
        if (++shown > 200) { label(candidates, QStringLiteral("候选较多，仅显示前 200 项")); break; }
        label(candidates, image);
    }
    if (!shown) label(candidates, QStringLiteral("当前扫描中没有识别到候选"));
    auto *images = submenu(QStringLiteral("图片位置与引用"), "imageReferencesMenu");
    QSet<QString> imagePaths;
    for (const auto &reference : result.index.references) if (reference.reference.kind == MarkdownReference::Kind::Image && reference.target.success) imagePaths.insert(reference.target.path);
    for (const auto &image : result.imageCandidates) imagePaths.insert(image);
    auto imageList = imagePaths.values(); imageList.sort(); int imageCount = 0;
    for (const auto &image : imageList) {
        if (++imageCount > 32) { label(images, QStringLiteral("仅显示前 32 张图片；可在源文查看其他引用")); break; }
        auto *details = new SharedMenu(windowAppearance(), images); details->setTitle(caption(image)); details->setToolTipsVisible(true); images->addMenu(details);
        const auto state = result.targets.value(image);
        label(details, image);
        if (result.targets.contains(image)) label(details, state.diagnostic.isEmpty() ? targetStatus(state.error) : state.diagnostic);
        int locations = 0;
        for (const auto &reference : result.index.inbound(image)) {
            if (++locations > 12) { label(details, QStringLiteral("引用较多，仅显示前 12 处")); break; }
            label(details, QStringLiteral("%1，第 %2 行").arg(reference.sourcePath).arg(reference.sourceLine));
        }
        if (!locations) label(details, QStringLiteral("当前扫描未识别到引用；不表示可安全删除"));
    }
    if (!imageCount) label(images, QStringLiteral("当前扫描未识别到本地图片"));
    if (!result.index.gaps.isEmpty() || !result.excludedDirectories.isEmpty()) {
        auto *coverage = submenu(QStringLiteral("检查范围与限制"), "referenceCoverageMenu");
        for (const auto &excluded : result.excludedDirectories) label(coverage, QStringLiteral("未扫描版本管理目录：") + excluded);
        for (const auto &gap : result.index.gaps.first(30)) label(coverage, gap);
        if (result.index.gaps.size() > 30) label(coverage, QStringLiteral("另有 %1 项未完成检查").arg(result.index.gaps.size() - 30));
    }
}
void WorkspaceWindow::installTabClose(int index, const QUuid &id) {
    auto *button = new SharedButton({}, "tabClose", windowAppearance(), m_tabs);
    button->setProperty("tabClose", true); button->setSymbol("close"); button->setFixedSize(tabToken("closeHit"), tabToken("closeHit"));
    button->setAccessibleName(QStringLiteral("关闭 %1").arg(m_tabs->tabData(index).toMap().value("title").toString())); button->setToolTip(button->accessibleName());
    m_tabs->setTabButton(index, QTabBar::RightSide, button);
    connect(button, &QPushButton::clicked, this, [this, id] { closePane(id); });
}
void WorkspaceWindow::refreshPane(const QUuid &id) {
    auto *pane = m_panes.value(id).data(); const int index = indexOf(id); if (!pane || index < 0) return;
    auto title = QFileInfo(pane->displayPath()).fileName(); if (title.isEmpty()) title = QStringLiteral("打开文档"); const bool dirty = pane->document() && pane->document()->dirty();
    m_tabs->setTabData(index, QVariantMap{{"id", id}, {"title", title}, {"dirty", dirty}}); m_tabs->setTabText(index, title + (dirty ? QStringLiteral("，未保存") : QString())); m_tabs->setTabToolTip(index, pane->displayPath()); if (auto *close = m_tabs->tabButton(index, QTabBar::RightSide)) { close->setAccessibleName(QStringLiteral("关闭 %1").arg(title)); close->setToolTip(close->accessibleName()); } m_tabs->update();
    if (pane == activePane() && !m_navigationVisible) { m_tree->selectPath(pane->displayPath()); updateNavigationSelection();m_outline->setSession(pane->document()?pane->document()->session():nullptr); }
}
void WorkspaceWindow::activatePane(const QUuid &id) { const int index = indexOf(id); if (index < 0) return; m_navigationVisible = false; { const QSignalBlocker block(m_search); m_search->clear(); } m_searchDelay.stop(); m_tree->cancelSearch(); m_tabs->setCurrentIndex(index); selectCurrent(); if (isMinimized()) showNormal(); show(); raise(); activateWindow(); }
void WorkspaceWindow::selectCurrent() {
    const auto kind = m_tabs->tabData(m_tabs->currentIndex()).toMap().value("navigation").toString();
    const bool searching = m_search && !m_search->text().trimmed().isEmpty();
    m_navigationVisible = !kind.isEmpty() || searching;
    if (!kind.isEmpty() && !searching) m_navigationPage->showPage(kind == "favorites" ? WorkspaceNavigation::Page::Favorites : WorkspaceNavigation::Page::Recent, rootPath());
    auto *pane = activePane(); m_pages->setCurrentWidget(m_navigationVisible ? static_cast<QWidget *>(m_navigationPage) : pane ? static_cast<QWidget *>(pane) : m_welcome); m_footers->setCurrentWidget(pane && !m_navigationVisible ? pane->footer() : m_emptyFooter); m_emptyOpen->setEnabled(!pane || m_navigationVisible); m_emptyQuit->setEnabled(!pane || m_navigationVisible);updateNavigationSelection();if(m_navigationVisible||!pane)setFocusMode(false);m_outline->setSession(pane&&!m_navigationVisible&&pane->document()?pane->document()->session():nullptr);m_sidebarViews->setCurrentIndex(m_outlineWanted&&pane&&!m_navigationVisible?1:0);if (pane && !m_navigationVisible) {m_tree->selectPath(pane->displayPath());pane->setFocusMode(m_focusMode);pane->activateDocument();} }
void WorkspaceWindow::showNavigation(bool favorites) {
    const QString kind = favorites ? "favorites" : "recent";
    int index = -1;
    for (int i = 0; i < m_tabs->count(); ++i) if (m_tabs->tabData(i).toMap().value("navigation") == kind) { index = i; break; }
    if (index < 0) {
        const auto id = QUuid::createUuid(); const auto title = favorites ? QStringLiteral("收藏") : QStringLiteral("最近打开");
        index = m_tabs->addTab(title); m_tabs->setTabData(index, QVariantMap{{"id", id}, {"title", title}, {"navigation", kind}, {"symbol", favorites ? "star" : "clock"}});
        installTabClose(index, id); m_tabs->setTabToolTip(index, title);
    }
    activatePane(tabId(m_tabs, index));
}
void WorkspaceWindow::startSearch() { const auto query = m_search->text().trimmed(); if (!query.isEmpty()) m_tree->searchFiles(query); }
void WorkspaceWindow::updateNavigationSelection() { const bool recent = m_navigationVisible && m_navigationPage->page() == WorkspaceNavigation::Page::Recent, favorites = m_navigationVisible && m_navigationPage->page() == WorkspaceNavigation::Page::Favorites; m_recent->setProperty("navigationSelected", recent); m_favorites->setProperty("navigationSelected", favorites); m_recent->update(); m_favorites->update(); if (!m_favoriteAction) return; const auto *pane = activePane(); const auto path = m_navigationVisible ? m_navigationPage->selectedPath() : pane && pane->document() ? pane->document()->path() : QString(); m_favoriteAction->setIcon(themedIcon("star",windowAppearance()->color("text"))); m_favoriteAction->setText(m_navigation->favorite(path) ? QStringLiteral("取消收藏") : QStringLiteral("添加到收藏")); m_favoriteAction->setEnabled(!path.isEmpty()); }
void WorkspaceWindow::closePane(const QUuid &id) { if (m_closingAll) return;
    const int index = indexOf(id);
    if (index >= 0 && !m_tabs->tabData(index).toMap().value("navigation").toString().isEmpty()) { m_tabs->removeTab(index); selectCurrent(); return; } if (auto *pane = m_panes.value(id).data()) { activatePane(id); pane->close(); } }
void WorkspaceWindow::closeEvent(QCloseEvent *event) { if (m_maintenance->busy()) { event->ignore(); showNotice(QStringLiteral("正在维护资源，请等待逐项结果后再关闭。")); emit closeCancelled(); return; } if (m_panes.isEmpty()) { m_closingAll = true; event->accept(); return; } event->ignore(); if (!m_closingAll) { m_closingAll = true; QTimer::singleShot(0, this, &WorkspaceWindow::advanceClose); } }
void WorkspaceWindow::advanceClose() { if (!m_closingAll) return; if (m_panes.isEmpty()) { close(); return; } const auto list = panes(); if (!list.isEmpty()) { auto *pane = list.first(); activatePane(pane->paneId()); pane->close(); } }
void WorkspaceWindow::resizeEvent(QResizeEvent *event) { ApplicationWindow::resizeEvent(event); updateSidebar(); }
void WorkspaceWindow::updateSidebar() { if (m_sidebar) m_sidebar->setVisible(!m_focusMode&&m_sidebarWanted && (m_sidebarExplicit || width() >= mainTokens().value("compactBelowWidth").toInt())); }
void WorkspaceWindow::setFocusMode(bool enabled){if(enabled&&(!activePane()||m_navigationVisible))return;if(m_focusMode==enabled)return;m_focusMode=enabled;for(auto*p:panes())p->setFocusMode(enabled);updateSidebar();}
bool WorkspaceWindow::event(QEvent *event) { const bool result = ApplicationWindow::event(event); if (!m_tearingDown && !m_navigationVisible && event->type() == QEvent::WindowActivate && m_pages) if (auto *pane = activePane()) if (pane->document()) pane->document()->checkExternal(); return result; }
bool WorkspaceWindow::eventFilter(QObject *target, QEvent *event) { if (target == m_search) { if (event->type() == QEvent::FocusIn) { const auto reason = static_cast<QFocusEvent *>(event)->reason(); m_searchTab = reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason; applyAppearance(); } else if (event->type() == QEvent::FocusOut || event->type() == QEvent::MouseButtonPress) { m_searchTab = false; applyAppearance(); } else if (event->type() == QEvent::KeyPress) { const auto key = static_cast<QKeyEvent *>(event)->key(); if (key == Qt::Key_Escape) { m_search->clear(); m_searchTab = false; applyAppearance(); return true; } if (key == Qt::Key_Return || key == Qt::Key_Enter) { m_searchTab = false; applyAppearance(); } } } return ApplicationWindow::eventFilter(target, event); }
}
