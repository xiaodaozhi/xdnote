#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <memory>

class QQuickWindow;
class GuardedFixtureTextEdit;

// Native provider for the isolated Quick test window and its guarded editor.
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
