#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

namespace {
std::vector<BYTE> tokenInformation(HANDLE token, TOKEN_INFORMATION_CLASS kind) {
    DWORD size = 0;
    GetTokenInformation(token, kind, nullptr, 0, &size);
    std::vector<BYTE> bytes(size);
    if (!size || !GetTokenInformation(token, kind, bytes.data(), size, &size)) return {};
    return bytes;
}

int server(const wchar_t *name) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return 20;
    auto user = tokenInformation(token, TokenUser);
    auto group = tokenInformation(token, TokenPrimaryGroup);
    auto package = tokenInformation(token, TokenAppContainerSid);
    CloseHandle(token);
    if (user.empty() || group.empty() || package.empty()) return 21;
    auto userSid = reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid;
    auto groupSid = reinterpret_cast<TOKEN_PRIMARY_GROUP *>(group.data())->PrimaryGroup;
    auto packageSid = reinterpret_cast<TOKEN_APPCONTAINER_INFORMATION *>(package.data())->TokenAppContainer;
    if (!packageSid) return 22;
    std::vector<BYTE> aclStorage(1024);
    auto acl = reinterpret_cast<PACL>(aclStorage.data());
    SECURITY_DESCRIPTOR descriptor;
    if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
        !InitializeAcl(acl, static_cast<DWORD>(aclStorage.size()), ACL_REVISION) ||
        !AddAccessAllowedAce(acl, ACL_REVISION, FILE_ALL_ACCESS, userSid) ||
        !AddAccessAllowedAce(acl, ACL_REVISION, FILE_ALL_ACCESS, packageSid) ||
        !SetSecurityDescriptorOwner(&descriptor, userSid, FALSE) ||
        !SetSecurityDescriptorGroup(&descriptor, groupSid, FALSE) ||
        !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE)) return 23;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), &descriptor, FALSE};
    HANDLE pipe = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                  PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                  8, 4096, 4096, 3000, &attributes);
    if (pipe == INVALID_HANDLE_VALUE) {
        std::printf("pipe error %lu\n", GetLastError()); std::fflush(stdout);
        return 24;
    }
    std::puts("ready"); std::fflush(stdout);
    const int command = std::getchar();
    CloseHandle(pipe);
    return command == 'x' ? 0 : 25;
}

int client(const wchar_t *name, bool createInstance) {
    HANDLE pipe = createInstance
        ? CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                           8, 4096, 4096, 3000, nullptr)
        : CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_ACCESS_DENIED ? 5 : 100 + static_cast<int>(error);
    }
    CloseHandle(pipe);
    return 0;
}
}

int wmain(int argc, wchar_t **argv) {
    if (argc != 3) return 2;
    const std::wstring mode(argv[1]);
    if (mode == L"server") return server(argv[2]);
    if (mode == L"open") return client(argv[2], false);
    if (mode == L"instance") return client(argv[2], true);
    return 3;
}
