#include "WorkspaceNavigation.h"
#include "NavigationStore.h"
#include "Appearance.h"
#include "SharedWidgets.h"
#include "VisualTokens.h"
#include "UiFonts.h"
#include "InputModality.h"
#include <QListWidget>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QMouseEvent>
#include <QVBoxLayout>
#include <QDir>
#include <QFileInfo>
#include <QScrollBar>
#include <QScopedValueRollback>
#include <functional>

namespace xdnote {
namespace {
constexpr int pathRole = Qt::UserRole, detailRole = Qt::UserRole + 1, timeRole = Qt::UserRole + 2, sectionRole = Qt::UserRole + 3;
int nav(const char *key) { return visualTokens().value("navigation").toObject().value(key).toInt(); }
class Results final : public QListWidget {
public:
    using QListWidget::QListWidget;
    std::function<void(QString)> toggleFavorite;
protected:
    void mousePressEvent(QMouseEvent *event) override {
        auto *item = itemAt(event->position().toPoint()); m_star = event->button() == Qt::LeftButton && item && !item->data(pathRole).toString().isEmpty() && event->position().x() >= visualItemRect(item).right() - 44;
        if (m_star) { setCurrentItem(item); event->accept(); } else QListWidget::mousePressEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (m_star && event->button() == Qt::LeftButton) { m_star = false; auto *item = itemAt(event->position().toPoint()); if (item && event->position().x() >= visualItemRect(item).right() - 44 && toggleFavorite) toggleFavorite(item->data(pathRole).toString()); event->accept(); }
        else QListWidget::mouseReleaseEvent(event);
    }
    bool m_star = false;
};
class ResultDelegate final : public QStyledItemDelegate {
public:
    ResultDelegate(Appearance *appearance, NavigationStore *store, QObject *parent) : QStyledItemDelegate(parent), appearance(appearance), store(store) {}
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const override { return {200, nav(index.data(sectionRole).toBool() ? "treeRowHeight" : "resultRowHeight")}; }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        if (index.data(sectionRole).toBool()) { painter->save(); painter->setFont(uiFont(12)); painter->setPen(appearance->color("textSecondary")); painter->drawText(option.rect, Qt::AlignVCenter | Qt::AlignLeft, index.data().toString()); painter->restore(); return; }
        painter->save(); const auto rect = option.rect.adjusted(0, 2, 0, -2); const bool selected = option.state & QStyle::State_Selected;
        painter->setRenderHint(QPainter::Antialiasing); painter->setPen(Qt::NoPen); painter->setBrush(selected || option.state & QStyle::State_MouseOver ? appearance->accentColor("soft") : appearance->color("background")); painter->drawRoundedRect(rect, 6, 6);
        themedIcon("document", appearance->color("text")).paint(painter, QRect(rect.left() + 16, rect.center().y() - 12, 24, 24));
        const int left = rect.left() + 56, right = rect.right() - 140; const auto font = uiFont(14); painter->setFont(font); painter->setPen(appearance->color("text"));
        painter->drawText(QRect(left, rect.top() + 10, qMax(0, right - left), 22), Qt::AlignVCenter, QFontMetrics(font).elidedText(index.data().toString(), Qt::ElideMiddle, qMax(0, right - left)));
        const auto secondary = uiFont(12); painter->setFont(secondary); painter->setPen(appearance->color("textSecondary"));
        painter->drawText(QRect(left, rect.top() + 34, qMax(0, right - left), 18), Qt::AlignVCenter, QFontMetrics(secondary).elidedText(index.data(detailRole).toString(), Qt::ElideMiddle, qMax(0, right - left)));
        painter->drawText(QRect(rect.right() - 128, rect.top(), 76, rect.height()), Qt::AlignRight | Qt::AlignVCenter, index.data(timeRole).toString());
        themedIcon(store->favorite(index.data(pathRole).toString()) ? "star-filled" : "star", appearance->color("textSecondary")).paint(painter, QRect(rect.right() - 36, rect.center().y() - 9, 18, 18));
        if (selected && option.state & QStyle::State_HasFocus && InputModality::forApplication()->keyboardNavigation()) { painter->setBrush(Qt::NoBrush); painter->setPen(QPen(appearance->accentColor(), 2)); painter->drawRoundedRect(rect.adjusted(2, 2, -2, -2), 5, 5); }
        painter->restore();
    }
    Appearance *appearance; NavigationStore *store;
};
}
WorkspaceNavigation::WorkspaceNavigation(Appearance *appearance, NavigationStore *store, QWidget *parent) : QWidget(parent), m_appearance(appearance), m_store(store) {
    setObjectName("workspaceNavigation"); auto *layout = new QVBoxLayout(this); layout->setContentsMargins(nav("pageInset"), 16, nav("pageInset"), 16); layout->setSpacing(16);
    m_heading = new QLabel(this); m_heading->setObjectName("navigationHeading"); auto titleFont = uiFont(20); titleFont.setWeight(QFont::DemiBold); m_heading->setFont(titleFont); layout->addWidget(m_heading);
    m_summary = new QLabel(this); m_summary->setObjectName("navigationSummary"); m_summary->setFont(uiFont(12)); m_summary->setWordWrap(true); layout->addWidget(m_summary);
    auto *list = new Results(this); m_list = list; list->setObjectName("navigationResults"); list->setAccessibleName(QStringLiteral("文档导航结果；按菜单键可添加或取消收藏")); list->setMouseTracking(true); list->setSelectionMode(QAbstractItemView::SingleSelection); list->setEditTriggers(QAbstractItemView::NoEditTriggers); list->setAttribute(Qt::WA_MacShowFocusRect, false); list->setItemDelegate(new ResultDelegate(appearance, store, list)); layout->addWidget(list, 1);
    list->toggleFavorite = [this](const QString &path) { m_store->setFavorite(path, !m_store->favorite(path)); };
    connect(list, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) { const auto path = item->data(pathRole).toString(); if (!path.isEmpty()) emit openRequested(path); });
    connect(list, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) { const auto path = item->data(pathRole).toString(); if (!path.isEmpty()) emit openRequested(path); });
    list->setContextMenuPolicy(Qt::CustomContextMenu); connect(list, &QWidget::customContextMenuRequested, this, [this](QPoint point) {
        if (point.x() >= 0) if (auto *item = m_list->itemAt(point)) m_list->setCurrentItem(item);
        const auto path = selectedPath(); if (path.isEmpty()) return; SharedMenu menu(m_appearance, m_list); auto *star = menu.addAction(m_store->favorite(path) ? QStringLiteral("取消收藏") : QStringLiteral("添加到收藏")); star->setObjectName("toggleFavorite"); star->setIcon(themedIcon("star", m_appearance->color("text")));
        connect(star, &QAction::triggered, this, [this, path] { m_store->setFavorite(path, !m_store->favorite(path)); }); menu.exec(point.x() < 0 ? m_list->mapToGlobal(m_list->rect().center()) : m_list->viewport()->mapToGlobal(point));
    });
    m_emptyPane = new QWidget(this); auto *emptyLayout = new QVBoxLayout(m_emptyPane); emptyLayout->setContentsMargins(24, 24, 24, 24); emptyLayout->setSpacing(16); emptyLayout->addStretch();
    m_emptyIcon = new QLabel(m_emptyPane); m_emptyIcon->setFixedSize(nav("emptyIconSize"), nav("emptyIconSize")); emptyLayout->addWidget(m_emptyIcon, 0, Qt::AlignHCenter); m_spinner = new SharedSpinner(appearance, m_emptyPane); emptyLayout->addWidget(m_spinner, 0, Qt::AlignHCenter);
    m_empty = new QLabel(m_emptyPane); m_empty->setObjectName("navigationEmpty"); auto emptyFont = uiFont(24); emptyFont.setWeight(QFont::DemiBold); m_empty->setFont(emptyFont); m_empty->setAlignment(Qt::AlignCenter); m_empty->setWordWrap(true); emptyLayout->addWidget(m_empty);
    m_emptyDetails = new QLabel(m_emptyPane); m_emptyDetails->setObjectName("navigationEmptyDetails"); m_emptyDetails->setAlignment(Qt::AlignCenter); m_emptyDetails->setWordWrap(true); emptyLayout->addWidget(m_emptyDetails);
    auto *clear = new SharedButton(QStringLiteral("清除搜索"), "clearWorkspaceSearch", appearance, m_emptyPane); clear->setOutlined(true); m_clear = clear; emptyLayout->addWidget(clear, 0, Qt::AlignHCenter); emptyLayout->addStretch(); layout->addWidget(m_emptyPane, 1); connect(clear, &QPushButton::clicked, this, &WorkspaceNavigation::clearSearchRequested);
    connect(store, &NavigationStore::changed, this, &WorkspaceNavigation::refresh); connect(appearance, &Appearance::changed, this, &WorkspaceNavigation::applyAppearance); connect(InputModality::forApplication(), &InputModality::changed, list->viewport(), qOverload<>(&QWidget::update)); applyAppearance(); refresh();
}
void WorkspaceNavigation::applyAppearance() { setStyleSheet(QStringLiteral("QWidget#workspaceNavigation{background:%1} QLabel{background:transparent;color:%2} QLabel#navigationSummary,QLabel#navigationEmptyDetails{color:%3} QListWidget{background:%1;border:0;outline:0}").arg(m_appearance->color("background").name(), m_appearance->color("text").name(), m_appearance->color("textSecondary").name()) + themedScrollBars(m_appearance)); m_list->viewport()->update(); refresh(); }
QString WorkspaceNavigation::selectedPath() const { return m_list->currentItem() ? m_list->currentItem()->data(pathRole).toString() : QString(); }
void WorkspaceNavigation::toggleSelectedFavorite() { const auto path = selectedPath(); if (!path.isEmpty()) m_store->setFavorite(path, !m_store->favorite(path)); }
void WorkspaceNavigation::showPage(Page page, const QString &root) { m_page = page; m_root = root; refresh(); }
void WorkspaceNavigation::showSearch(const QString &query, const QStringList &paths, const QString &warning, bool loading) { m_page = Page::Search; m_query = query; m_searchPaths = paths; m_searchWarning = warning; m_loading = loading; refresh(); }
void WorkspaceNavigation::refresh() {
    if (m_refreshing) { m_refreshAgain = true; return; }
    QScopedValueRollback<bool> refreshing(m_refreshing, true);
    do { m_refreshAgain = false; refreshContent(); } while (m_refreshAgain);
}
void WorkspaceNavigation::refreshContent() {
    const auto selection = selectedPath(); const int scroll = m_list->verticalScrollBar()->value(); m_list->clear();
    m_heading->setText(m_page == Page::Recent ? QStringLiteral("最近打开") : m_page == Page::Favorites ? QStringLiteral("收藏") : QStringLiteral("搜索文档"));
    QStringList paths; const auto recent = m_store->recent();
    if (m_page == Page::Recent) for (const auto &entry : recent) paths.append(entry.path);
    else if (m_page == Page::Favorites) paths = m_store->favorites(); else paths = m_searchPaths;
    QString previousSection;
    for (const auto &path : paths) {
        if (m_page == Page::Recent) for (const auto &entry : recent) if (entry.path == path) { const auto date = entry.opened.toLocalTime().date(); const auto section = date == QDate::currentDate() ? QStringLiteral("今天") : date == QDate::currentDate().addDays(-1) ? QStringLiteral("昨天") : QStringLiteral("更早"); if (section != previousSection) { auto *heading = new QListWidgetItem(section, m_list); heading->setData(sectionRole, true); heading->setFlags(Qt::NoItemFlags); previousSection = section; } break; }
        const QFileInfo file(path); auto *item = new QListWidgetItem(file.fileName(), m_list); item->setData(pathRole, path); item->setToolTip(path);
        auto detail = m_root.isEmpty() ? QStringLiteral("文档") : QDir(m_root).relativeFilePath(file.path());
        if (detail == ".") detail = QStringLiteral("文档"); else if (QDir::isAbsolutePath(detail) || detail == ".." || detail.startsWith("../")) detail = QStringLiteral("工作区外"); else detail = QStringLiteral("文档 / ") + detail.replace('/', QStringLiteral(" / "));
        if (!file.exists()) detail += QStringLiteral(" · 文件已不存在"); item->setData(detailRole, detail);
        for (const auto &entry : recent) if (entry.path == path) { const auto local = entry.opened.toLocalTime(); item->setData(timeRole, local.date() == QDate::currentDate() ? local.toString("HH:mm") : local.toString("MM-dd HH:mm")); break; }
        item->setData(Qt::AccessibleTextRole, item->text() + QStringLiteral("，") + detail + (m_store->favorite(path) ? QStringLiteral("，已收藏") : QString())); if (path == selection) m_list->setCurrentItem(item);
    }
    m_list->verticalScrollBar()->setValue(scroll); const bool empty = paths.isEmpty(), searching = m_page == Page::Search && m_loading, incomplete = m_page == Page::Search && !m_searchWarning.isEmpty(); m_list->setVisible(!empty); m_emptyPane->setVisible(empty);
    m_empty->setText(m_page == Page::Recent ? QStringLiteral("还没有最近打开的文档") : m_page == Page::Favorites ? QStringLiteral("还没有收藏的文档") : searching ? QStringLiteral("正在按文件名搜索…") : incomplete ? QStringLiteral("搜索未完整完成") : QStringLiteral("没有找到匹配的文档"));
    m_emptyDetails->setText(m_page == Page::Recent ? QStringLiteral("打开一篇 Markdown 文档，稍后可以在这里继续。") : m_page == Page::Favorites ? QStringLiteral("在最近打开或文档菜单中添加收藏。") : searching ? QStringLiteral("正在检查工作区中的 Markdown 文件名。") : incomplete ? m_searchWarning : QStringLiteral("试试其他关键词。")); m_spinner->setVisible(searching); m_spinner->setRunning(searching); m_emptyIcon->setVisible(!searching); m_emptyIcon->setPixmap(themedIcon(m_page == Page::Recent ? "clock" : m_page == Page::Favorites ? "star" : "document-search", m_appearance->color("textSecondary")).pixmap(QSize(nav("emptyIconSize"), nav("emptyIconSize")), devicePixelRatioF()));
    m_summary->setText(m_page == Page::Search ? (!m_searchWarning.isEmpty() ? m_searchWarning : QStringLiteral("文件名包含“%1” · %2 项").arg(m_query).arg(paths.size())) : QStringLiteral("%1 篇文档").arg(paths.size())); m_clear->setVisible(m_page == Page::Search && !m_loading); m_list->viewport()->update();
}
}
