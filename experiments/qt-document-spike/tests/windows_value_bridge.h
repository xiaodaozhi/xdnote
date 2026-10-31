#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <memory>

class QQuickWindow;
class GuardedFixtureTextEdit;

// Controlled single-editor native ValuePattern fixture. Qt's other widgets
// are tested separately; this does not claim a complete TextPattern provider.
class FixtureWindowsValueBridge final : public QObject, public QAbstractNativeEventFilter
{
public:
    FixtureWindowsValueBridge(QQuickWindow *window, GuardedFixtureTextEdit *editor);
    ~FixtureWindowsValueBridge() override;
    bool nativeEventFilter(const QByteArray &type, void *message, qintptr *result) override;

private:
    struct Data;
    std::unique_ptr<Data> m_data;
};
