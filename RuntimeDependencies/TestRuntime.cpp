#include <windows.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cwchar>

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) return 2;
    const std::wstring directory = argv[1];
    HMODULE helper = LoadLibraryW((directory + L"\\AIHelper.dll").c_str());
    HMODULE encryption = LoadLibraryW((directory + L"\\D-encryption.dll").c_str());
    if (!helper || !encryption) { std::printf("DLL load failed: %lu\n", GetLastError()); return 3; }
    using Load = BOOL(__stdcall*)(const wchar_t*, const wchar_t*);
    using Unload = BOOL(__stdcall*)(const wchar_t*);
    using Encrypt = void(*)(wchar_t*, wchar_t*, wchar_t*);
    using Decrypt = int(*)(wchar_t*, wchar_t*, wchar_t*);
    auto load = reinterpret_cast<Load>(GetProcAddress(helper, "LoadNTW"));
    auto unload = reinterpret_cast<Unload>(GetProcAddress(helper, "UnloadNTW"));
    auto encrypt = reinterpret_cast<Encrypt>(GetProcAddress(encryption, "EncryptDataToFile"));
    auto decrypt = reinterpret_cast<Decrypt>(GetProcAddress(encryption, "DecryptDataFromFile"));
    if (!load || !unload || !encrypt || !decrypt || !GetProcAddress(helper, "outDebug") || !GetProcAddress(helper, "InjectDll")) return 4;
    // Invalid-argument paths return before accessing SCM; no driver operations.
    if (load(nullptr, L"unused") || GetLastError() != ERROR_INVALID_PARAMETER) return 5;
    if (unload(nullptr) || GetLastError() != ERROR_INVALID_PARAMETER) return 6;
    wchar_t temporaryDirectory[MAX_PATH]{}, path[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temporaryDirectory) || !GetTempFileNameW(temporaryDirectory, L"udp", 0, path)) return 7;
    struct Cleanup { wchar_t* path; ~Cleanup() { DeleteFileW(path); } } cleanup{path};
    wchar_t text[] = L"UnrealDbgPro \u4e2d\u6587 \U0001F680\nround-trip";
    wchar_t password[] = L"test-\u5bc6\u94a5";
    wchar_t wrongPassword[] = L"incorrect";
    SetLastError(ERROR_SUCCESS);
    encrypt(text, path, password);
    if (GetLastError() != ERROR_SUCCESS) return 8;
    const int bytes = decrypt(path, password, nullptr);
    if (bytes != static_cast<int>((wcslen(text) + 1) * sizeof(wchar_t))) return 9;
    // Changing the file after sizing must not change the pinned copy result.
    wchar_t changed[] = L"a different and longer file contents for sizing race";
    encrypt(changed, path, password);
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t));
    if (decrypt(path, password, buffer.data()) != bytes || std::wstring(buffer.data()) != text) return 10;
    if (decrypt(path, wrongPassword, nullptr) != 0 || GetLastError() == ERROR_SUCCESS) return 11;
    if (decrypt(nullptr, password, nullptr) != 0 || GetLastError() != ERROR_INVALID_PARAMETER) return 12;
    wchar_t empty[] = L"";
    encrypt(empty, path, password);
    if (GetLastError() || decrypt(path, password, nullptr) != sizeof(wchar_t)) return 13;
    wchar_t emptyOutput[1] = {L'x'};
    if (decrypt(path, password, emptyOutput) != sizeof(wchar_t) || emptyOutput[0]) return 14;
    DeleteFileW(path);
    if (decrypt(path, password, nullptr) != 0 || GetLastError() != ERROR_FILE_NOT_FOUND) return 15;
    FreeLibrary(encryption);
    FreeLibrary(helper);
    std::puts("PASS: exports, invalid arguments, Unicode crypto round-trip, pinned result, wrong key, empty and missing file. No drivers loaded or processes injected.");
    return 0;
}
