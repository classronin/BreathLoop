// 进程入口：只做参数解析与分发，不写业务。
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "bootstrap.h"
#include "buildcheck.h"
#include "config.h"

namespace {

std::string ToUtf8(const wchar_t* text) {
    int len = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), len, nullptr, nullptr);
    return out;
}

}  // namespace

// 退出时不再走 CRT 的静态析构：静态链接的 libvips/glib 在本机析构阶段会 0xC0000005，
// WER 还要挂 ~100 秒。业务数据（设置、日志）都在运行期显式落盘，这里直接用
// ExitProcess 交还退出码即可。
int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    AppOptions options;
    bool buildcheck = false;
    bool exportcheck = false;
    bool dumpframe = false;
    std::string export_asset;
    std::string export_base;
    if (argv != nullptr) {
        for (int i = 1; i < argc; ++i) {
            std::string arg = ToUtf8(argv[i]);
            if (arg == "--buildcheck") {
                buildcheck = true;
            } else if (arg == "--exporttest") {
                exportcheck = true;
                if (i + 1 < argc) export_asset = ToUtf8(argv[++i]);
                if (i + 1 < argc) export_base = ToUtf8(argv[++i]);
            } else if (arg == "--dumpframe") {
                dumpframe = true;
                if (i + 1 < argc) export_asset = ToUtf8(argv[++i]);
                if (i + 1 < argc) export_base = ToUtf8(argv[++i]);
            } else if (arg == "--shot") {
                if (i + 1 < argc) options.shot_path = ToUtf8(argv[++i]);
            } else if (options.initial_image.empty()) {
                options.initial_image = arg;
            }
        }
        LocalFree(argv);
    }

    int rc = 0;
    if (buildcheck) rc = RunBuildCheck();
    else if (exportcheck) rc = RunExportCheck(export_asset, export_base);
    else if (dumpframe) rc = RunDumpFrame(export_asset, export_base);
    else rc = RunApp(options);
    ExitProcess(static_cast<UINT>(rc));
}
