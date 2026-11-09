#include "QuickAccessibleRoot.h"
#include <QAccessibleObject>
#include <QQuickWidget>
#include <QWindow>
#include <QtQml/qqml.h>

namespace xdnote {
namespace {
class AccessibleQuickRoot final : public QAccessibleObject {
public:
    explicit AccessibleQuickRoot(QuickAccessibleRoot *root) : QAccessibleObject(root) {}
    QuickAccessibleRoot *root() const { return qobject_cast<QuickAccessibleRoot *>(object()); }
    QQuickWidget *host() const { auto *item = root(); return item ? qobject_cast<QQuickWidget *>(item->accessibilityHost()) : nullptr; }
    bool isValid() const override { auto *widget = host(); return QAccessibleObject::isValid() && widget && widget->rootObject() == root(); }
    QWindow *window() const override { auto *widget = host(); return widget && widget->window() ? widget->window()->windowHandle() : nullptr; }
    QAccessibleInterface *parent() const override { return isValid() ? QAccessible::queryAccessibleInterface(host()) : nullptr; }
    int childCount() const override { return children().size(); }
    QAccessibleInterface *child(int index) const override { const auto items = children(); return index >= 0 && index < items.size() ? items.at(index) : nullptr; }
    int indexOfChild(const QAccessibleInterface *target) const override {
        if (!target) return -1;
        const auto items = children();
        for (int index = 0; index < items.size(); ++index) if (items.at(index)->object() == target->object()) return index;
        return -1;
    }
    QAccessibleInterface *childAt(int x, int y) const override {
        const auto items = children();
        for (auto it = items.crbegin(); it != items.crend(); ++it) {
            auto *item = *it;
            if (!item->isValid() || item->state().invisible || !item->rect().contains(x, y)) continue;
            if (auto *nested = item->childAt(x, y)) return nested;
            return item;
        }
        return nullptr;
    }
    QAccessibleInterface *focusChild() const override {
        for (auto *item : children()) {
            if (item->state().focused) return item;
            if (auto *nested = item->focusChild()) return nested;
        }
        return nullptr;
    }
    QRect rect() const override { auto *widget = host(); return isValid() ? QRect(widget->mapToGlobal(QPoint()), widget->size()) : QRect(); }
    QString text(QAccessible::Text type) const override { return type == QAccessible::Name ? QStringLiteral("文档操作") : QString(); }
    QAccessible::Role role() const override { return QAccessible::Grouping; }
    QAccessible::State state() const override {
        QAccessible::State result;
        auto *widget = host(); auto *item = root();
        result.invisible = !isValid() || !widget->isVisible() || !item->isVisible();
        result.disabled = !isValid() || !widget->isEnabled();
        return result;
    }
private:
    static void collect(QQuickItem *parent, QList<QAccessibleInterface *> &result) {
        for (auto *item : parent->childItems()) {
            if (auto *accessible = QAccessible::queryAccessibleInterface(item)) result.append(accessible);
            else collect(item, result);
        }
    }
    QList<QAccessibleInterface *> children() const {
        QList<QAccessibleInterface *> result;
        if (isValid()) collect(root(), result);
        return result;
    }
};
QAccessibleInterface *quickRootFactory(const QString &, QObject *object) {
    if (auto *root = qobject_cast<QuickAccessibleRoot *>(object)) return new AccessibleQuickRoot(root);
    return nullptr;
}
}
void registerQuickAccessibleRoot() {
    static const bool registered = [] {
        qmlRegisterType<QuickAccessibleRoot>("Xdnote.Internal", 1, 0, "QuickAccessibleRoot");
        QAccessible::installFactory(&quickRootFactory);
        return true;
    }();
    Q_UNUSED(registered);
}
}
