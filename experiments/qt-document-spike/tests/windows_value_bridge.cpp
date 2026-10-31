#include "windows_value_bridge.h"
#include "guarded_quick_accessibility.h"

#include <QCoreApplication>
#include <QQuickWindow>
#include <QTextDocument>
#include <QThread>

#define NOMINMAX
#include <windows.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <functional>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;

namespace {
HRESULT editStatus(FixtureEditResult result)
{
    switch (result) {
    case FixtureEditResult::Applied: case FixtureEditResult::Unchanged: return S_OK;
    case FixtureEditResult::InvalidText: return E_INVALIDARG;
    case FixtureEditResult::NoDocument: case FixtureEditResult::Detached:
    case FixtureEditResult::Closing: return UIA_E_ELEMENTNOTAVAILABLE;
    case FixtureEditResult::WrongThread: return E_UNEXPECTED;
    default: return UIA_E_INVALIDOPERATION;
    }
}

class ValueProvider final : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
                                                IRawElementProviderSimple, IValueProvider>
{
public:
    ValueProvider(HWND window, std::weak_ptr<FixtureEditorLifetime> life) : m_window(window), m_life(life) {}

    HRESULT onGui(const std::function<HRESULT(GuardedFixtureTextEdit *)> &function) const
    {
        auto *app = QCoreApplication::instance();
        if (!app || QCoreApplication::closingDown()) return UIA_E_ELEMENTNOTAVAILABLE;
        HRESULT status = UIA_E_ELEMENTNOTAVAILABLE;
        auto call = [weak = m_life, &status, function] {
            auto life = weak.lock();
            if (life && !life->closing && life->editor)
                status = function(life->editor);
        };
        if (QThread::currentThread() == app->thread()) call();
        else if (!QMetaObject::invokeMethod(app, call, Qt::BlockingQueuedConnection))
            return UIA_E_ELEMENTNOTAVAILABLE;
        return status;
    }

    IFACEMETHODIMP get_ProviderOptions(ProviderOptions *value) override
    {
        if (!value) return E_INVALIDARG;
        *value = ProviderOptions_ServerSideProvider;
        return S_OK;
    }
    IFACEMETHODIMP GetPatternProvider(PATTERNID id, IUnknown **value) override
    {
        if (!value) return E_INVALIDARG;
        *value = nullptr;
        if (id == UIA_ValuePatternId)
            return QueryInterface(__uuidof(IValueProvider), reinterpret_cast<void **>(value));
        return S_OK;
    }
    IFACEMETHODIMP GetPropertyValue(PROPERTYID id, VARIANT *value) override
    {
        if (!value) return E_INVALIDARG;
        VariantInit(value);
        return onGui([=](GuardedFixtureTextEdit *editor) -> HRESULT {
            switch (id) {
            case UIA_ControlTypePropertyId: value->vt = VT_I4; value->lVal = UIA_EditControlTypeId; break;
            case UIA_NamePropertyId:
                value->vt = VT_BSTR; value->bstrVal = SysAllocString(L"Document");
                if (!value->bstrVal) return E_OUTOFMEMORY;
                break;
            case UIA_HelpTextPropertyId: {
                const QString message = editor->writeError();
                value->vt = VT_BSTR;
                value->bstrVal = SysAllocStringLen(reinterpret_cast<const OLECHAR *>(message.utf16()), UINT(message.size()));
                if (!value->bstrVal) return E_OUTOFMEMORY;
                break;
            }
            case UIA_IsEnabledPropertyId:
                value->vt = VT_BOOL; value->boolVal = editor->isEnabled() ? VARIANT_TRUE : VARIANT_FALSE; break;
            case UIA_IsControlElementPropertyId: case UIA_IsContentElementPropertyId:
            case UIA_IsKeyboardFocusablePropertyId: case UIA_IsValuePatternAvailablePropertyId:
                value->vt = VT_BOOL; value->boolVal = VARIANT_TRUE; break;
            default: break;
            }
            return S_OK;
        });
    }
    IFACEMETHODIMP get_HostRawElementProvider(IRawElementProviderSimple **value) override
    {
        if (!value) return E_INVALIDARG;
        *value = nullptr;
        return IsWindow(m_window) ? UiaHostProviderFromHwnd(m_window, value) : UIA_E_ELEMENTNOTAVAILABLE;
    }
    IFACEMETHODIMP SetValue(LPCWSTR value) override
    {
        if (!value) return E_INVALIDARG;
        const QString text = QString::fromWCharArray(value);
        return onGui([&](GuardedFixtureTextEdit *editor) -> HRESULT {
            return editStatus(editor->replaceAccessibleText(text));
        });
    }
    IFACEMETHODIMP get_Value(BSTR *value) override
    {
        if (!value) return E_INVALIDARG;
        *value = nullptr;
        return onGui([=](GuardedFixtureTextEdit *editor) -> HRESULT {
            auto life = editor->lifetime().lock();
            if (!life || !life->document) return UIA_E_ELEMENTNOTAVAILABLE;
            QString text = life->document->toRawText();
            text.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
            *value = SysAllocStringLen(reinterpret_cast<const OLECHAR *>(text.utf16()), UINT(text.size()));
            return *value ? S_OK : E_OUTOFMEMORY;
        });
    }
    IFACEMETHODIMP get_IsReadOnly(BOOL *value) override
    {
        if (!value) return E_INVALIDARG;
        return onGui([=](GuardedFixtureTextEdit *editor) -> HRESULT {
            auto life = editor->lifetime().lock();
            *value = editor->isReadOnly() || editor->isInputMethodComposing() || (life && life->busy);
            return S_OK;
        });
    }

private:
    HWND m_window;
    std::weak_ptr<FixtureEditorLifetime> m_life;
};
} // namespace

struct FixtureWindowsValueBridge::Data
{
    HWND window = nullptr;
    ComPtr<ValueProvider> provider;
};

FixtureWindowsValueBridge::FixtureWindowsValueBridge(QQuickWindow *window, GuardedFixtureTextEdit *editor)
    : m_data(std::make_unique<Data>())
{
    m_data->window = reinterpret_cast<HWND>(window->winId());
    m_data->provider = Microsoft::WRL::Make<ValueProvider>(m_data->window, editor->lifetime());
    QCoreApplication::instance()->installNativeEventFilter(this);
    connect(editor, &GuardedFixtureTextEdit::accessibleValueChanged, this,
            [this](const QString &before, const QString &after) {
        VARIANT oldValue; VariantInit(&oldValue);
        VARIANT newValue; VariantInit(&newValue);
        oldValue.vt = newValue.vt = VT_BSTR;
        oldValue.bstrVal = SysAllocStringLen(reinterpret_cast<const OLECHAR *>(before.utf16()), UINT(before.size()));
        newValue.bstrVal = SysAllocStringLen(reinterpret_cast<const OLECHAR *>(after.utf16()), UINT(after.size()));
        UiaRaiseAutomationPropertyChangedEvent(m_data->provider.Get(), UIA_ValueValuePropertyId, oldValue, newValue);
        VariantClear(&oldValue); VariantClear(&newValue);
    });
}

FixtureWindowsValueBridge::~FixtureWindowsValueBridge()
{
    if (auto *app = QCoreApplication::instance()) app->removeNativeEventFilter(this);
}

bool FixtureWindowsValueBridge::nativeEventFilter(const QByteArray &, void *message, qintptr *result)
{
    const auto *event = static_cast<MSG *>(message);
    if (event->hwnd != m_data->window || event->message != WM_GETOBJECT
        || LONG(event->lParam) != UiaRootObjectId)
        return false;
    *result = UiaReturnRawElementProvider(event->hwnd, event->wParam, event->lParam, m_data->provider.Get());
    return true;
}
