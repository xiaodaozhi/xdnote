// This client can target only the controlled window owned by its parent test
// process. It never enumerates or modifies other application windows.
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <string>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;

namespace {
class TextEvents final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IUIAutomationEventHandler>
{
public:
    IFACEMETHODIMP HandleAutomationEvent(IUIAutomationElement *, EVENTID eventId) override
    {
        if (eventId == UIA_Text_TextChangedEventId)
            ++count;
        return S_OK;
    }
    std::atomic<unsigned> count{0};
};

class ValueEvents final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IUIAutomationPropertyChangedEventHandler>
{
public:
    IFACEMETHODIMP HandlePropertyChangedEvent(IUIAutomationElement *, PROPERTYID propertyId, VARIANT value) override
    {
        if (propertyId == UIA_ValueValuePropertyId) {
            ++count;
            if (value.vt == VT_BSTR && value.bstrVal && std::wstring(value.bstrVal) == L"UIA modified")
                ++matching;
            if (value.vt == VT_BSTR && value.bstrVal && std::wstring(value.bstrVal) == L"original tail")
                ++undoMatching;
        }
        return S_OK;
    }
    std::atomic<unsigned> count{0};
    std::atomic<unsigned> matching{0};
    std::atomic<unsigned> undoMatching{0};
};

DWORD parentProcessId()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD parent = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == GetCurrentProcessId()) {
                parent = entry.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return parent;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3)
        return 2;
    const bool observeUndo = argc == 3 && std::string(argv[2]) == "--observe-undo";
    if (argc == 3 && !observeUndo)
        return 2;
    char *end = nullptr;
    const auto numericHandle = _strtoui64(argv[1], &end, 10);
    if (!numericHandle || !end || *end)
        return 2;
    const HWND window = reinterpret_cast<HWND>(static_cast<uintptr_t>(numericHandle));
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    wchar_t title[128]{};
    GetWindowTextW(window, title, int(std::size(title)));
    if (!owner || owner != parentProcessId() || std::wstring(title) != L"xdnote-controlled-uia-fixture")
        return 3;

    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized))
        return 4;
    struct ComScope {
        ~ComScope() { CoUninitialize(); }
    } comScope;

    ComPtr<IUIAutomation> automation;
    HRESULT result = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&automation));
    if (FAILED(result))
        return 5;
    ComPtr<IUIAutomationElement> root;
    result = automation->ElementFromHandle(window, &root);
    if (FAILED(result) || !root)
        return 6;

    VARIANT name;
    VariantInit(&name);
    name.vt = VT_BSTR;
    name.bstrVal = SysAllocString(L"Document");
    if (!name.bstrVal)
        return 7;
    ComPtr<IUIAutomationCondition> condition;
    result = automation->CreatePropertyCondition(UIA_NamePropertyId, name, &condition);
    VariantClear(&name);
    if (FAILED(result) || !condition)
        return 7;
    ComPtr<IUIAutomationElement> element;
    result = root->FindFirst(TreeScope_Descendants, condition.Get(), &element);
    if (FAILED(result) || !element)
        return 8;
    ComPtr<IUIAutomationValuePattern> pattern;
    result = element->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&pattern));
    if (FAILED(result) || !pattern)
        return 9;
    BOOL readOnly = FALSE;
    if (FAILED(pattern->get_CurrentIsReadOnly(&readOnly)))
        return 10;
    BSTR value = nullptr;
    result = pattern->get_CurrentValue(&value);
    const bool expectedValue = SUCCEEDED(result) && value && std::wstring(value) == L"original tail";
    SysFreeString(value);
    if (!expectedValue)
        return 11;

    auto textEvents = Make<TextEvents>();
    auto valueEvents = Make<ValueEvents>();
    if (!textEvents || !valueEvents)
        return 12;
    result = automation->AddAutomationEventHandler(UIA_Text_TextChangedEventId, element.Get(),
                                                    TreeScope_Element, nullptr, textEvents.Get());
    if (FAILED(result))
        return 13;
    PROPERTYID valueProperty = UIA_ValueValuePropertyId;
    result = automation->AddPropertyChangedEventHandlerNativeArray(element.Get(), TreeScope_Element,
                                                                    nullptr, valueEvents.Get(), &valueProperty, 1);
    if (FAILED(result)) {
        automation->RemoveAutomationEventHandler(UIA_Text_TextChangedEventId, element.Get(), textEvents.Get());
        return 14;
    }
    result = pattern->SetValue(L"UIA modified");
    for (int i = 0; i < 50 && (!textEvents->count.load() || !valueEvents->matching.load()); ++i)
        Sleep(10);
    const unsigned setTextEvents = textEvents->count.load();
    const unsigned setValueEvents = valueEvents->count.load();
    const unsigned setMatchingEvents = valueEvents->matching.load();
    unsigned undoTextEvents = 0;
    unsigned undoMatchingEvents = 0;
    if (observeUndo) {
        const unsigned beforeUndoMatches = valueEvents->undoMatching.load();
        std::cout << "UNDO_READY\n" << std::flush;
        std::string response;
        if (!std::getline(std::cin, response) || response != "CONTINUE")
            return 15;
        for (int i = 0; i < 100 && (textEvents->count.load() == setTextEvents
                                      || valueEvents->undoMatching.load() == beforeUndoMatches); ++i)
            Sleep(10);
        undoTextEvents = textEvents->count.load() - setTextEvents;
        undoMatchingEvents = valueEvents->undoMatching.load() - beforeUndoMatches;
    }
    automation->RemoveAutomationEventHandler(UIA_Text_TextChangedEventId, element.Get(), textEvents.Get());
    automation->RemovePropertyChangedEventHandler(element.Get(), valueEvents.Get());
    std::cout << "READONLY " << (readOnly ? 1 : 0) << '\n';
    std::cout << "HRESULT " << static_cast<std::uint32_t>(result) << '\n';
    std::cout << "TEXT_EVENTS " << setTextEvents << '\n';
    std::cout << "VALUE_EVENTS " << setValueEvents << '\n';
    std::cout << "MATCHING_VALUE_EVENTS " << setMatchingEvents << '\n';
    if (observeUndo) {
        std::cout << "UNDO_TEXT_EVENTS " << undoTextEvents << '\n';
        std::cout << "UNDO_MATCHING_VALUE_EVENTS " << undoMatchingEvents << '\n';
    }
    return 0;
}
