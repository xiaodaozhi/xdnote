// Only addresses its parent test process's dedicated fixture HWND.
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <cstdint>
#include <iostream>
#include <string>

using Microsoft::WRL::ComPtr;

DWORD parentId()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD parent = 0;
    if (Process32FirstW(snapshot, &entry)) do {
        if (entry.th32ProcessID == GetCurrentProcessId()) { parent = entry.th32ParentProcessID; break; }
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return parent;
}

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    char *end = nullptr;
    const auto numeric = _strtoui64(argv[1], &end, 10);
    if (!numeric || !end || *end) return 2;
    const HWND window = reinterpret_cast<HWND>(static_cast<uintptr_t>(numeric));
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    wchar_t title[128]{};
    GetWindowTextW(window, title, 128);
    if (!owner || owner != parentId() || std::wstring(title) != L"xdnote-result-uia-fixture") return 3;
    const std::string mode = argv[2];
    if (mode != "valid" && mode != "invalid" && mode != "separator" && mode != "close") return 2;
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 4;
    struct Scope { ~Scope() { CoUninitialize(); } } scope;
    ComPtr<IUIAutomation> automation;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)))) return 5;
    ComPtr<IUIAutomationElement> element;
    if (FAILED(automation->ElementFromHandle(window, &element))) return 6;
    ComPtr<IUIAutomationValuePattern> value;
    if (FAILED(element->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&value)))) return 7;
    BSTR initial = nullptr;
    if (FAILED(value->get_CurrentValue(&initial))) return 8;
    const bool expected = initial && std::wstring(initial) == L"original";
    SysFreeString(initial);
    if (!expected) return 9;
    if (mode == "close") {
        std::cout << "READY\n" << std::flush;
        std::string response;
        if (!std::getline(std::cin, response) || response != "CONTINUE") return 10;
    }
    const wchar_t invalid[] = {wchar_t(0xd800), 0};
    const wchar_t separator[] = {wchar_t(0x2029), 0};
    const wchar_t *replacement = mode == "invalid" ? invalid : mode == "separator" ? separator : L"accepted";
    BSTR request = SysAllocString(replacement);
    if (!request) return 11;
    const HRESULT result = value->SetValue(request);
    SysFreeString(request);
    std::cout << "HRESULT " << static_cast<std::uint32_t>(result) << '\n';
    BSTR current = nullptr;
    const HRESULT readStatus = value->get_CurrentValue(&current);
    std::cout << "READSTATUS " << static_cast<std::uint32_t>(readStatus) << '\n';
    if (SUCCEEDED(readStatus)) {
        if (!current) return 11;
        const std::wstring actual(current);
        SysFreeString(current);
        if (actual == L"original") std::cout << "VALUE original\n";
        else if (actual == L"accepted") std::cout << "VALUE accepted\n";
        else return 12;
    }
    return 0;
}
