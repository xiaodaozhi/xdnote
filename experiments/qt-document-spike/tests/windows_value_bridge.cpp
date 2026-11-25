#include "windows_value_bridge.h"
#include "windows_uia_fixture.h"

#include <QAccessible>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;
using FixtureUia::Window;

namespace FixtureUia {
POINT clientOrigin(HWND window)
{
    POINT origin{};
    ClientToScreen(window, &origin);
    return origin;
}

QRectF nativeRect(const Window &window, const QRectF &clientRect)
{
    const POINT origin = clientOrigin(window.handle);
    const qreal scale = window.window->devicePixelRatio();
    return QRectF(origin.x + clientRect.x() * scale, origin.y + clientRect.y() * scale,
                  clientRect.width() * scale, clientRect.height() * scale);
}
}

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

int controlType(QAccessible::Role role)
{
    switch (role) {
    case QAccessible::Window: return UIA_WindowControlTypeId;
    case QAccessible::EditableText: return UIA_EditControlTypeId;
    case QAccessible::StaticText: return UIA_TextControlTypeId;
    case QAccessible::Button: return UIA_ButtonControlTypeId;
    case QAccessible::Grouping: return UIA_GroupControlTypeId;
    default: return UIA_CustomControlTypeId;
    }
}

class Node : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IRawElementProviderSimple,
                                 IRawElementProviderFragment, IRawElementProviderFragmentRoot,
                                 IValueProvider, IInvokeProvider>
{
public:
    Node(std::shared_ptr<Window> window, QAccessible::Id id, bool editor)
        : m_window(std::move(window)), m_id(id), m_editor(editor) {}

    IFACEMETHODIMP QueryInterface(REFIID id, void **value) override
    {
        if (!value) return E_POINTER;
        if ((!isRoot() && id == __uuidof(IRawElementProviderFragmentRoot))
            || (!m_editor && id == __uuidof(IValueProvider))) {
            *value = nullptr;
            return E_NOINTERFACE;
        }
        return RuntimeClass::QueryInterface(id, value);
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
        return onNode([&](QAccessibleInterface &node) -> HRESULT {
            if (m_editor && id == UIA_ValuePatternId)
                return QueryInterface(__uuidof(IValueProvider), reinterpret_cast<void **>(value));
            if (m_editor && id == UIA_TextPatternId)
                return FixtureUia::makeTextProvider(m_window, this, value);
            if (id == UIA_InvokePatternId && canInvoke(node))
                return QueryInterface(__uuidof(IInvokeProvider), reinterpret_cast<void **>(value));
            return S_OK;
        });
    }

    IFACEMETHODIMP GetPropertyValue(PROPERTYID id, VARIANT *value) override
    {
        if (!value) return E_INVALIDARG;
        VariantInit(value);
        return onNode([&](QAccessibleInterface &node) -> HRESULT {
            const auto state = node.state();
            switch (id) {
            case UIA_ControlTypePropertyId:
                value->vt = VT_I4;
                value->lVal = isRoot() ? UIA_WindowControlTypeId : controlType(node.role());
                break;
            case UIA_NamePropertyId: case UIA_HelpTextPropertyId:
                value->vt = VT_BSTR;
                value->bstrVal = FixtureUia::toBstr(node.text(id == UIA_NamePropertyId
                    ? QAccessible::Name : QAccessible::Description));
                return value->bstrVal ? S_OK : E_OUTOFMEMORY;
            case UIA_IsEnabledPropertyId: setBool(value, !state.disabled); break;
            case UIA_IsOffscreenPropertyId: setBool(value, state.offscreen || state.invisible); break;
            case UIA_HasKeyboardFocusPropertyId: setBool(value, state.focused); break;
            case UIA_IsKeyboardFocusablePropertyId: setBool(value, state.focusable); break;
            case UIA_IsControlElementPropertyId: case UIA_IsContentElementPropertyId:
                setBool(value, !state.invisible);
                break;
            case UIA_IsValuePatternAvailablePropertyId: case UIA_IsTextPatternAvailablePropertyId:
                setBool(value, m_editor);
                break;
            case UIA_IsInvokePatternAvailablePropertyId: setBool(value, canInvoke(node)); break;
            default: break;
            }
            return S_OK;
        });
    }

    IFACEMETHODIMP get_HostRawElementProvider(IRawElementProviderSimple **value) override
    {
        if (!value) return E_INVALIDARG;
        *value = nullptr;
        return onNode([&](QAccessibleInterface &) {
            return isRoot() ? UiaHostProviderFromHwnd(m_window->handle, value) : S_OK;
        });
    }

    IFACEMETHODIMP Navigate(NavigateDirection direction, IRawElementProviderFragment **value) override
    {
        if (!value) return E_INVALIDARG;
        *value = nullptr;
        return onNode([&](QAccessibleInterface &node) -> HRESULT {
            switch (direction) {
            case NavigateDirection_Parent:
                return isRoot() ? S_OK : fragment(node.parent(), value);
            case NavigateDirection_FirstChild:
                return child(node, 0, 1, value);
            case NavigateDirection_LastChild:
                return child(node, node.childCount() - 1, -1, value);
            case NavigateDirection_NextSibling: case NavigateDirection_PreviousSibling: {
                auto *parent = isRoot() ? nullptr : node.parent();
                const int index = parent ? parent->indexOfChild(&node) : -1;
                const int step = direction == NavigateDirection_NextSibling ? 1 : -1;
                return index >= 0 ? child(*parent, index + step, step, value) : S_OK;
            }
            }
            return E_INVALIDARG;
        });
    }

    IFACEMETHODIMP GetRuntimeId(SAFEARRAY **value) override
    {
        if (!value) return E_INVALIDARG;
        *value = nullptr;
        return onNode([&](QAccessibleInterface &) -> HRESULT {
            if (isRoot()) return S_OK;
            auto *array = SafeArrayCreateVector(VT_I4, 0, 2);
            if (!array) return E_OUTOFMEMORY;
            LONG index = 0;
            LONG part = UiaAppendRuntimeId;
            HRESULT status = SafeArrayPutElement(array, &index, &part);
            if (SUCCEEDED(status)) {
                ++index;
                part = static_cast<LONG>(m_id);
                status = SafeArrayPutElement(array, &index, &part);
            }
            if (SUCCEEDED(status)) *value = array;
            else SafeArrayDestroy(array);
            return status;
        });
    }

    IFACEMETHODIMP get_BoundingRectangle(UiaRect *value) override
    {
        if (!value) return E_INVALIDARG;
        *value = {};
        return onNode([&](QAccessibleInterface &node) -> HRESULT {
            const QRectF rectangle = FixtureUia::nativeRect(*m_window,
                node.rect().translated(-m_window->window->position()));
            *value = {rectangle.x(), rectangle.y(), rectangle.width(), rectangle.height()};
            return S_OK;
        });
    }

    IFACEMETHODIMP GetEmbeddedFragmentRoots(SAFEARRAY **value) override
    {
        if (!value) return E_INVALIDARG;
        *value = nullptr;
        return onNode([](QAccessibleInterface &) { return S_OK; });
    }

    IFACEMETHODIMP SetFocus() override
    {
        return onNode([](QAccessibleInterface &node) -> HRESULT {
            auto *actions = node.actionInterface();
            if (!actions || !actions->actionNames().contains(QAccessibleActionInterface::setFocusAction()))
                return UIA_E_INVALIDOPERATION;
            actions->doAction(QAccessibleActionInterface::setFocusAction());
            return S_OK;
        });
    }

    IFACEMETHODIMP get_FragmentRoot(IRawElementProviderFragmentRoot **value) override
    {
        if (!value) return E_INVALIDARG;
        *value = nullptr;
        return onNode([&](QAccessibleInterface &) -> HRESULT {
            return m_window->rootFragment ? m_window->rootFragment->QueryInterface(IID_PPV_ARGS(value))
                                          : HRESULT(UIA_E_ELEMENTNOTAVAILABLE);
        });
    }

    IFACEMETHODIMP ElementProviderFromPoint(double x, double y, IRawElementProviderFragment **value) override
    {
        if (!value) return E_INVALIDARG;
        *value = nullptr;
        return onNode([&](QAccessibleInterface &node) -> HRESULT {
            if (!isRoot()) return E_NOINTERFACE;
            const POINT origin = FixtureUia::clientOrigin(m_window->handle);
            const qreal scale = m_window->window->devicePixelRatio();
            const QPoint point = m_window->window->position() + QPointF((x - origin.x) / scale, (y - origin.y) / scale).toPoint();
            auto *target = &node;
            while (auto *child = target->childAt(point.x(), point.y())) {
                if (!child->isValid() || child == target) break;
                target = child;
            }
            return fragment(target, value);
        });
    }

    IFACEMETHODIMP GetFocus(IRawElementProviderFragment **value) override
    {
        if (!value) return E_INVALIDARG;
        *value = nullptr;
        return onNode([&](QAccessibleInterface &node) {
            return isRoot() ? fragment(node.focusChild(), value) : E_NOINTERFACE;
        });
    }

    IFACEMETHODIMP SetValue(LPCWSTR value) override
    {
        if (!value) return E_INVALIDARG;
        const QString text = QString::fromWCharArray(value);
        return onEditor([&](FixtureEditorLifetime &life) {
            return editStatus(life.editor->replaceAccessibleText(text));
        });
    }

    IFACEMETHODIMP get_Value(BSTR *value) override
    {
        if (!value) return E_INVALIDARG;
        *value = nullptr;
        return onEditor([&](FixtureEditorLifetime &life) {
            auto *node = QAccessible::queryAccessibleInterface(life.editor);
            if (!node) return HRESULT(UIA_E_ELEMENTNOTAVAILABLE);
            *value = FixtureUia::toBstr(node->text(QAccessible::Value));
            return *value ? S_OK : E_OUTOFMEMORY;
        });
    }

    IFACEMETHODIMP get_IsReadOnly(BOOL *value) override
    {
        if (!value) return E_INVALIDARG;
        *value = TRUE;
        return onEditor([&](FixtureEditorLifetime &life) {
            *value = life.editor->isReadOnly() || life.editor->isInputMethodComposing() || life.busy;
            return S_OK;
        });
    }

    IFACEMETHODIMP Invoke() override
    {
        return onNode([](QAccessibleInterface &node) -> HRESULT {
            if (node.state().disabled) return UIA_E_ELEMENTNOTENABLED;
            if (!canInvoke(node)) return UIA_E_INVALIDOPERATION;
            node.actionInterface()->doAction(QAccessibleActionInterface::pressAction());
            return S_OK;
        });
    }

private:
    bool isRoot() const { return m_id == 0; }

    static bool canInvoke(QAccessibleInterface &node)
    {
        const auto *actions = node.actionInterface();
        return actions && actions->actionNames().contains(QAccessibleActionInterface::pressAction());
    }

    static void setBool(VARIANT *value, bool state)
    {
        value->vt = VT_BOOL;
        value->boolVal = state ? VARIANT_TRUE : VARIANT_FALSE;
    }

    template<class Function> HRESULT onEditor(Function function)
    {
        return m_window->onGui([&](QQuickWindow &) -> HRESULT {
            const auto life = m_window->editor.lock();
            return m_editor && life && life->editor && !life->closing && life->document
                ? function(*life) : HRESULT(UIA_E_ELEMENTNOTAVAILABLE);
        });
    }

    template<class Function> HRESULT onNode(Function function) const
    {
        return m_window->onGui([&](QQuickWindow &window) -> HRESULT {
            if (m_editor) {
                const auto life = m_window->editor.lock();
                if (!life || !life->editor || life->closing)
                    return UIA_E_ELEMENTNOTAVAILABLE;
            }
            auto *node = isRoot() ? QAccessible::queryAccessibleInterface(&window)
                                  : QAccessible::accessibleInterface(m_id);
            return node && node->isValid() ? function(*node) : HRESULT(UIA_E_ELEMENTNOTAVAILABLE);
        });
    }

    HRESULT fragment(QAccessibleInterface *node, IRawElementProviderFragment **value) const
    {
        if (!node || !node->isValid()) return S_OK;
        const auto life = m_window->editor.lock();
        const bool editor = life && life->editor && node->object() == life->editor;
        if (editor && life->closing) return S_OK;
        auto *cached = editor ? m_window->editorFragment : nullptr;
        if (node->object() == m_window->window)
            cached = m_window->rootFragment;
        if (cached) {
            cached->AddRef();
            *value = cached;
            return S_OK;
        }
        auto provider = Microsoft::WRL::Make<Node>(m_window, QAccessible::uniqueId(node), false);
        return provider ? provider.CopyTo(value) : E_OUTOFMEMORY;
    }

    HRESULT child(QAccessibleInterface &parent, int index, int step, IRawElementProviderFragment **value) const
    {
        const int count = parent.childCount();
        for (; index >= 0 && index < count; index += step) {
            const HRESULT status = fragment(parent.child(index), value);
            if (FAILED(status) || *value) return status;
        }
        return S_OK;
    }

    const std::shared_ptr<Window> m_window;
    const QAccessible::Id m_id;
    const bool m_editor;
};
}

struct FixtureWindowsValueBridge::Data
{
    std::shared_ptr<Window> window = std::make_shared<Window>();
    ComPtr<Node> root;
    ComPtr<Node> editor;
};

FixtureWindowsValueBridge::FixtureWindowsValueBridge(QQuickWindow *window, GuardedFixtureTextEdit *editor)
    : m_data(std::make_unique<Data>())
{
    auto &context = *m_data->window;
    context.handle = reinterpret_cast<HWND>(window->winId());
    context.window = window;
    context.editor = editor->lifetime();
    m_data->root = Microsoft::WRL::Make<Node>(m_data->window, 0, false);
    auto *accessible = QAccessible::queryAccessibleInterface(editor);
    if (!accessible || !m_data->root) return;
    m_data->editor = Microsoft::WRL::Make<Node>(m_data->window, QAccessible::uniqueId(accessible), true);
    if (!m_data->editor) return;
    context.rootFragment = m_data->root.Get();
    context.editorFragment = m_data->editor.Get();
    QCoreApplication::instance()->installNativeEventFilter(this);
    connect(editor, &GuardedFixtureTextEdit::accessibleValueChanged, this,
            [this](const QString &before, const QString &after) {
        VARIANT oldValue;
        VARIANT newValue;
        VariantInit(&oldValue);
        VariantInit(&newValue);
        oldValue.vt = newValue.vt = VT_BSTR;
        oldValue.bstrVal = FixtureUia::toBstr(before);
        newValue.bstrVal = FixtureUia::toBstr(after);
        UiaRaiseAutomationPropertyChangedEvent(m_data->editor.Get(), UIA_ValueValuePropertyId, oldValue, newValue);
        UiaRaiseAutomationEvent(m_data->editor.Get(), UIA_Text_TextChangedEventId);
        VariantClear(&oldValue);
        VariantClear(&newValue);
    });
    connect(editor, &QQuickTextEdit::selectedTextChanged, this, [this] {
        UiaRaiseAutomationEvent(m_data->editor.Get(), UIA_Text_TextSelectionChangedEventId);
    });
    connect(editor, &QObject::destroyed, this, [this] {
        m_data->window->editorFragment = nullptr;
        UiaDisconnectProvider(m_data->editor.Get());
        UiaRaiseStructureChangedEvent(m_data->root.Get(), StructureChangeType_ChildrenInvalidated, nullptr, 0);
    });
}

FixtureWindowsValueBridge::~FixtureWindowsValueBridge()
{
    if (auto *app = QCoreApplication::instance()) app->removeNativeEventFilter(this);
    m_data->window->editorFragment = nullptr;
    m_data->window->rootFragment = nullptr;
    m_data->window->window.clear();
    if (m_data->root)
        UiaReturnRawElementProvider(m_data->window->handle, 0, 0, nullptr);
}

bool FixtureWindowsValueBridge::nativeEventFilter(const QByteArray &, void *message, qintptr *result)
{
    const auto *event = static_cast<MSG *>(message);
    if (event->hwnd != m_data->window->handle || event->message != WM_GETOBJECT
        || LONG(event->lParam) != UiaRootObjectId)
        return false;
    *result = UiaReturnRawElementProvider(event->hwnd, event->wParam, event->lParam, m_data->root.Get());
    return true;
}
