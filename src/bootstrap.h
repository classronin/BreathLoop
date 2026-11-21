// 组合根：把窗口、文档、场景、面板装到一起，然后进入消息循环。
// 这是全工程唯一知道"系统由哪些部件组成"的地方。
#pragma once

#include <string>

struct AppOptions {
    std::string initial_image;  // 启动时要载入的图片路径（argv[1]，可为空）
    std::string shot_path;      // 非空时：稳定若干帧后把窗口画面存成 PNG 再退出（开发自检用）
};

// 返回进程退出码。
int RunApp(const AppOptions& options);
