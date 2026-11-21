// 设置落盘：exe 同目录的 breathloop_settings.json。
//
// 只存会变的东西：面板停靠/浮动与尺寸、卡片折叠、导出参数、帧率与视图、
// 窗口位置/大小与最大化状态。
// 读到坏的/缺的字段一律退回默认（旧版设置文件不会拦住启动）。
#pragma once

namespace bl::ui {
class Scene;
}

namespace bl::io {

// 窗口摆放：窗口矩形（屏幕坐标）+ 是否最大化。valid == false 表示还没有存过，
// 调用方按默认尺寸居中创建。maximized 为 true 时 x/y/w/h 记的是"还原后"的矩形。
struct WindowPlacement {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    bool maximized = false;
    bool valid = false;
};

void LoadSettings(ui::Scene& scene);
void SaveSettings(ui::Scene& scene);

WindowPlacement LoadWindowPlacement();
void SaveWindowPlacement(int x, int y, int w, int h, bool maximized);

}  // namespace bl::io
