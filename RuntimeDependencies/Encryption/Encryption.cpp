#include <windows.h>
#include <wincrypt.h>
#include <string>
#include <vector>
#include <cstring>
#include <stdexcept>
#include <new>

namespace {
constexpr DWORD maximumBytes = 16 * 1024 * 1024;
struct Crypto {
    HCRYPTPROV provider{};
    HCRYPTHASH hash{};
    HCRYPTKEY key{};
    ~Crypto() {
        const DWORD error = GetLastError();
        if (key) CryptDestroyKey(key);
        if (hash) CryptDestroyHash(hash);
        if (provider) CryptReleaseContext(provider, 0);
        SetLastError(error);
    }
};
struct File {
    HANDLE value;
    ~File() { if (value != INVALID_HANDLE_VALUE) { const DWORD e = GetLastError(); CloseHandle(value); SetLastError(e); } }
};
bool Utf8(const wchar_t* value, std::string& result) {
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr, nullptr);
    if (!size) return false;
    result.resize(size);
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, result.data(), size, nullptr, nullptr)) return false;
    result.pop_back();
    return true;
}
bool Init(Crypto& crypto, const wchar_t* password) {
    std::string utf8;
    if (!Utf8(password, utf8)) return false;
    // Preserve the archived C++ DLL's format: UTF-8 password, SHA-256,
    // CryptoAPI CALG_AES_256, default CBC IV and final-block padding.
    return CryptAcquireContextW(&crypto.provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)
        && CryptCreateHash(crypto.provider, CALG_SHA_256, 0, 0, &crypto.hash)
        && CryptHashData(crypto.hash, reinterpret_cast<const BYTE*>(utf8.data()), static_cast<DWORD>(utf8.size()), 0)
        && CryptDeriveKey(crypto.provider, CALG_AES_256, crypto.hash, 0, &crypto.key);
}
bool Decrypt(const wchar_t* path, const wchar_t* password, std::wstring& result) {
    File file{CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.value, &size)) return false;
    if (size.QuadPart <= 0 || size.QuadPart > maximumBytes || size.QuadPart % 16) {
        SetLastError(ERROR_INVALID_DATA); return false;
    }
    std::vector<BYTE> data(static_cast<size_t>(size.QuadPart));
    DWORD count = 0;
    if (!ReadFile(file.value, data.data(), static_cast<DWORD>(data.size()), &count, nullptr)) return false;
    if (count != data.size()) { SetLastError(ERROR_HANDLE_EOF); return false; }
    Crypto crypto;
    if (!Init(crypto, password) || !CryptDecrypt(crypto.key, 0, TRUE, 0, data.data(), &count)) return false;
    if (!count) { result.clear(); return true; }
    if (memchr(data.data(), 0, count)) { SetLastError(ERROR_INVALID_DATA); return false; }
    const int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<char*>(data.data()), count, nullptr, 0);
    if (!chars) return false;
    result.resize(chars);
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<char*>(data.data()), count, result.data(), chars) != 0;
}
// The legacy ABI has no buffer capacity. Pin the result between the query
// and copy calls so a changing file cannot overflow a correctly sized caller.
struct Pending { std::wstring path, password, text; bool valid{}; };
thread_local Pending pending;
}

extern "C" __declspec(dllexport) int DecryptDataFromFile(wchar_t* path, wchar_t* password, wchar_t* output)
{
    try {
        if (!path || !*path || !password) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
        if (!output) {
            pending.valid = false;
            if (!Decrypt(path, password, pending.text)) return 0;
            pending.path = path;
            pending.password = password;
            pending.valid = true;
        } else if (!pending.valid || pending.path != path || pending.password != password) {
            SetLastError(ERROR_INVALID_PARAMETER); return 0;
        }
        const size_t bytes = (pending.text.size() + 1) * sizeof(wchar_t);
        if (bytes > maximumBytes) { pending.valid = false; SetLastError(ERROR_FILE_TOO_LARGE); return 0; }
        if (output) {
            memcpy(output, pending.text.c_str(), bytes);
            pending.valid = false;
            SecureZeroMemory(pending.password.data(), pending.password.size() * sizeof(wchar_t));
            pending.password.clear();
            pending.text.clear();
        }
        SetLastError(ERROR_SUCCESS);
        return static_cast<int>(bytes);
    } catch (const std::bad_alloc&) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
      catch (...) { SetLastError(ERROR_INVALID_DATA); return 0; }
}

extern "C" __declspec(dllexport) void EncryptDataToFile(wchar_t* text, wchar_t* path, wchar_t* password)
{
    try {
        if (!text || !path || !*path || !password) { SetLastError(ERROR_INVALID_PARAMETER); return; }
        std::string utf8;
        if (!Utf8(text, utf8)) return;
        if (utf8.size() > maximumBytes - 16) { SetLastError(ERROR_FILE_TOO_LARGE); return; }
        Crypto crypto;
        if (!Init(crypto, password)) return;
        DWORD size = static_cast<DWORD>(utf8.size());
        DWORD capacity = size;
        if (!CryptEncrypt(crypto.key, 0, TRUE, 0, nullptr, &capacity, 0)) return;
        std::vector<BYTE> data(capacity);
        memcpy(data.data(), utf8.data(), utf8.size());
        if (!CryptEncrypt(crypto.key, 0, TRUE, 0, data.data(), &size, capacity)) return;
        File file{CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (file.value == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        if (!WriteFile(file.value, data.data(), size, &written, nullptr)) return;
        if (written != size) { SetLastError(ERROR_WRITE_FAULT); return; }
        SetLastError(ERROR_SUCCESS);
    } catch (const std::bad_alloc&) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); }
      catch (...) { SetLastError(ERROR_INVALID_DATA); }
}
