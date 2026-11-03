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
#include <functional>

namespace xdnote {
namespace {
constexpr int pathRole = Qt::UserRole, detailRole = Qt::UserRole + 1, timeRole = Qt::UserRole + 2;
int nav(const char *key) { return visualTokens().value("navigation").toObject().value(key).toInt(); }
class Results final : public QListWidget {
public:
    using QListWidget::QListWidget;
    std::function<void(QString)> toggleFavorite;
protected:
    void mousePressEvent(QMouseEvent *event) override {
        auto *item = itemAt(event->position().toPoint()); m_star = event->button() == Qt::LeftButton && item && event->position().x() >= visualItemRect(item).right() - 44;
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
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return {200, nav("resultRowHeight")}; }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
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
    connect(list, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) { emit openRequested(item->data(pathRole).toString()); });
    connect(list, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) { emit openRequested(item->data(pathRole).toString()); });
    list->setContextMenuPolicy(Qt::CustomContextMenu); connect(list, &QWidget::customContextMenuRequested, this, [this](QPoint point) {
        if (point.x() >= 0) if (auto *item = m_list->itemAt(point)) m_list->setCurrentItem(item);
        const auto path = selectedPath(); if (path.isEmpty()) return; SharedMenu menu(m_appearance, m_list); auto *star = menu.addAction(m_store->favorite(path) ? QStringLiteral("取消收藏") : QStringLiteral("添加到收藏")); star->setObjectName("toggleFavorite"); star->setIcon(themedIcon("star", m_appearance->color("text")));
        connect(star, &QAction::triggered, this, [this, path] { m_store->setFavorite(path, !m_store->favorite(path)); }); menu.exec(point.x() < 0 ? m_list->mapToGlobal(m_list->rect().center()) : m_list->viewport()->mapToGlobal(point));
    });
    m_empty = new QLabel(this); m_empty->setObjectName("navigationEmpty"); m_empty->setFont(uiFont(18)); m_empty->setAlignment(Qt::AlignCenter); m_empty->setWordWrap(true); layout->addWidget(m_empty, 1);
    auto *clear = new SharedButton(QStringLiteral("清除搜索"), "clearWorkspaceSearch", appearance, this); clear->setOutlined(true); m_clear = clear; layout->addWidget(clear, 0, Qt::AlignHCenter); connect(clear, &QPushButton::clicked, this, &WorkspaceNavigation::clearSearchRequested);
    connect(store, &NavigationStore::changed, this, &WorkspaceNavigation::refresh); connect(appearance, &Appearance::changed, this, &WorkspaceNavigation::applyAppearance); connect(InputModality::forApplication(), &InputModality::changed, list->viewport(), qOverload<>(&QWidget::update)); applyAppearance(); refresh();
}
void WorkspaceNavigation::applyAppearance() { setStyleSheet(QStringLiteral("QWidget#workspaceNavigation{background:%1} QLabel{background:transparent;color:%2} QLabel#navigationSummary{color:%3} QListWidget{background:%1;border:0;outline:0}").arg(m_appearance->color("background").name(), m_appearance->color("text").name(), m_appearance->color("textSecondary").name()) + themedScrollBars(m_appearance)); m_list->viewport()->update(); }
QString WorkspaceNavigation::selectedPath() const { return m_list->currentItem() ? m_list->currentItem()->data(pathRole).toString() : QString(); }
void WorkspaceNavigation::toggleSelectedFavorite() { const auto path = selectedPath(); if (!path.isEmpty()) m_store->setFavorite(path, !m_store->favorite(path)); }
void WorkspaceNavigation::showPage(Page page, const QString &root) { m_page = page; m_root = root; refresh(); }
void WorkspaceNavigation::showSearch(const QString &query, const QStringList &paths, const QString &warning, bool loading) { m_page = Page::Search; m_query = query; m_searchPaths = paths; m_searchWarning = warning; m_loading = loading; refresh(); }
void WorkspaceNavigation::refresh() {
    const auto selection = selectedPath(); const int scroll = m_list->verticalScrollBar()->value(); m_list->clear();
    m_heading->setText(m_page == Page::Recent ? QStringLiteral("最近打开") : m_page == Page::Favorites ? QStringLiteral("收藏") : QStringLiteral("搜索文档"));
    QStringList paths; const auto recent = m_store->recent();
    if (m_page == Page::Recent) for (const auto &entry : recent) paths.append(entry.path);
    else if (m_page == Page::Favorites) paths = m_store->favorites(); else paths = m_searchPaths;
    for (const auto &path : paths) {
        const QFileInfo file(path); auto *item = new QListWidgetItem(file.fileName(), m_list); item->setData(pathRole, path); item->setToolTip(path);
        auto detail = m_root.isEmpty() ? QStringLiteral("文档") : QDir(m_root).relativeFilePath(file.path());
        if (detail == ".") detail = QStringLiteral("文档"); else if (QDir::isAbsolutePath(detail) || detail == ".." || detail.startsWith("../")) detail = QStringLiteral("工作区外"); else detail = QStringLiteral("文档 / ") + detail.replace('/', QStringLiteral(" / "));
        if (!file.exists()) detail += QStringLiteral(" · 文件已不存在"); item->setData(detailRole, detail);
        for (const auto &entry : recent) if (entry.path == path) { const auto local = entry.opened.toLocalTime(); item->setData(timeRole, local.date() == QDate::currentDate() ? local.toString("HH:mm") : local.toString("MM-dd HH:mm")); break; }
        item->setData(Qt::AccessibleTextRole, item->text() + QStringLiteral("，") + detail + (m_store->favorite(path) ? QStringLiteral("，已收藏") : QString())); if (path == selection) m_list->setCurrentItem(item);
    }
    m_list->verticalScrollBar()->setValue(scroll); const bool empty = paths.isEmpty(); m_list->setVisible(!empty); m_empty->setVisible(empty);
    m_empty->setText(m_page == Page::Recent ? QStringLiteral("还没有最近打开的文档") : m_page == Page::Favorites ? QStringLiteral("还没有收藏的文档\n\n在最近打开或文件菜单中添加收藏。") : m_loading ? QStringLiteral("正在按文件名搜索…") : QStringLiteral("没有找到匹配的文档"));
    m_summary->setText(m_page == Page::Search ? (!m_searchWarning.isEmpty() ? m_searchWarning : QStringLiteral("文件名包含“%1” · %2 项").arg(m_query).arg(paths.size())) : QStringLiteral("%1 篇文档").arg(paths.size())); m_clear->setVisible(m_page == Page::Search && !m_loading);
}
}
