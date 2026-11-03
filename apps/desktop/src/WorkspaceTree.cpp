#include "WorkspaceTree.h"
#include "Appearance.h"
#include "SharedWidgets.h"
#include "InputModality.h"
#include "VisualTokens.h"
#include "UiFonts.h"
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QtConcurrentRun>
#include <QPainter>
#include <QMouseEvent>
#include <filesystem>
#include <algorithm>

namespace xdnote {
namespace {
constexpr int pathRole = Qt::UserRole + 1, loadedRole = Qt::UserRole + 2;
struct Entry { QString path, name; bool directory; };
struct Listing { QList<Entry> entries; QString error; bool missing = false, limited = false; int visited = 0; };
bool ordinaryPath(const QString &path, const QString &root) {
    auto current = path;
    for (;;) { const QFileInfo info(current); if (info.isSymLink() || info.isJunction()) return false; if (current == root) return true; const auto parent = info.dir().absolutePath(); if (current == parent) return false; current = parent; }
}
Listing listDirectory(const QString &path, const QString &root) {
    Listing result; std::error_code error;
    if (!ordinaryPath(path, root)) { result.error = QStringLiteral("目录已更改为链接，请刷新文件导航"); return result; }
    auto iterator = std::filesystem::directory_iterator(std::filesystem::u8path(path.toUtf8().constData()), error);
    if (error) { result.missing = !QFileInfo::exists(path); result.error = result.missing ? QStringLiteral("目录已不存在") : QStringLiteral("无法读取目录，请检查权限或设备连接"); return result; }
    for (const auto end = std::filesystem::directory_iterator(); iterator != end; iterator.increment(error)) {
        if (error) break;
        if (++result.visited > 10000) { result.limited = true; break; }
        const auto utf8 = iterator->path().u8string(); const QFileInfo info(QString::fromUtf8(utf8.data(), int(utf8.size())));
        if (info.isSymLink() || info.isJunction()) continue;
        const bool directory = info.isDir(); const auto suffix = info.suffix().toLower();
        if (directory || (info.isFile() && (suffix == "md" || suffix == "markdown"))) result.entries.append({info.absoluteFilePath(), info.fileName(), directory});
    }
    if (error) result.error = QStringLiteral("读取目录中断，请重试");
    if (!ordinaryPath(path, root)) { result.entries.clear(); result.error = QStringLiteral("目录已更改，请刷新文件导航"); }
    std::sort(result.entries.begin(), result.entries.end(), [](const Entry &a, const Entry &b) { return a.directory != b.directory ? a.directory : QString::localeAwareCompare(a.name, b.name) < 0; });
    return result;
}
}
WorkspaceTree::WorkspaceTree(Appearance *appearance, QWidget *parent) : QTreeView(parent), m_appearance(appearance) {
    setFont(parent ? parent->font() : uiFont()); setAttribute(Qt::WA_MacShowFocusRect, false);
    setObjectName("workspaceTree"); setAccessibleName(QStringLiteral("工作区 Markdown 文件")); setModel(&m_model); setHeaderHidden(true); setIndentation(16); setUniformRowHeights(true); setTextElideMode(Qt::ElideMiddle); setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); setMouseTracking(true);
    setEditTriggers(QAbstractItemView::NoEditTriggers); setDragDropMode(QAbstractItemView::NoDragDrop); setSelectionMode(QAbstractItemView::SingleSelection);
    connect(this, &QTreeView::expanded, this, [this](const QModelIndex &index) { auto *item = m_model.itemFromIndex(index); const auto path = index.data(pathRole).toString(); if (!m_expanded.contains(path)) m_expanded.append(path); if (item && !item->data(loadedRole).toBool()) { item->setData(true, loadedRole); fetchDirectory(path, item, m_generation); } });
    connect(this, &QTreeView::collapsed, this, [this](const QModelIndex &index) { m_expanded.removeAll(index.data(pathRole).toString()); });
    auto open = [this](const QModelIndex &index) { const auto path = index.data(pathRole).toString(); if (acceptsFile(path)) emit openRequested(path); };
    connect(this, &QTreeView::activated, this, open);
    connect(this, &QTreeView::clicked, this, open);
    m_refreshDelay.setSingleShot(true); m_refreshDelay.setInterval(40); connect(&m_refreshDelay, &QTimer::timeout, this, &WorkspaceTree::refresh);
    connect(&m_watch, &QFileSystemWatcher::directoryChanged, this, [this] { m_refreshDelay.start(); });
    connect(appearance, &Appearance::changed, this, &WorkspaceTree::applyAppearance); connect(InputModality::forApplication(), &InputModality::changed, viewport(), qOverload<>(&QWidget::update)); applyAppearance();
}
WorkspaceTree::~WorkspaceTree() { m_refreshDelay.stop(); cancelSearch(); }
void WorkspaceTree::drawRow(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    const bool selected = selectionModel()->isSelected(index); const auto cell = visualRect(index);
    painter->save(); painter->fillRect(option.rect, m_appearance->color("sidebar")); painter->setRenderHint(QPainter::Antialiasing); const auto rect = option.rect.adjusted(8, 1, -8, -1);
    if (selected || index == m_hover) { painter->setPen(Qt::NoPen); painter->setBrush(m_appearance->accentColor("soft")); painter->drawRoundedRect(rect, 5, 5); }
    if (selected) { painter->setBrush(m_appearance->accentColor()); painter->drawRoundedRect(QRectF(rect.left(), rect.top() + 5, 3, rect.height() - 10), 1.5, 1.5); }
    if (selected && hasFocus() && currentIndex() == index && InputModality::forApplication()->keyboardNavigation()) { painter->setBrush(Qt::NoBrush); painter->setPen(QPen(m_appearance->accentColor(), 2)); painter->drawRoundedRect(rect.adjusted(2, 2, -2, -2), 4, 4); }
    const auto icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole)); int left = cell.left() + 4;
    if (!icon.isNull()) { icon.paint(painter, QRect(left, cell.center().y() - 8, 16, 16)); left += 24; }
    painter->setFont(font()); painter->setPen(m_appearance->color(index.flags() & Qt::ItemIsEnabled ? "text" : "textSecondary")); const auto textRect = QRect(left, cell.top(), qMax(0, viewport()->width() - left - 12), cell.height()); painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, fontMetrics().elidedText(index.data().toString(), Qt::ElideMiddle, textRect.width()));
    drawBranches(painter, QRect(0, cell.top(), cell.left(), cell.height()), index); painter->restore();
}
void WorkspaceTree::mouseMoveEvent(QMouseEvent *event) { const auto previous = m_hover; m_hover = indexAt(event->position().toPoint()); if (previous != m_hover) viewport()->update(); QTreeView::mouseMoveEvent(event); }
void WorkspaceTree::leaveEvent(QEvent *event) { m_hover = QPersistentModelIndex(); viewport()->update(); QTreeView::leaveEvent(event); }
void WorkspaceTree::drawBranches(QPainter *painter, const QRect &rect, const QModelIndex &index) const {
    if (model()->hasChildren(index)) themedIcon(isExpanded(index) ? "chevron-down" : "chevron-right", m_appearance->color("textSecondary")).paint(painter, QRect(rect.right() - indentation() + 2, rect.center().y() - 6, 12, 12));
}
void WorkspaceTree::applyAppearance() {
    setStyleSheet(QStringLiteral("QTreeView{background:%1;color:%2;border:0;outline:0} QTreeView::item{height:%3px;padding-right:4px;background:transparent} QTreeView::item:selected,QTreeView::item:hover{background:transparent}").arg(m_appearance->color("sidebar").name(), m_appearance->color("text").name()).arg(visualTokens().value("navigation").toObject().value("treeRowHeight").toInt()) + themedScrollBars(m_appearance));
    const auto updateItems = [this](auto &&self, QStandardItem *parent) -> void { for (int i = 0; i < parent->rowCount(); ++i) { auto *item = parent->child(i); const QFileInfo info(item->data(pathRole).toString()); if (!item->data(pathRole).toString().isEmpty()) item->setIcon(themedIcon(info.isDir() ? "folder" : "document", m_appearance->color("textSecondary"))); self(self, item); } };
    updateItems(updateItems, m_model.invisibleRootItem());
}
void WorkspaceTree::setState(State state, const QString &message) { m_state = state; m_message = message; emit stateChanged(); }
void WorkspaceTree::setRootPath(const QString &path) { cancelSearch(); if (!m_watch.directories().isEmpty()) m_watch.removePaths(m_watch.directories()); m_expanded.clear(); m_selectedPath.clear(); m_root = path.isEmpty() ? QString() : QDir::cleanPath(QFileInfo(path).absoluteFilePath()); refresh(); }
void WorkspaceTree::selectPath(const QString &path) { if (path == m_selectedPath && currentIndex().data(pathRole).toString() == path) return; if (path != m_selectedPath) m_selectionRetries.clear(); m_selectedPath = path; restoreSelection(); }
void WorkspaceTree::restoreSelection() {
    if (!acceptsFile(m_selectedPath)) { clearSelection(); setCurrentIndex({}); return; }
    const auto parts = QDir(m_root).relativeFilePath(m_selectedPath).split('/'); auto *parent = m_model.invisibleRootItem(); auto parentPath = m_root;
    for (int part = 0; part < parts.size(); ++part) {
        QStandardItem *match = nullptr;
        for (int row = 0; row < parent->rowCount(); ++row) if (parent->child(row)->text() == parts[part]) { match = parent->child(row); break; }
        if (!match) { if (!m_pendingDirectories.contains(parentPath) && !m_selectionRetries.contains(parentPath)) { m_selectionRetries.insert(parentPath); fetchDirectory(parentPath, parent == m_model.invisibleRootItem() ? nullptr : parent, m_generation); } return; }
        if (part == parts.size() - 1) { setCurrentIndex(match->index()); scrollTo(match->index()); return; }
        expand(match->index()); parent = match; parentPath = match->data(pathRole).toString();
    }
}
void WorkspaceTree::cancelSearch() { if (m_searchCancel) m_searchCancel->store(true); m_searchCancel.reset(); }
void WorkspaceTree::searchFiles(const QString &query) {
    cancelSearch(); auto cancelled = m_searchCancel = std::make_shared<std::atomic_bool>(false);
    struct Search { QStringList paths; QString warning; };
    auto *watcher = new QFutureWatcher<Search>(this); const auto root = m_root;
    connect(watcher, &QFutureWatcher<Search>::finished, this, [this, watcher, query, root, cancelled] {
        const auto result = watcher->result(); watcher->deleteLater(); if (cancelled->load() || root != m_root) return;
        emit searchFinished(query, result.paths, result.warning);
    });
    watcher->setFuture(QtConcurrent::run([root, query, cancelled] {
        Search result;
        if (root.isEmpty()) { result.warning = QStringLiteral("请先选择工作区目录，再按文件名搜索"); return result; }
        QStringList directories{root}; int visited = 0, read = 0;
        while (!directories.isEmpty() && !cancelled->load()) {
            const auto directory = directories.takeLast(); const auto listing = listDirectory(directory, root); visited += listing.visited; ++read;
            if (!listing.error.isEmpty()) result.warning = QStringLiteral("部分目录无法读取，结果可能不完整：") + listing.error;
            if (listing.limited) result.warning = QStringLiteral("目录条目过多，搜索结果不完整");
            for (const auto &entry : listing.entries) {
                if (entry.directory) directories.append(entry.path);
                else if (entry.name.contains(query, Qt::CaseInsensitive)) result.paths.append(entry.path);
                if (result.paths.size() >= 500) break;
            }
            if (visited >= 50000 || read >= 512 || result.paths.size() >= 500) { result.warning = QStringLiteral("达到本次搜索上限，结果可能不完整；请缩小文件名关键词"); break; }
        }
        std::sort(result.paths.begin(), result.paths.end(), [](const auto &a, const auto &b) { return QString::localeAwareCompare(a, b) < 0; }); return result;
    }));
}
void WorkspaceTree::refresh() {
    m_refreshDelay.stop(); ++m_generation; m_pendingDirectories.clear(); m_selectionRetries.clear(); m_model.clear();
    // Keep subscriptions across an asynchronous reread: atomic replacement can
    // otherwise finish while this tree has temporarily stopped watching.
    for (const auto &path : m_watch.directories()) if (!QFileInfo(path).isDir() || !ordinaryPath(path, m_root)) m_watch.removePath(path);
    if (m_root.isEmpty()) { setState(State::Unselected, QStringLiteral("选择目录以浏览 Markdown 文档")); return; }
    const QFileInfo root(m_root);
    if (!root.exists() || !root.isDir()) { setState(State::Missing, QStringLiteral("工作区目录已不存在")); return; }
    if (root.isSymLink() || root.isJunction()) { setState(State::Unreadable, QStringLiteral("请选择普通目录，暂不浏览链接目录")); return; }
    setState(State::Loading, QStringLiteral("正在读取目录…")); fetchDirectory(m_root, nullptr, m_generation);
}
bool WorkspaceTree::acceptsFile(const QString &path) const {
    if (m_root.isEmpty() || path.isEmpty()) return false;
    const auto relative = QDir(m_root).relativeFilePath(QFileInfo(path).absoluteFilePath());
    if (QDir::isAbsolutePath(relative) || relative == ".." || relative.startsWith("../")) return false;
    auto current = m_root; const QFileInfo root(m_root); if (!root.isDir() || root.isSymLink() || root.isJunction()) return false;
    for (const auto &part : relative.split('/')) { current = QDir(current).filePath(part); const QFileInfo info(current); if (info.isSymLink() || info.isJunction()) return false; }
    const QFileInfo file(path); const auto suffix = file.suffix().toLower(); return file.isFile() && (suffix == "md" || suffix == "markdown");
}
void WorkspaceTree::fetchDirectory(const QString &path, QStandardItem *parent, quint64 generation) {
    m_pendingDirectories.insert(path);
    const QPersistentModelIndex parentIndex = parent ? parent->index() : QModelIndex();
    if (parent) { parent->removeRows(0, parent->rowCount()); auto *loading = new QStandardItem(QStringLiteral("正在读取…")); loading->setEnabled(false); parent->appendRow(loading); }
    auto *watcher = new QFutureWatcher<Listing>(this);
    connect(watcher, &QFutureWatcher<Listing>::finished, this, [this, watcher, path, parentIndex, generation, isRoot = !parent] {
        const auto listing = watcher->result(); watcher->deleteLater(); if (generation != m_generation || (!isRoot && !parentIndex.isValid())) return; m_pendingDirectories.remove(path);
        auto *target = isRoot ? m_model.invisibleRootItem() : m_model.itemFromIndex(parentIndex); target->removeRows(0, target->rowCount());
        if (listing.error.isEmpty()) {
            for (const auto &entry : listing.entries) {
                auto *item = new QStandardItem(themedIcon(entry.directory ? "folder" : "document", m_appearance->color("textSecondary")), entry.name); item->setData(entry.path, pathRole); item->setToolTip(entry.path); item->setEditable(false); target->appendRow(item);
                if (entry.directory) { auto *placeholder = new QStandardItem(QStringLiteral("展开以读取")); placeholder->setEnabled(false); item->appendRow(placeholder); }
            }
            if (!m_watch.directories().contains(path) && m_watch.directories().size() < 256) m_watch.addPath(path);
        }
        if (isRoot) setState(!listing.error.isEmpty() ? (listing.missing ? State::Missing : State::Unreadable) : listing.entries.isEmpty() ? State::Empty : State::Ready, !listing.error.isEmpty() ? listing.error : listing.limited ? QStringLiteral("目录条目过多，仅显示本次读取的前 10000 项，请进入子目录") : listing.entries.isEmpty() ? QStringLiteral("此目录没有 Markdown 文档或子目录") : QString());
        else if (!listing.error.isEmpty() || listing.entries.isEmpty() || listing.limited) { auto *note = new QStandardItem(!listing.error.isEmpty() ? listing.error : listing.limited ? QStringLiteral("目录条目过多，结果未完整读取") : QStringLiteral("没有 Markdown 文档")); note->setEnabled(false); target->appendRow(note); }
        for (int row = 0; row < target->rowCount(); ++row) if (m_expanded.contains(target->child(row)->data(pathRole).toString())) expand(target->child(row)->index());
        restoreSelection();
    });
    watcher->setFuture(QtConcurrent::run([path, root = m_root] { return listDirectory(path, root); }));
}
}
