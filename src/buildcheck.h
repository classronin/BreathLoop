// 构建自检：在隐藏窗口里跑一遍 GL 3.3 core 的离屏渲染 + 读回 + libvips 编码，
// 证明"静态链接成一枚 exe"这条链路真的通。只在开发与交付前使用。
#pragma once

#include <string>

// 返回 0 表示全部通过；失败项写进 exe 同目录的 breathloop_buildcheck.log。
int RunBuildCheck();

// 导出等价自检：用与 tools/gen_export_golden.py 完全相同的场景导出一份 WebP，
// 供逐帧比对。asset_path 是测试图，out_base 是输出前缀（不带扩展名）。
int RunExportCheck(const std::string& asset_path, const std::string& out_base);

// 诊断用：把一张图 1:1 画进 FBO 并存成 PNG（附一张解码原图 .src.png）。
int RunDumpFrame(const std::string& asset_path, const std::string& out_png);
