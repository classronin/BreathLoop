// 面板停靠与浮动状态：所在边、尺寸、标题拖动、边缘缩放。
//
// 坐标与界面一致：左下角原点、y 向上；矩形可直接用于 scissor。
#pragma once

#include <array>
#include <string>

namespace bl::ui {

// 命中的边 → 光标名（SceneCursors 认这几个名字）
const char* EdgeCursor(const std::string& edge);

struct PanelFloat {
    PanelFloat() = default;
    PanelFloat(std::string dock_side, float default_width)
        : side(std::move(dock_side)),
          default_w(default_width),
          w(default_width) {}

    std::string side = "left";    // 停靠边；浮动时记住上次停靠的边
    float default_w = 300.0f;
    float w = 300.0f;
    float h = 0.0f;
    float left = 0.0f;
    float bottom = 0.0f;
    bool floating = false;
    double last_header_click = 0.0;
    bool dragging = false;
    bool resizing = false;

    bool has_press = false;
    float press_x = 0.0f, press_y = 0.0f, press_left = 0.0f, press_bottom = 0.0f;

    bool has_resize = false;
    std::string resize_edge;
    float resize_x = 0.0f, resize_y = 0.0f;
    float resize_left = 0.0f, resize_bottom = 0.0f;
    float resize_w = 0.0f, resize_h = 0.0f;

    // ---- 几何 ----
    std::array<float, 4> rect(float win_w, float win_h,
                              bool collapsed) const;   // left, bottom, w, h
    bool contains(float x, float y, float win_w, float win_h, bool collapsed,
                  float pad = 0.0f) const;
    // 停靠时那条 16px 的竖条；浮动时返回 false
    bool bar_rect(float win_w, float win_h, bool collapsed,
                  float* out) const;
    bool contains_header(float x, float y, float win_w, float win_h,
                         bool collapsed) const;
    void clamp_to(float win_w, float win_h);

    // ---- 标题拖动 ----
    void begin_drag(float x, float y);
    bool drag_to(float x, float y);
    bool end_drag();

    // ---- 边缘缩放 ----
    std::string resize_hit(float x, float y, float win_w, float win_h,
                           bool collapsed) const;
    void begin_resize(const std::string& edge, float x, float y);
    bool resize_to(float x, float y, float win_w, float win_h, float max_w);
    bool end_resize();

    // ---- 双击 ----
    bool take_double_click(double now, double threshold);
};

}  // namespace bl::ui
