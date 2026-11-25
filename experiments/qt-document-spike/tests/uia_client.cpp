// This client can target only the controlled window owned by its parent test
// process. It never enumerates or modifies other application windows.
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include "uia_fixture_client.h"

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

ComPtr<IUIAutomationElement> findNamed(IUIAutomation *automation, IUIAutomationElement *root,
                                       const wchar_t *label)
{
    VARIANT name;
    VariantInit(&name);
    name.vt = VT_BSTR;
    name.bstrVal = SysAllocString(label);
    if (!name.bstrVal)
        return {};
    ComPtr<IUIAutomationCondition> condition;
    const HRESULT result = automation->CreatePropertyCondition(UIA_NamePropertyId, name, &condition);
    VariantClear(&name);
    ComPtr<IUIAutomationElement> element;
    if (SUCCEEDED(result) && condition)
        root->FindFirst(TreeScope_Descendants, condition.Get(), &element);
    return element;
}

bool matches(HRESULT result, BSTR actual, const wchar_t *expected)
{
    const bool equal = SUCCEEDED(result) && actual && std::wstring(actual, SysStringLen(actual)) == expected;
    SysFreeString(actual);
    return equal;
}

bool matchesText(IUIAutomationTextRange *range, const wchar_t *expected)
{
    BSTR actual = nullptr;
    const HRESULT result = range->GetText(-1, &actual);
    return matches(result, actual, expected);
}

bool matchesDocument(IUIAutomationTextPattern *text, IUIAutomationValuePattern *value, const wchar_t *expected)
{
    ComPtr<IUIAutomationTextRange> range;
    if (FAILED(text->get_DocumentRange(&range)) || !range || !matchesText(range.Get(), expected))
        return false;
    BSTR actual = nullptr;
    const HRESULT result = value->get_CurrentValue(&actual);
    return matches(result, actual, expected);
}

bool advance(const char *stage)
{
    std::cout << stage << std::endl;
    std::string response;
    return std::getline(std::cin, response) && response == "CONTINUE";
}

bool hasExpectedParent(IUIAutomation *automation, IUIAutomationElement *root,
                       IUIAutomationElement *sibling)
{
    const auto group = findNamed(automation, root, L"Fixture group");
    ComPtr<IUIAutomationTreeWalker> walker;
    ComPtr<IUIAutomationElement> parent;
    BOOL sameParent = FALSE;
    return group && SUCCEEDED(automation->get_RawViewWalker(&walker)) && walker
        && SUCCEEDED(walker->GetParentElement(sibling, &parent)) && parent
        && SUCCEEDED(automation->CompareElements(group.Get(), parent.Get(), &sameParent)) && sameParent;
}

bool invokeFixtureButton(IUIAutomation *automation, IUIAutomationElement *root)
{
    const auto button = findNamed(automation, root, L"Fixture button");
    ComPtr<IUIAutomationInvokePattern> invoke;
    if (!button || !hasExpectedParent(automation, root, button.Get())
        || FAILED(button->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invoke))) || !invoke
        || FAILED(invoke->Invoke()))
        return false;
    BSTR help = nullptr;
    const HRESULT result = button->get_CurrentHelpText(&help);
    return matches(result, help, L"Invoked");
}

bool verifyFixtureRanges(IUIAutomation *automation, IUIAutomationElement *editor,
                         IUIAutomationTextPattern *text)
{
    ComPtr<IUIAutomationTextRange> whole;
    if (FAILED(text->get_DocumentRange(&whole)) || !whole)
        return false;
    BSTR query = SysAllocString(L"TAIL");
    ComPtr<IUIAutomationTextRange> tail;
    const HRESULT found = query ? whole->FindText(query, FALSE, TRUE, &tail) : E_OUTOFMEMORY;
    SysFreeString(query);
    if (FAILED(found) || !tail || !matchesText(tail.Get(), L"tail"))
        return false;
    ComPtr<IUIAutomationTextRange> clone;
    BOOL same = FALSE;
    int comparison = 0;
    if (FAILED(tail->Clone(&clone)) || !clone || FAILED(tail->Compare(clone.Get(), &same)) || !same
        || FAILED(tail->CompareEndpoints(TextPatternRangeEndpoint_Start, whole.Get(),
                                         TextPatternRangeEndpoint_Start, &comparison))
        || comparison <= 0)
        return false;
    int moved = 0;
    if (FAILED(clone->MoveEndpointByUnit(TextPatternRangeEndpoint_Start, TextUnit_Character, 1, &moved))
        || moved != 1 || !matchesText(clone.Get(), L"ail") || FAILED(clone->Select()))
        return false;
    ComPtr<IUIAutomationTextRangeArray> selection;
    ComPtr<IUIAutomationTextRange> selected;
    if (FAILED(text->GetSelection(&selection)) || !selection || FAILED(selection->GetElement(0, &selected))
        || !selected || !matchesText(selected.Get(), L"ail"))
        return false;
    if (FAILED(clone->MoveEndpointByRange(TextPatternRangeEndpoint_Start, tail.Get(), TextPatternRangeEndpoint_Start))
        || FAILED(clone->ExpandToEnclosingUnit(TextUnit_Word)) || !matchesText(clone.Get(), L"tail")
        || FAILED(clone->Move(TextUnit_Word, -1, &moved)) || moved != -1 || !matchesText(clone.Get(), L"original "))
        return false;
    SupportedTextSelection selectionSupport = SupportedTextSelection_None;
    ComPtr<IUIAutomationTextRangeArray> visible;
    ComPtr<IUIAutomationTextRange> visibleRange;
    if (FAILED(text->get_SupportedTextSelection(&selectionSupport)) || selectionSupport != SupportedTextSelection_Single
        || FAILED(text->GetVisibleRanges(&visible)) || !visible || FAILED(visible->GetElement(0, &visibleRange))
        || !visibleRange || !matchesText(visibleRange.Get(), L"original tail"))
        return false;
    VARIANT readOnly;
    VariantInit(&readOnly);
    const HRESULT attribute = tail->GetAttributeValue(UIA_IsReadOnlyAttributeId, &readOnly);
    const bool writable = SUCCEEDED(attribute) && readOnly.vt == VT_BOOL && readOnly.boolVal == VARIANT_FALSE;
    VariantClear(&readOnly);
    if (!writable)
        return false;
    SAFEARRAY *rectangles = nullptr;
    const HRESULT bounds = tail->GetBoundingRectangles(&rectangles);
    LONG lower = 0;
    LONG upper = 0;
    double firstRect[4]{};
    bool measured = SUCCEEDED(bounds) && rectangles
        && SUCCEEDED(SafeArrayGetLBound(rectangles, 1, &lower))
        && SUCCEEDED(SafeArrayGetUBound(rectangles, 1, &upper)) && upper - lower >= 3;
    for (int i = 0; measured && i < 4; ++i) {
        LONG index = lower + i;
        measured = SUCCEEDED(SafeArrayGetElement(rectangles, &index, firstRect + i));
    }
    if (rectangles) SafeArrayDestroy(rectangles);
    if (!measured || firstRect[2] <= 0 || firstRect[3] <= 0)
        return false;
    const POINT point{LONG(firstRect[0] + firstRect[2] / 2), LONG(firstRect[1] + firstRect[3] / 2)};
    ComPtr<IUIAutomationTextRange> atPoint;
    if (FAILED(text->RangeFromPoint(point, &atPoint)) || !atPoint || !matchesText(atPoint.Get(), L""))
        return false;
    ComPtr<IUIAutomationElement> enclosing;
    BOOL sameEditor = FALSE;
    return SUCCEEDED(tail->GetEnclosingElement(&enclosing)) && enclosing
        && SUCCEEDED(automation->CompareElements(editor, enclosing.Get(), &sameEditor)) && sameEditor;
}

int verifyGuardedLifecycle(IUIAutomation *automation, IUIAutomationElement *root,
                           IUIAutomationElement *editor, IUIAutomationValuePattern *value, bool fixtureProvider)
{
    const auto sibling = findNamed(automation, root, L"Fixture sibling");
    ComPtr<IUIAutomationTextPattern> text;
    ComPtr<IUIAutomationTextRange> firstRange;
    if (!sibling || FAILED(editor->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&text))) || !text
        || !matchesDocument(text.Get(), value, L"First guarded document")
        || (fixtureProvider && (!hasExpectedParent(automation, root, sibling.Get())
                               || FAILED(text->get_DocumentRange(&firstRange)) || !firstRange))
        || !advance("REPLACE"))
        return 20;

    const auto rebound = findNamed(automation, root, L"Document");
    BOOL sameEditor = FALSE;
    if (!rebound || FAILED(automation->CompareElements(editor, rebound.Get(), &sameEditor)) || !sameEditor
        || !matchesDocument(text.Get(), value, L"original tail"))
        return 21;
    ComPtr<IUIAutomationTextRangeArray> selection;
    ComPtr<IUIAutomationTextRange> selected;
    int length = 0;
    if (FAILED(text->GetSelection(&selection)) || !selection || FAILED(selection->get_Length(&length))
        || length != 1 || FAILED(selection->GetElement(0, &selected)) || !selected
        || !matchesText(selected.Get(), L"tail"))
        return 22;
    if (fixtureProvider) {
        BSTR previousText = nullptr;
        const HRESULT previousStatus = firstRange->GetText(-1, &previousText);
        SysFreeString(previousText);
        if (previousStatus != static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE)
            || !verifyFixtureRanges(automation, editor, text.Get()))
            return 27;
        const wchar_t separator[] = {wchar_t(0x2029), 0};
        BSTR request = SysAllocString(separator);
        const HRESULT status = request ? value->SetValue(request) : E_OUTOFMEMORY;
        SysFreeString(request);
        if (status != E_INVALIDARG || !matchesDocument(text.Get(), value, L"original tail"))
            return 28;
    }
    if (FAILED(value->SetValue(L"UIA modified")) || !matchesDocument(text.Get(), value, L"UIA modified")
        || !advance("READ_ONLY"))
        return 23;
    BOOL readOnly = FALSE;
    if (FAILED(value->get_CurrentIsReadOnly(&readOnly)) || !readOnly || SUCCEEDED(value->SetValue(L"rejected"))
        || !matchesDocument(text.Get(), value, L"UIA modified") || !advance("UNDO_WRITE")
        || !matchesDocument(text.Get(), value, L"original tail") || !advance("UNDO_PRIOR")
        || !matchesDocument(text.Get(), value, L"original") || !advance("CLOSE"))
        return 24;

    ComPtr<IUIAutomationTextRange> closedRange;
    const HRESULT closedText = text->get_DocumentRange(&closedRange);
    BSTR closedValue = nullptr;
    const HRESULT closedRead = value->get_CurrentValue(&closedValue);
    const UINT closedValueLength = SysStringLen(closedValue);
    SysFreeString(closedValue);
    const HRESULT closedWrite = value->SetValue(L"after close");
    HRESULT closedPropertyStatus = S_OK;
    const bool propertyUnavailable = valuePropertyUnavailable(automation, editor, &closedPropertyStatus);
    std::cout << "CLOSED_HRESULTS " << std::hex << static_cast<std::uint32_t>(closedText)
              << ' ' << static_cast<std::uint32_t>(closedRead) << ' ' << static_cast<std::uint32_t>(closedWrite)
              << " VALUE_PROPERTY " << static_cast<std::uint32_t>(closedPropertyStatus)
              << " VALUE_LENGTH " << std::dec << closedValueLength
              << std::endl;
    if (closedText != static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE) || closedRange || closedValueLength != 0
        || SUCCEEDED(closedWrite) || !propertyUnavailable
        || findNamed(automation, root, L"Document"))
        return 25;

    const auto remainingSibling = findNamed(automation, root, L"Fixture sibling");
    BOOL sameSibling = FALSE;
    if (!remainingSibling || FAILED(automation->CompareElements(sibling.Get(), remainingSibling.Get(), &sameSibling))
        || !sameSibling || (fixtureProvider && (!hasExpectedParent(automation, root, remainingSibling.Get())
                                               || !invokeFixtureButton(automation, root))))
        return 26;
    return 0;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3)
        return 2;
    const std::string mode = argc == 3 ? argv[2] : "";
    const bool observeUndo = mode == "--observe-undo";
    const bool fixtureProvider = mode == "--fixture-lifecycle";
    const bool guardedLifecycle = mode == "--guarded-lifecycle" || fixtureProvider;
    if (argc == 3 && !observeUndo && !guardedLifecycle)
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
    const wchar_t *expectedTitle = fixtureProvider ? L"xdnote-fixture-uia-lifecycle"
        : guardedLifecycle ? L"xdnote-guarded-uia-lifecycle" : L"xdnote-controlled-uia-fixture";
    if (!owner || owner != parentProcessId() || std::wstring(title) != expectedTitle)
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

    const auto element = findNamed(automation.Get(), root.Get(), L"Document");
    if (!element)
        return 8;
    ComPtr<IUIAutomationValuePattern> pattern;
    result = element->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&pattern));
    if (FAILED(result) || !pattern)
        return 9;
    if (guardedLifecycle)
        return verifyGuardedLifecycle(automation.Get(), root.Get(), element.Get(), pattern.Get(), fixtureProvider);
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
