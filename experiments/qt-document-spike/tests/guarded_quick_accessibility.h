#pragma once

#include <QAccessible>
#include <QMetaObject>
#include <QPointer>
#include <QtQuick/private/qquicktextedit_p.h>

#include <memory>

class QObject;
class QString;
class QTextDocument;
class GuardedFixtureTextEdit;

enum class FixtureEditResult {
    Applied, Unchanged, ReadOnly, Composing, Busy, Closing,
    InvalidText, NoDocument, UndoUnavailable, WrongThread, Detached
};

struct FixtureEditorLifetime {
    QPointer<GuardedFixtureTextEdit> editor; // Accessed on the GUI thread only.
    std::shared_ptr<QTextDocument> document;
    bool busy = false;
    bool closing = false;
    bool closeScheduled = false;
    bool reporting = false;
};

// A distinct fixture type lets the factory identify it before QML properties
// and Accessible attached properties are initialized.
class GuardedFixtureTextEdit : public QQuickTextEdit
{
    Q_OBJECT
public:
    explicit GuardedFixtureTextEdit(QQuickItem *parent = nullptr);
    ~GuardedFixtureTextEdit() override;
    FixtureEditResult replaceAccessibleText(const QString &value);
    FixtureEditResult replaceOwnedDocument(std::shared_ptr<QTextDocument> document);
    void requestClose();
    std::weak_ptr<FixtureEditorLifetime> lifetime() const { return m_lifetime; }
    QString writeError() const { return m_writeError; }

signals:
    void accessibleWriteFinished(int result, const QString &message);
    void accessibleValueChanged(const QString &before, const QString &after);

private:
    struct NotificationState;
    void bindDocument();
    void notifyDocumentText();
    FixtureEditResult reportWriteResult(FixtureEditResult result);
    void finishTransaction();

    std::shared_ptr<FixtureEditorLifetime> m_lifetime;
    QString m_writeError;
    std::shared_ptr<NotificationState> m_notifications;
    QMetaObject::Connection m_documentChange;
};

// Qt 6.8-only test candidate. The factory is installed explicitly by tests
// and applies only to the separate fixture editor type.
void registerGuardedQuickFixtureType();
QAccessibleInterface *guardedQuickAccessibilityFactory(const QString &key, QObject *object);
bool isGuardedQuickAccessibility(QAccessibleInterface *interface);
