#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <cstdarg>
#include <cwchar>

namespace {
struct ServiceHandle {
    SC_HANDLE value;
    ~ServiceHandle() { if (value) { const DWORD e = GetLastError(); CloseServiceHandle(value); SetLastError(e); } }
};
struct Handle {
    HANDLE value;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) { const DWORD e = GetLastError(); CloseHandle(value); SetLastError(e); } }
};
}

// Stable C ABI: no STL objects or allocator ownership cross the DLL boundary.
extern "C" __declspec(dllexport) BOOL __stdcall LoadNTW(
    const wchar_t* imagePath, const wchar_t* serviceName)
{
    if (!imagePath || !*imagePath || !serviceName || !*serviceName) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    const DWORD attributes = GetFileAttributesW(imagePath);
    if (attributes == INVALID_FILE_ATTRIBUTES) return FALSE;
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    ServiceHandle manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE)};
    if (!manager.value) return FALSE;
    // Existing services are handled and identity-checked by InitNTDevice.cpp.
    // Never start/reconfigure an existing service here.
    // Kernel-driver ImagePath is a filename, not a user-mode command line.
    // Keep the validated path intact rather than adding literal quote characters.
    ServiceHandle service{CreateServiceW(manager.value, serviceName, serviceName,
        SERVICE_START | DELETE, SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START,
        SERVICE_ERROR_NORMAL, imagePath, nullptr, nullptr, nullptr, nullptr, nullptr)};
    if (!service.value) {
        if (GetLastError() == ERROR_SERVICE_EXISTS) SetLastError(ERROR_ALREADY_EXISTS);
        return FALSE;
    }
    if (!StartServiceW(service.value, 0, nullptr)) {
        const DWORD error = GetLastError();
        DeleteService(service.value); // only the service just created above
        SetLastError(error);
        return FALSE;
    }
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL __stdcall UnloadNTW(const wchar_t* serviceName)
{
    if (!serviceName || !*serviceName) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    ServiceHandle manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!manager.value) return FALSE;
    ServiceHandle service{OpenServiceW(manager.value, serviceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE)};
    if (!service.value) return FALSE;
    SERVICE_STATUS status{};
    if (!QueryServiceStatus(service.value, &status)) return FALSE;
    if (status.dwCurrentState != SERVICE_STOPPED) {
        if (!ControlService(service.value, SERVICE_CONTROL_STOP, &status) && GetLastError() != ERROR_SERVICE_NOT_ACTIVE)
            return FALSE;
        const ULONGLONG deadline = GetTickCount64() + 10000;
        do {
            if (!QueryServiceStatus(service.value, &status)) return FALSE;
            if (status.dwCurrentState == SERVICE_STOPPED) break;
            if (GetTickCount64() >= deadline) { SetLastError(ERROR_TIMEOUT); return FALSE; }
            Sleep(50);
        } while (true);
    }
    if (!DeleteService(service.value)) return FALSE;
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

extern "C" __declspec(dllexport) int __stdcall outDebug(const wchar_t* format, ...)
{
    const DWORD savedError = GetLastError();
    if (!format) { SetLastError(ERROR_INVALID_PARAMETER); return -1; }
    wchar_t text[32768]{};
    va_list args;
    va_start(args, format);
    const int count = _vsnwprintf_s(text, _countof(text), _TRUNCATE, format, args);
    va_end(args);
    using PrintLog = void(__stdcall*)(wchar_t*);
    auto callback = reinterpret_cast<PrintLog>(GetProcAddress(GetModuleHandleW(nullptr), "PrintLog"));
    if (callback) callback(text); else OutputDebugStringW(text);
    SetLastError(savedError);
    return count;
}

extern "C" __declspec(dllexport) void __stdcall InjectDll(DWORD pid, wchar_t* dllPath, BOOL is64Process)
{
    // This replacement deliberately supports only the solution's x64 target.
    if (!pid || !dllPath || !*dllPath) { SetLastError(ERROR_INVALID_PARAMETER); return; }
    if (!is64Process) { SetLastError(ERROR_NOT_SUPPORTED); return; }
    wchar_t absolutePath[32768]{};
    const DWORD pathLength = GetFullPathNameW(dllPath, _countof(absolutePath), absolutePath, nullptr);
    if (!pathLength) return;
    if (pathLength >= _countof(absolutePath)) { SetLastError(ERROR_FILENAME_EXCED_RANGE); return; }
    if (GetFileAttributesW(absolutePath) == INVALID_FILE_ATTRIBUTES) return;
    Handle process{OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid)};
    if (!process.value) return;
    BOOL wow64 = FALSE;
    if (!IsWow64Process(process.value, &wow64)) return;
    if (wow64) { SetLastError(ERROR_NOT_SUPPORTED); return; }

    // LoadLibraryW may be forwarded to KernelBase; use the actual owner's RVA
    // and the target's matching module base rather than a local ASLR address.
    const auto loader = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HMODULE owner = nullptr;
    if (!loader || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(loader), &owner)) return;
    wchar_t ownerPath[32768]{};
    const DWORD ownerLength = GetModuleFileNameW(owner, ownerPath, _countof(ownerPath));
    if (!ownerLength) return;
    if (ownerLength >= _countof(ownerPath)) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return; }
    const wchar_t* ownerName = wcsrchr(ownerPath, L'\\');
    ownerName = ownerName ? ownerName + 1 : ownerPath;
    Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid)};
    if (snapshot.value == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    LPTHREAD_START_ROUTINE remoteLoader = nullptr;
    if (Module32FirstW(snapshot.value, &module)) do {
        if (_wcsicmp(module.szModule, ownerName) == 0) {
            const auto offset = reinterpret_cast<ULONG_PTR>(loader) - reinterpret_cast<ULONG_PTR>(owner);
            if (offset < module.modBaseSize)
                remoteLoader = reinterpret_cast<LPTHREAD_START_ROUTINE>(module.modBaseAddr + offset);
            break;
        }
    } while (Module32NextW(snapshot.value, &module));
    if (!remoteLoader) { SetLastError(ERROR_MOD_NOT_FOUND); return; }
    const SIZE_T bytes = (pathLength + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(process.value, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) return;
    DWORD error = ERROR_SUCCESS;
    SIZE_T written = 0;
    bool mayFree = true;
    if (!WriteProcessMemory(process.value, remotePath, absolutePath, bytes, &written) || written != bytes) {
        error = GetLastError();
        if (!error) error = ERROR_WRITE_FAULT;
    } else {
        Handle thread{CreateRemoteThread(process.value, nullptr, 0, remoteLoader, remotePath, 0, nullptr)};
        if (!thread.value) error = GetLastError();
        else {
            const DWORD wait = WaitForSingleObject(thread.value, 10000);
            if (wait != WAIT_OBJECT_0) {
                error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
                mayFree = false; // the target thread may still read its argument
            } else {
                DWORD exitCode = 0;
                if (!GetExitCodeThread(thread.value, &exitCode)) error = GetLastError();
                else if (!exitCode) error = ERROR_DLL_INIT_FAILED;
            }
        }
    }
    if (mayFree) VirtualFreeEx(process.value, remotePath, 0, MEM_RELEASE);
    if (error) outDebug(L"[AIHelper replacement] InjectDll failed: pid=%lu error=%lu", pid, error);
    SetLastError(error);
}

#ifdef _MSC_VER
// Original clients must use the same MSVC STL/CRT ABI. New clients use W APIs.
extern "C" __declspec(dllexport) BOOL __stdcall LoadNT(const std::wstring imagePath, const std::wstring serviceName)
{ return LoadNTW(imagePath.c_str(), serviceName.c_str()); }
extern "C" __declspec(dllexport) BOOL __stdcall UnloadNT(const std::wstring serviceName)
{ return UnloadNTW(serviceName.c_str()); }
#endif
