#pragma once

#include <QAccessible>
#include <QMetaObject>
#include <QtQuick/private/qquicktextedit_p.h>

#include <memory>

class QObject;
class QString;

// A distinct fixture type lets the factory identify it before QML properties
// and Accessible attached properties are initialized.
class GuardedFixtureTextEdit : public QQuickTextEdit
{
    Q_OBJECT
public:
    explicit GuardedFixtureTextEdit(QQuickItem *parent = nullptr);

private:
    struct NotificationState;
    void bindDocument();
    void notifyDocumentText();

    std::shared_ptr<NotificationState> m_notifications;
    QMetaObject::Connection m_documentChange;
};

// Qt 6.8-only test candidate. The factory is installed explicitly by tests
// and applies only to the separate fixture editor type.
void registerGuardedQuickFixtureType();
QAccessibleInterface *guardedQuickAccessibilityFactory(const QString &key, QObject *object);
bool isGuardedQuickAccessibility(QAccessibleInterface *interface);
