#include "io/paths.h"

#include <windows.h>

namespace paths {
namespace {

std::string g_exe_dir;
std::string g_exe_path;

void EnsureInit() {
    if (!g_exe_path.empty()) return;
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(len > 0 ? len - 1 : 0, '\0');
    if (len > 1) {
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8.data(), len, nullptr, nullptr);
    }
    g_exe_path = utf8;
    size_t cut = g_exe_path.find_last_of("\\/");
    g_exe_dir = cut == std::string::npos ? std::string(".") : g_exe_path.substr(0, cut);
}

}  // namespace

const std::string& ExeDir() {
    EnsureInit();
    return g_exe_dir;
}

const std::string& ExePath() {
    EnsureInit();
    return g_exe_path;
}

std::string BesideExe(const std::string& filename) {
    return ExeDir() + "\\" + filename;
}

std::filesystem::path Utf8Path(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string Utf8String(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
}

}  // namespace paths
