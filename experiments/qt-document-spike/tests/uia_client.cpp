// This client can target only the controlled window owned by its parent test
// process. It never enumerates or modifies other application windows.
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <UIAutomation.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {
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
    if (argc != 2)
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

    result = pattern->SetValue(L"UIA modified");
    std::cout << "READONLY " << (readOnly ? 1 : 0) << '\n';
    std::cout << "HRESULT " << static_cast<std::uint32_t>(result) << '\n';
    return 0;
}
