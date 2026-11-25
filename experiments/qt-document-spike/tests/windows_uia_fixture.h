#pragma once

#include "guarded_quick_accessibility.h"

#include <QCoreApplication>
#include <QQuickWindow>
#include <QThread>

#define NOMINMAX
#include <windows.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <wrl/implements.h>

namespace FixtureUia {

struct Window {
    HWND handle = nullptr;
    QPointer<QQuickWindow> window;
    std::weak_ptr<FixtureEditorLifetime> editor;
    // Borrowed from the bridge. Created, used, and cleared on the GUI thread.
    IRawElementProviderFragment *rootFragment = nullptr;
    IRawElementProviderFragment *editorFragment = nullptr;

    // Only the native handle is used outside the GUI thread.
    template<class Function> HRESULT onGui(Function function) const
    {
        auto *app = QCoreApplication::instance();
        if (!app || QCoreApplication::closingDown())
            return UIA_E_ELEMENTNOTAVAILABLE;
        HRESULT result = UIA_E_ELEMENTNOTAVAILABLE;
        auto dispatch = [&] {
            if (window)
                result = function(*window);
        };
        if (QThread::currentThread() == app->thread())
            dispatch();
        else if (!QMetaObject::invokeMethod(app, dispatch, Qt::BlockingQueuedConnection))
            return UIA_E_ELEMENTNOTAVAILABLE;
        return result;
    }
};

inline BSTR toBstr(const QString &text)
{
    return SysAllocStringLen(reinterpret_cast<const OLECHAR *>(text.utf16()), UINT(text.size()));
}

POINT clientOrigin(HWND window);
QRectF nativeRect(const Window &window, const QRectF &clientRect);
HRESULT makeTextProvider(const std::shared_ptr<Window> &window, IRawElementProviderSimple *editor, IUnknown **result);
}
