#pragma once
#include <QPointer>
#include <QQuickItem>

namespace xdnote {
// Root of the admission sample's embedded Quick controls. It does not own the
// QWidget host or any document and does not replace the controls' interfaces.
class QuickAccessibleRoot : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QObject *accessibilityHost READ accessibilityHost WRITE setAccessibilityHost)
public:
    using QQuickItem::QQuickItem;
    QObject *accessibilityHost() const { return m_host; }
    void setAccessibilityHost(QObject *host) { m_host = host; }
private:
    QPointer<QObject> m_host;
};

// Call after creating the QQuickWidget and before loading its QML source.
void registerQuickAccessibleRoot();
}
