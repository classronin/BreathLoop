#include "bootstrap.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
// 需要 HWND 才能改窗口类背景画刷（见下面 _kill_white_eraser 对应段）。
// 按 GLFW 惯例：先定义 GLFW_EXPOSE_NATIVE_WIN32，再包含 glfw3.h + glfw3native.h。
#include <GLFW/glfw3native.h>

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <vips/vips.h>

#include "config.h"
#include "io/paths.h"
#include "io/settings.h"
#include "render/gl.h"
#include "render/backdrop.h"
#include "render/shaders.h"
#include "ui/painter.h"
#include "ui/scene.h"
#include "ui/text.h"

namespace {

// 初始化失败要让人看见，而不是默默退出：这是给用户的提示，不是调试输出
std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), len);
    return out;
}

void FailToStart(const std::wstring& detail) {
    MessageBoxW(nullptr, (L"BreathLoop 启动失败：" + detail).c_str(), L"BreathLoop",
                MB_ICONERROR | MB_OK);
}

void OnError(int code, const char* description) {
    FailToStart(L"GLFW " + std::to_wstring(code) + L"：" +
                Utf8ToWide(description == nullptr ? "" : description));
}

bl::ui::Scene* SceneOf(GLFWwindow* window) {
    return static_cast<bl::ui::Scene*>(glfwGetWindowUserPointer(window));
}

void OnFileDrop(GLFWwindow* window, int count, const char** dropped) {
    bl::ui::Scene* scene = SceneOf(window);
    if (scene == nullptr) return;
    std::vector<std::string> paths;
    for (int i = 0; i < count; ++i) paths.push_back(dropped[i]);
    scene->file_drop(paths);
}

void OnKey(GLFWwindow* window, int key, int, int action, int mods) {
    bl::ui::Scene* scene = SceneOf(window);
    if (scene == nullptr) return;
    if (action == GLFW_PRESS) scene->key_press(key, mods);
    else if (action == GLFW_RELEASE) scene->key_release(key, mods);
}

void OnChar(GLFWwindow* window, unsigned int codepoint) {
    bl::ui::Scene* scene = SceneOf(window);
    if (scene == nullptr || codepoint == 0) return;
    unsigned int cp = codepoint;
    char utf8[5] = {0, 0, 0, 0, 0};
    if (cp < 0x80) {
        utf8[0] = static_cast<char>(cp);
    } else if (cp < 0x800) {
        utf8[0] = static_cast<char>(0xC0 | (cp >> 6));
        utf8[1] = static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        utf8[0] = static_cast<char>(0xE0 | (cp >> 12));
        utf8[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        utf8[2] = static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        utf8[0] = static_cast<char>(0xF0 | (cp >> 18));
        utf8[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        utf8[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        utf8[3] = static_cast<char>(0x80 | (cp & 0x3F));
    }
    scene->text_input(std::string(utf8));
}

// GLFW 的光标坐标是窗口坐标（原点左上、y 向下），场景里一律是
// 帧缓冲坐标（原点左下、y 向上）。边界只在这里换算一次。
double ScaleOf(GLFWwindow* window) {
    int ww = 0;
    int wh = 0;
    int fw = 0;
    int fh = 0;
    glfwGetWindowSize(window, &ww, &wh);
    glfwGetFramebufferSize(window, &fw, &fh);
    if (ww <= 0 || wh <= 0) return 1.0;
    (void)fw;
    return static_cast<double>(fh) / static_cast<double>(wh);
}

double SceneX(GLFWwindow* window, double x) {
    return x * ScaleOf(window);
}

double SceneY(GLFWwindow* window, double y) {
    int ww = 0;
    int wh = 0;
    int fw = 0;
    int fh = 0;
    glfwGetWindowSize(window, &ww, &wh);
    glfwGetFramebufferSize(window, &fw, &fh);
    if (ww <= 0 || wh <= 0) return y;
    (void)fw;
    return static_cast<double>(fh) - y * ScaleOf(window);
}

void OnCursorPos(GLFWwindow* window, double x, double y) {
    bl::ui::Scene* scene = SceneOf(window);
    if (scene == nullptr) return;
    const double sy = SceneY(window, y);
    const double dx = SceneX(window, x) - scene->mouse_x;
    const double dy = sy - scene->mouse_y;
    // GLFW 的 motion 回调不带 mods，拖动中的 Ctrl 只能自己查键态
    // （Ctrl+T 的"解除比例"要在拖角时读它）。
    int mods = 0;
    if (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS
        || glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS) {
        mods |= GLFW_MOD_CONTROL;
    }
    if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS
        || glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS) {
        mods |= GLFW_MOD_SHIFT;
    }
    if (glfwGetKey(window, GLFW_KEY_LEFT_ALT) == GLFW_PRESS
        || glfwGetKey(window, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS) {
        mods |= GLFW_MOD_ALT;
    }
    if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS ||
        glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS) {
        scene->mouse_drag(SceneX(window, x), sy, dx, dy, mods);
    } else {
        scene->mouse_motion(SceneX(window, x), sy, dx, dy);
    }
}

void OnMouseButton(GLFWwindow* window, int button, int action, int mods) {
    bl::ui::Scene* scene = SceneOf(window);
    if (scene == nullptr) return;
    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(window, &x, &y);
    const double sx = SceneX(window, x);
    const double sy = SceneY(window, y);
    if (action == GLFW_PRESS) scene->mouse_press(sx, sy, button, mods);
    else scene->mouse_release(sx, sy, button, mods);
}

void OnScroll(GLFWwindow* window, double, double dy) {
    bl::ui::Scene* scene = SceneOf(window);
    if (scene == nullptr) return;
    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(window, &x, &y);
    scene->mouse_scroll(SceneX(window, x), SceneY(window, y), dy);
}

void OnFocus(GLFWwindow* window, int focused) {
    bl::ui::Scene* scene = SceneOf(window);
    if (scene == nullptr) return;
    if (!focused) scene->deactivate();
}

// 窗口呈现按 GLFW 的公开契约，不再自己拼 Win32 置前序列：
// - docs/window.md:1008-1013：被最小化的窗口用 glfwRestoreWindow 还原；
//   Win32 后端就是 ShowWindow(SW_RESTORE)（win32_window.c:1781-1784）。
// - sdk-api .../nf-winuser-showwindow.md:91：SW_SHOW 只按"当前尺寸与位置"显示，
//   不会把最小化还原；:95 的 SW_RESTORE 才是还原。
// - :111-118：第一次 ShowWindow 被启动方 STARTUPINFO 覆盖 —— GLFW 建窗后的那次
//   显示（SW_SHOWNA，win32_window.c:1794-1813）正好是第一次，所以双击启动若是
//   最小化，只补 SW_SHOW 没用，必须先 restore 再 show。
// - 聚焦契约见 docs/window.md:1128-1131。
void PresentWindow(GLFWwindow* window, bool center) {
    int mx = 0;
    int my = 0;
    int mw = 0;
    int mh = 0;
    glfwGetMonitorWorkarea(glfwGetPrimaryMonitor(), &mx, &my, &mw, &mh);
    int ww = 0;
    int wh = 0;
    glfwGetWindowSize(window, &ww, &wh);
    if (center && mw > 0 && mh > 0) {
        glfwSetWindowPos(window, mx + (mw - ww) / 2, my + (mh - wh) / 2);
    }

    if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0) {
        glfwRestoreWindow(window);   // SW_RESTORE：还原最小化/最大化
    }
    glfwShowWindow(window);
    glfwFocusWindow(window);
}

// 存档里的窗口矩形是否还落在某一台显示器的工作区里：显示器拔了/分辨率改了
// 之后，旧坐标可能整块在屏幕外，那种情况就当没存过、按默认尺寸居中开。
bool PlacementOnScreen(const bl::io::WindowPlacement& place) {
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    for (int i = 0; i < count; ++i) {
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        glfwGetMonitorWorkarea(monitors[i], &x, &y, &w, &h);
        const int ox = place.x < x ? x : place.x;
        const int oy = place.y < y ? y : place.y;
        const int ox2 = place.x + place.w < x + w ? place.x + place.w : x + w;
        const int oy2 = place.y + place.h < y + h ? place.y + place.h : y + h;
        // 至少要有 160×80 的标题栏区域压在屏上，才认为这个位置可用
        if (ox2 - ox >= 160 && oy2 - oy >= 80) return true;
    }
    return false;
}

}  // namespace

int RunApp(const AppOptions& options) {
    glfwSetErrorCallback(OnError);
    if (!glfwInit()) {
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_SAMPLES, 0);
    // 先建一个不上屏的窗口：窗口一旦上屏，到第一次 swap 之间客户区由 DWM 用
    // 白底填充，双击启动时看到的那一下白闪就是它。先隐藏，把首帧画成与界面
    // 同色（画布外的深灰）再显示，白色阶段就整段不存在。
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    // 上次退出时的窗口摆放：位置/大小/是否最大化都按存档恢复。窗口仍然是
    // 隐藏创建的，所以这里**不用** GLFW_MAXIMIZED 建窗提示：那条路走的是
    // GLFW 自己拼的"手动最大化"（win32_window.c:481-527，把窗口矩形按
    // SM_CYCAPTION 往上顶一截），恢复出来的位置比系统原生最大化高出一个
    // 标题栏，与用户点最大化按钮的结果不一致。改成先按还原矩形建窗、亮出来
    // 之后再走 glfwMaximizeWindow（可见窗口 = 原生 SW_MAXIMIZE）。
    const bl::io::WindowPlacement place = bl::io::LoadWindowPlacement();
    bool use_place = false;
    bool wanted_maximized = false;
    int start_w = cfg::WIN_W;
    int start_h = cfg::WIN_H;
    if (place.valid && PlacementOnScreen(place)) {
        use_place = true;
        start_w = place.w;
        start_h = place.h;
        glfwWindowHint(GLFW_POSITION_X, place.x);
        glfwWindowHint(GLFW_POSITION_Y, place.y);
        wanted_maximized = place.maximized;
    }

    GLFWwindow* window = glfwCreateWindow(start_w, start_h,
                                          cfg::WINDOW_TITLE, nullptr, nullptr);
    if (window == nullptr) {
        FailToStart(L"无法创建 OpenGL 3.3 窗口（请确认显卡驱动）");
        glfwTerminate();
        return 1;
    }
    glfwSetWindowSizeLimits(window, cfg::MIN_W, cfg::MIN_H, GLFW_DONT_CARE,
                            GLFW_DONT_CARE);
    // 白闪的真正来源（复用 ImageEditor Bootstrap 的做法，见
    // E:\GitHub\C\ImageEditor\BreathLoop\breathloop\bootstrap.py:12-29 的
    // _kill_white_eraser）：系统在窗口首次显示、尺寸变化时，先用**窗口类的
    // 背景画刷**擦一遍红区表面，再轮到 GL 换帧；GLFW 注册窗口类时
    // hbrBackground 没给（E:\source\glfw\src\win32_window.c:1284-1306），
    // 于是那一擦是系统白。把类画刷换成画布外底色（深灰），这段间隙就看不见。
    HWND hwnd = glfwGetWin32Window(window);
    if (hwnd != nullptr) {
        static HBRUSH erase_brush = nullptr;   // 活到进程结束（末尾 ExitProcess）
        if (erase_brush == nullptr) {
            erase_brush = CreateSolidBrush(RGB(cfg::CANVAS_DARK.r,
                                               cfg::CANVAS_DARK.g,
                                               cfg::CANVAS_DARK.b));
        }
        SetClassLongPtrW(hwnd, GCLP_HBRBACKGROUND,
                         reinterpret_cast<LONG_PTR>(erase_brush));

        // 标题栏/Alt-Tab 的图标：GLFW 只在注册窗口类时按 "GLFW_ICON"
        // 资源名取一次（E:\source\glfw\src\win32_window.c:1294-1304），
        // 建完窗再用 WM_SETICON 显式换一次，大/小两种尺寸都从 exe 里的
        // 1 号 ICON 组按系统度量加载，不依赖 GLFW 那次是否命中。
        HICON icon_big = static_cast<HICON>(LoadImageW(
            GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
            GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
        HICON icon_small = static_cast<HICON>(LoadImageW(
            GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
        if (icon_big != nullptr) {
            SendMessageW(hwnd, WM_SETICON, ICON_BIG,
                         reinterpret_cast<LPARAM>(icon_big));
        }
        if (icon_small != nullptr) {
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL,
                         reinterpret_cast<LPARAM>(icon_small));
        }
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    if (!gl::LoadAll()) {
        FailToStart(L"OpenGL 函数加载失败");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // 底色先定为画布外色；窗口先不显示，等主循环把第一帧完整画完再显示
    // （与 ImageEditor bootstrap.py:71-81 的做法一致：显示动作和首帧内容同刻）。
    glClearColor(cfg::CANVAS_DARK.r / 255.0f, cfg::CANVAS_DARK.g / 255.0f,
                 cfg::CANVAS_DARK.b / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    bl::ui::Painter painter;
    std::string error;
    if (!painter.init(&error)) {
        FailToStart(L"绘制初始化失败：" + Utf8ToWide(error));
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    if (!bl::ui::Text::Instance().init(&error)) {
        FailToStart(L"字体初始化失败：" + Utf8ToWide(error));
        painter.destroy();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    if (!bl::BreathStyle::init_program(&error)) {
        FailToStart(L"着色器初始化失败：" + Utf8ToWide(error));
        painter.destroy();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    if (!bl::Backdrop::init(&error)) {
        FailToStart(L"画布底初始化失败：" + Utf8ToWide(error));
        painter.destroy();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    bl::ui::Scene scene;
    if (!scene.init(window, &error)) {
        FailToStart(L"界面初始化失败：" + Utf8ToWide(error));
        painter.destroy();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    bl::io::LoadSettings(scene);
    scene.relayout(scene.height());
    if (!scene.has_layers()) scene.fit_view();

    // libvips 契约：任何 vips 调用之前必须先初始化
    // （E:\source\libvips.wiki\Hello-World.md:21、:89 的 VIPS_INIT 顺序）。
    // 放在窗口与场景就绪之后，不让图像库的初始化挡在建窗之前。
    if (vips_init("BreathLoop") != 0) {
        FailToStart(L"libvips 初始化失败");
        painter.destroy();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    if (!options.initial_image.empty()) scene.add_layer(options.initial_image);

    glfwSetWindowUserPointer(window, &scene);
    glfwSetDropCallback(window, OnFileDrop);
    glfwSetKeyCallback(window, OnKey);
    glfwSetCharCallback(window, OnChar);
    glfwSetCursorPosCallback(window, OnCursorPos);
    glfwSetMouseButtonCallback(window, OnMouseButton);
    glfwSetScrollCallback(window, OnScroll);
    glfwSetWindowFocusCallback(window, OnFocus);

    double last = glfwGetTime();
    int shot_frames = 0;
    // 事件循环按 GLFW 文档分工（E:\source\glfw\docs\input.md:39-58）：
    // 连续渲染用 glfwPollEvents；只在输入到达时更新才用 glfwWaitEvents。
    // 呼吸形变是随时间采样的（domain/state.cpp:205-222 每帧按 t 取样），
    // 属于"连续渲染"，所以只要有图层就走 glfwPollEvents，不做任何限流；
    // 只有空画布（没有图层、没有导出）才睡到下一次输入。
    bool first = true;
    bool reveal = true;   // 首帧画完才显示窗口
    // 退出时要写回的窗口矩形：最大化状态下 glfwGetWindowSize 会返回最大化后的
    // 尺寸，所以平时（非最大化）每帧记一份"还原尺寸"，退出时直接用它。
    // 建窗时就已经最大化的情形，起始值取自存档里的还原矩形。
    int restore_x = place.valid ? place.x : 0;
    int restore_y = place.valid ? place.y : 0;
    int restore_w = start_w;
    int restore_h = start_h;
    double next_frame = glfwGetTime();
    const double budget = 1.0 / cfg::MAX_FPS;
    while (!glfwWindowShouldClose(window)) {
        // 呼吸形变是随时间采样的（domain/state.cpp:205-222），有图层就必须连续出帧。
        const bool animating = scene.has_layers() || scene.exporting()
                               || !options.shot_path.empty();
        const double frame_begin = glfwGetTime();
        if (first) {
            glfwPollEvents();
            first = false;
        } else if (animating) {
            // 帧步调用文档指定的"周期性更新"入口（docs/input.md:60-69）。
            // compat.md:214-216 说明 WGL_EXT_swap_control 缺失时 swapInterval
            // 完全无效，所以预算必须自己算；vsync 已经卡够一帧时 left≈0，
            // 直接 poll，不会再多睡半帧。
            const double left = next_frame - frame_begin;
            if (left > 0.0) glfwWaitEventsTimeout(left);
            else glfwPollEvents();
        } else {
            glfwWaitEvents();   // 空画布：睡到下一次输入
        }

        int fbw = 0;
        int fbh = 0;
        glfwGetFramebufferSize(window, &fbw, &fbh);
        if (fbw <= 0 || fbh <= 0) {
            continue;   // 最小化/无客户区：下一轮按 animating 分支等事件再画
        }
        scene.set_size(fbw, fbh);
        glViewport(0, 0, fbw, fbh);

        // 记"还原矩形"：只在正常态（非最大化、非最小化）时更新，退出时写回存档
        if (glfwGetWindowAttrib(window, GLFW_MAXIMIZED) == 0
            && glfwGetWindowAttrib(window, GLFW_ICONIFIED) == 0) {
            int wx = 0;
            int wy = 0;
            int ww = 0;
            int wh = 0;
            glfwGetWindowPos(window, &wx, &wy);
            glfwGetWindowSize(window, &ww, &wh);
            if (ww > 0 && wh > 0) {
                restore_x = wx;
                restore_y = wy;
                restore_w = ww;
                restore_h = wh;
            }
        }

        const double now = glfwGetTime();
        const double dt = now - last;
        last = now;
        scene.update(dt);

        scene.draw_canvas();

        painter.begin(fbw, fbh);
        scene.draw_ui(painter);
        painter.end();

        if (!options.shot_path.empty()) {
            ++shot_frames;
            // 每 20 帧落一张，覆盖同名文件：交互自检时可以边操作边看画面
            if (shot_frames >= 20 && shot_frames % 20 == 0) {
                std::vector<unsigned char> pixels(
                    static_cast<size_t>(fbw) * fbh * 4);
                glReadPixels(0, 0, fbw, fbh, GL_RGBA, GL_UNSIGNED_BYTE,
                             pixels.data());
                std::vector<unsigned char> flipped(pixels.size());
                const size_t row = static_cast<size_t>(fbw) * 4;
                for (int y = 0; y < fbh; ++y) {
                    std::memcpy(flipped.data() + static_cast<size_t>(y) * row,
                                pixels.data()
                                    + static_cast<size_t>(fbh - 1 - y) * row,
                                row);
                }
                VipsImage* image = vips_image_new_from_memory_copy(
                    flipped.data(), flipped.size(), fbw, fbh, 4,
                    VIPS_FORMAT_UCHAR);
                if (image != nullptr) {
                    vips_pngsave(image, options.shot_path.c_str(),
                                 "compression", 3, nullptr);
                    g_object_unref(image);
                }
            }
        }

        glfwSwapBuffers(window);
        if (reveal) {   // 第一帧已经在后缓冲里：此刻才显示窗口
            reveal = false;
            PresentWindow(window, !use_place);
            if (wanted_maximized) {
                // 此刻窗口已可见，glfwMaximizeWindow 走系统原生 SW_MAXIMIZE
                // （win32_window.c:1786-1792），最大化矩形与用户点按钮完全一致。
                glfwMaximizeWindow(window);
                wanted_maximized = false;
            }
        }

        // 用"含 swap 的真实帧耗时"推进下一帧截止时间：够一帧就不再等，
        // 不够才补差额。vsync 有效时不会多睡，失效时也不会跑成 400fps。
        const double spent = glfwGetTime() - frame_begin;
        const double now_end = glfwGetTime();
        next_frame = (spent < budget) ? now_end + (budget - spent) : now_end;
    }

    bl::io::SaveSettings(scene);
    bl::io::SaveWindowPlacement(restore_x, restore_y, restore_w, restore_h,
                                glfwGetWindowAttrib(window, GLFW_MAXIMIZED) != 0);
    painter.destroy();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
