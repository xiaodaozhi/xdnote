#include "WorkspaceTree.h"
#include "Appearance.h"
#include "SharedWidgets.h"
#include "InputModality.h"
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QtConcurrentRun>
#include <filesystem>
#include <algorithm>

namespace xdnote {
namespace {
constexpr int pathRole = Qt::UserRole + 1, loadedRole = Qt::UserRole + 2;
struct Entry { QString path, name; bool directory; };
struct Listing { QList<Entry> entries; QString error; bool missing = false, limited = false; };
bool ordinaryPath(const QString &path, const QString &root) {
    auto current = path;
    for (;;) { const QFileInfo info(current); if (info.isSymLink() || info.isJunction()) return false; if (current == root) return true; const auto parent = info.dir().absolutePath(); if (current == parent) return false; current = parent; }
}
Listing listDirectory(const QString &path, const QString &root) {
    Listing result; std::error_code error;
    if (!ordinaryPath(path, root)) { result.error = QStringLiteral("目录已更改为链接，请刷新文件导航"); return result; }
    auto iterator = std::filesystem::directory_iterator(std::filesystem::u8path(path.toUtf8().constData()), error);
    if (error) { result.missing = !QFileInfo::exists(path); result.error = result.missing ? QStringLiteral("目录已不存在") : QStringLiteral("无法读取目录，请检查权限或设备连接"); return result; }
    int visited = 0;
    for (const auto end = std::filesystem::directory_iterator(); iterator != end; iterator.increment(error)) {
        if (error) break;
        if (++visited > 10000) { result.limited = true; break; }
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
class TreeDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        auto adjusted = option; if (!InputModality::forApplication()->keyboardNavigation()) adjusted.state &= ~QStyle::State_HasFocus;
        QStyledItemDelegate::paint(painter, adjusted, index);
    }
};
}
WorkspaceTree::WorkspaceTree(Appearance *appearance, QWidget *parent) : QTreeView(parent), m_appearance(appearance) {
    setFont(parent->font()); setAttribute(Qt::WA_MacShowFocusRect, false);
    setObjectName("workspaceTree"); setAccessibleName(QStringLiteral("工作区 Markdown 文件")); setModel(&m_model); setHeaderHidden(true); setIndentation(16); setUniformRowHeights(true);
    setEditTriggers(QAbstractItemView::NoEditTriggers); setDragDropMode(QAbstractItemView::NoDragDrop); setSelectionMode(QAbstractItemView::SingleSelection); setItemDelegate(new TreeDelegate(this));
    connect(this, &QTreeView::expanded, this, [this](const QModelIndex &index) { auto *item = m_model.itemFromIndex(index); if (item && !item->data(loadedRole).toBool()) { item->setData(true, loadedRole); fetchDirectory(item->data(pathRole).toString(), item, m_generation); } });
    auto open = [this](const QModelIndex &index) { const auto path = index.data(pathRole).toString(); if (acceptsFile(path)) emit openRequested(path); };
    connect(this, &QTreeView::activated, this, open);
    connect(this, &QTreeView::clicked, this, open);
    connect(&m_watch, &QFileSystemWatcher::directoryChanged, this, [this] { QTimer::singleShot(0, this, &WorkspaceTree::refresh); });
    connect(appearance, &Appearance::changed, this, &WorkspaceTree::applyAppearance); connect(InputModality::forApplication(), &InputModality::changed, viewport(), qOverload<>(&QWidget::update)); applyAppearance();
}
void WorkspaceTree::applyAppearance() {
    setStyleSheet(QStringLiteral("QTreeView{background:%1;color:%2;border:0;outline:0} QTreeView::item{height:32px;padding-right:8px} QTreeView::item:selected{background:%3;color:%4} QTreeView::item:hover{background:%3}").arg(m_appearance->color("sidebar").name(), m_appearance->color("text").name(), m_appearance->accentColor("soft").name(), m_appearance->accentColor("softForeground").name()) + themedScrollBars(m_appearance));
    const auto updateItems = [this](auto &&self, QStandardItem *parent) -> void { for (int i = 0; i < parent->rowCount(); ++i) { auto *item = parent->child(i); const QFileInfo info(item->data(pathRole).toString()); if (!item->data(pathRole).toString().isEmpty()) item->setIcon(themedIcon(info.isDir() ? "folder" : "document", m_appearance->color("textSecondary"))); self(self, item); } };
    updateItems(updateItems, m_model.invisibleRootItem());
}
void WorkspaceTree::setState(State state, const QString &message) { m_state = state; m_message = message; emit stateChanged(); }
void WorkspaceTree::setRootPath(const QString &path) { m_root = path.isEmpty() ? QString() : QDir::cleanPath(QFileInfo(path).absoluteFilePath()); refresh(); }
void WorkspaceTree::refresh() {
    ++m_generation; m_model.clear(); if (!m_watch.directories().isEmpty()) m_watch.removePaths(m_watch.directories());
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
    const QPersistentModelIndex parentIndex = parent ? parent->index() : QModelIndex();
    if (parent) { parent->removeRows(0, parent->rowCount()); auto *loading = new QStandardItem(QStringLiteral("正在读取…")); loading->setEnabled(false); parent->appendRow(loading); }
    auto *watcher = new QFutureWatcher<Listing>(this);
    connect(watcher, &QFutureWatcher<Listing>::finished, this, [this, watcher, path, parentIndex, generation, isRoot = !parent] {
        const auto listing = watcher->result(); watcher->deleteLater(); if (generation != m_generation || (!isRoot && !parentIndex.isValid())) return;
        auto *target = isRoot ? m_model.invisibleRootItem() : m_model.itemFromIndex(parentIndex); target->removeRows(0, target->rowCount());
        if (listing.error.isEmpty()) {
            for (const auto &entry : listing.entries) {
                auto *item = new QStandardItem(themedIcon(entry.directory ? "folder" : "document", m_appearance->color("textSecondary")), entry.name); item->setData(entry.path, pathRole); item->setToolTip(entry.path); item->setEditable(false); target->appendRow(item);
                if (entry.directory) { auto *placeholder = new QStandardItem(QStringLiteral("展开以读取")); placeholder->setEnabled(false); item->appendRow(placeholder); }
            }
            if (m_watch.directories().size() < 256) m_watch.addPath(path);
        }
        if (isRoot) setState(!listing.error.isEmpty() ? (listing.missing ? State::Missing : State::Unreadable) : listing.entries.isEmpty() ? State::Empty : State::Ready, !listing.error.isEmpty() ? listing.error : listing.limited ? QStringLiteral("目录条目过多，仅显示本次读取的前 10000 项，请进入子目录") : listing.entries.isEmpty() ? QStringLiteral("此目录没有 Markdown 文档或子目录") : QString());
        else if (!listing.error.isEmpty() || listing.entries.isEmpty() || listing.limited) { auto *note = new QStandardItem(!listing.error.isEmpty() ? listing.error : listing.limited ? QStringLiteral("目录条目过多，结果未完整读取") : QStringLiteral("没有 Markdown 文档")); note->setEnabled(false); target->appendRow(note); }
    });
    watcher->setFuture(QtConcurrent::run([path, root = m_root] { return listDirectory(path, root); }));
}
}
