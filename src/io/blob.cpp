#include "io/blob.h"

#include <windows.h>

namespace bl::io {
namespace {

std::wstring Wide(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                        static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring wide(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                        static_cast<int>(utf8.size()), wide.data(), len);
    return wide;
}

}  // namespace

bool ReadFileBytes(const std::string& utf8_path,
                   std::vector<unsigned char>* out) {
    if (out == nullptr) return false;
    out->clear();
    const std::wstring wide = Wide(utf8_path);
    if (wide.empty()) return false;

    HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) {
        CloseHandle(file);
        return false;
    }
    out->resize(static_cast<size_t>(size.QuadPart));

    size_t done = 0;
    while (done < out->size()) {
        const DWORD want = static_cast<DWORD>(
            (out->size() - done) > 0x7FFFFFFFu ? 0x7FFFFFFFu : (out->size() - done));
        DWORD read = 0;
        if (!ReadFile(file, out->data() + done, want, &read, nullptr) || read == 0) {
            CloseHandle(file);
            out->clear();
            return false;
        }
        done += read;
    }
    CloseHandle(file);
    return true;
}

bool WriteFileBytes(const std::string& utf8_path, const void* data,
                    std::size_t size) {
    const std::wstring wide = Wide(utf8_path);
    if (wide.empty()) return false;
    if (size > 0 && data == nullptr) return false;

    HANDLE file = CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    size_t done = 0;
    while (done < size) {
        const DWORD want = static_cast<DWORD>(
            (size - done) > 0x7FFFFFFFu ? 0x7FFFFFFFu : (size - done));
        DWORD written = 0;
        if (!WriteFile(file, bytes + done, want, &written, nullptr) ||
            written == 0) {
            CloseHandle(file);
            return false;
        }
        done += written;
    }
    CloseHandle(file);
    return true;
}

bool ReadResourceBytes(const wchar_t* name, std::vector<unsigned char>* out) {
    if (name == nullptr || out == nullptr) return false;
    out->clear();
    // 资源在自家 exe 里：按名字取 RCDATA，不改文件名、不怕中文路径。
    // RT_RCDATA 是 MAKEINTRESOURCE(10)（LPSTR 形态），W 版要求 LPCWSTR，统一转一手。
    HRSRC found = FindResourceW(nullptr, name,
                                reinterpret_cast<LPCWSTR>(RT_RCDATA));
    if (found == nullptr) return false;
    const DWORD size = SizeofResource(nullptr, found);
    HGLOBAL loaded = LoadResource(nullptr, found);
    if (loaded == nullptr) return false;
    const void* data = LockResource(loaded);
    if (data == nullptr || size == 0) return false;
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    out->assign(bytes, bytes + size);
    return true;
}

}  // namespace bl::io
