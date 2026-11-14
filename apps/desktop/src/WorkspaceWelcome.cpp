#include "WorkspaceWelcome.h"
#include "WorkspaceNavigation.h"
#include "NavigationStore.h"
#include "Appearance.h"
#include "SharedWidgets.h"
#include "UiFonts.h"
#include "VisualTokens.h"
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonObject>
#include <QLabel>
#include <QResizeEvent>
#include <QScrollArea>
#include <QVBoxLayout>

namespace xdnote {
namespace {
int token(const char *key) { return visualTokens()["workspaceWelcome"].toObject()[key].toInt(); }
}
WorkspaceWelcome::WorkspaceWelcome(Appearance *appearance, NavigationStore *navigation, QWidget *parent)
    : QWidget(parent), m_appearance(appearance), m_navigation(navigation) {
    setObjectName("workspaceWelcome"); auto *outer = new QVBoxLayout(this); outer->setContentsMargins(0, 0, 0, 0);
    m_scroll = new QScrollArea(this); m_scroll->setObjectName("welcomeScroll"); m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(true); m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); outer->addWidget(m_scroll);
    auto *canvas = new QWidget(m_scroll); auto *center = new QVBoxLayout(canvas); const int inset = token("pageInset");
    center->setContentsMargins(inset, inset, inset, inset); center->setSpacing(0); center->addStretch(2);
    m_panel = new QWidget(canvas); m_panel->setObjectName("welcomeContent"); auto *content = new QVBoxLayout(m_panel); content->setContentsMargins(0, 0, 0, 0); content->setSpacing(0);
    m_brand = new QLabel(m_panel); m_brand->setObjectName("welcomeBrandMark"); m_brand->setFixedSize(token("brandSize"), token("brandSize")); m_brand->setAlignment(Qt::AlignCenter);
    content->addWidget(m_brand, 0, Qt::AlignHCenter); content->addSpacing(24);
    auto *heading = new QLabel(QStringLiteral("欢迎使用 xdnote"), m_panel); heading->setObjectName("welcomeHeading");
    auto titleFont = uiFont(visualTokens()["typography"].toObject()["headings"].toObject()["window"].toInt()); titleFont.setWeight(QFont::DemiBold); heading->setFont(titleFont); heading->setAlignment(Qt::AlignCenter); heading->setWordWrap(true); content->addWidget(heading); content->addSpacing(8);
    auto *description = new QLabel(QStringLiteral("打开一个文件，开始阅读与写作。"), m_panel); description->setObjectName("welcomeDescription"); description->setFont(uiFont(token("subtitleSize"))); description->setAlignment(Qt::AlignCenter); description->setWordWrap(true); description->setProperty("secondary", true); content->addWidget(description); content->addSpacing(24);
    auto addAction = [&](const QString &label, const char *name, const char *icon) {
        auto *button = new SharedButton(label, name, appearance, m_panel); button->setFixedWidth(token("actionWidth")); if (*icon) button->setSymbol(icon);
        content->addWidget(button, 0, Qt::AlignHCenter); return button;
    };
    auto *open = addAction(QStringLiteral("打开文件"), "workspaceOpenFile", ""); open->setProperty("primary", true); open->setToolTip(QStringLiteral("在独立窗口中打开 Markdown 文件"));
    connect(open, &QPushButton::clicked, this, &WorkspaceWelcome::chooseFileRequested); content->addSpacing(8);
    auto *directory = addAction(QStringLiteral("打开文件夹"), "workspaceOpenDirectory", ""); directory->setOutlined(true); directory->setToolTip(QStringLiteral("打开文件夹作为工作区"));
    connect(directory, &QPushButton::clicked, this, &WorkspaceWelcome::chooseDirectoryRequested); content->addSpacing(8);
    auto *create = addAction(QStringLiteral("新建文档"), "workspaceNewDocument", "document-new"); create->setToolTip(QStringLiteral("在独立窗口新建文档，首次保存时选择位置"));
    connect(create, &QPushButton::clicked, this, &WorkspaceWelcome::newDocumentRequested); content->addSpacing(32);
    content->addWidget(new SharedSeparator(Qt::Horizontal, appearance, m_panel)); content->addSpacing(16);
    auto *recentHeading = new QHBoxLayout; recentHeading->setObjectName("welcomeRecentHeaderRow"); recentHeading->setContentsMargins(0, 0, 0, 0);
    auto *recentLabel = new QLabel(QStringLiteral("最近打开"), m_panel); recentLabel->setObjectName("welcomeRecentHeading"); auto sectionFont = uiFont(14); sectionFont.setWeight(QFont::DemiBold); recentLabel->setFont(sectionFont); recentHeading->addWidget(recentLabel); recentHeading->addStretch();
    auto *all = new SharedButton(QStringLiteral("查看全部"), "welcomeAllRecent", appearance, m_panel); recentHeading->addWidget(all); connect(all, &QPushButton::clicked, this, &WorkspaceWelcome::recentRequested);
    connect(navigation, &NavigationStore::changed, all, [navigation, all] { all->setVisible(!navigation->recent().isEmpty()); }); all->setVisible(!navigation->recent().isEmpty()); content->addLayout(recentHeading); content->addSpacing(8);
    m_recent = new WorkspaceNavigation(appearance, navigation, m_panel, token("recentLimit")); content->addWidget(m_recent); connect(m_recent, &WorkspaceNavigation::openRequested, this, &WorkspaceWelcome::openRequested);
    m_empty = new QLabel(QStringLiteral("暂无最近文件"), m_panel); m_empty->setObjectName("welcomeEmptyRecent"); m_empty->setProperty("secondary", true); m_empty->setFont(uiFont(14)); m_empty->setAlignment(Qt::AlignCenter); m_empty->setContentsMargins(0, 8, 0, 8); content->addWidget(m_empty);
    center->addWidget(m_panel, 0, Qt::AlignHCenter); center->addStretch(3); m_scroll->setWidget(canvas); m_scroll->viewport()->installEventFilter(this);
    connect(appearance, &Appearance::changed, this, &WorkspaceWelcome::applyAppearance); connect(navigation, &NavigationStore::changed, this, &WorkspaceWelcome::refreshRecent); refreshRecent(); applyAppearance();
}
bool WorkspaceWelcome::eventFilter(QObject *object, QEvent *event) {
    if (object == m_scroll->viewport() && event->type() == QEvent::Resize && m_panel)
        m_panel->setFixedWidth(qMin(token("contentWidth"), qMax(0, m_scroll->viewport()->width() - 2 * token("pageInset"))));
    return QWidget::eventFilter(object, event);
}
void WorkspaceWelcome::setRootPath(const QString &path) { m_root = path; refreshRecent(); }
void WorkspaceWelcome::refreshRecent() {
    const bool recent = !m_navigation->recent().isEmpty(); m_recent->setVisible(recent); m_empty->setVisible(!recent); m_recent->showPage(WorkspaceNavigation::Page::Recent, m_root);
    m_panel->setMinimumHeight(m_panel->layout()->minimumSize().height());
    m_scroll->widget()->layout()->activate();
}
void WorkspaceWelcome::applyAppearance() {
    setStyleSheet(QStringLiteral("QWidget#workspaceWelcome,QScrollArea#welcomeScroll,QScrollArea#welcomeScroll>QWidget>QWidget{background:%1;border:0} QLabel{background:transparent;color:%2} QLabel[secondary=\"true\"]{color:%3}").arg(m_appearance->color("background").name(), m_appearance->color("text").name(), m_appearance->color("textSecondary").name()) + themedScrollBars(m_appearance));
    m_brand->setPixmap(QIcon(QStringLiteral(":/xdnote/design/assets/ui-v2/brand/mark-%1.svg").arg(m_appearance->scheme())).pixmap(QSize(token("brandSize"), token("brandSize")), devicePixelRatioF()));
}
}
