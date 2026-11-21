#include "ui/floating.h"

#include <algorithm>

#include "config.h"

namespace bl::ui {
namespace {

float Clamp(float value, float low, float high) {
    return std::min(std::max(value, low), std::max(low, high));
}

}  // namespace

const char* EdgeCursor(const std::string& edge) {
    if (edge == "left" || edge == "right" || edge == "dock") return "size_h";
    if (edge == "top" || edge == "bottom") return "size_v";
    if (edge == "top_left" || edge == "bottom_right") return "size_nwse";
    if (edge == "top_right" || edge == "bottom_left") return "size_nesw";
    return nullptr;
}

std::array<float, 4> PanelFloat::rect(float win_w, float win_h,
                                      bool collapsed) const {
    if (floating) return {left, bottom, w, h};
    const float width =
        collapsed ? static_cast<float>(cfg::COLLAPSED_W) : w;
    const float x = side == "left" ? 0.0f : win_w - width;
    return {x, 0.0f, width, win_h};
}

bool PanelFloat::contains(float x, float y, float win_w, float win_h,
                          bool collapsed, float pad) const {
    const auto r = rect(win_w, win_h, collapsed);
    return (r[0] - pad <= x && x <= r[0] + r[2] + pad && r[1] - pad <= y
            && y <= r[1] + r[3] + pad);
}

bool PanelFloat::bar_rect(float win_w, float win_h, bool collapsed,
                          float* out) const {
    if (floating) return false;
    const auto r = rect(win_w, win_h, collapsed);
    const float inner =
        collapsed ? r[0]
                  : (side == "left" ? r[0] + r[2] - cfg::COLLAPSED_W : r[0]);
    out[0] = inner;
    out[1] = r[1];
    out[2] = static_cast<float>(cfg::COLLAPSED_W);
    out[3] = r[3];
    return true;
}

bool PanelFloat::contains_header(float x, float y, float win_w, float win_h,
                                 bool collapsed) const {
    if (!floating && collapsed) return false;
    const auto r = rect(win_w, win_h, collapsed);
    if (!(r[0] <= x && x <= r[0] + r[2]
          && r[1] + r[3] - cfg::PANEL_HEADER_H <= y
          && y <= r[1] + r[3])) {
        return false;
    }
    float bar[4] = {0, 0, 0, 0};
    if (bar_rect(win_w, win_h, collapsed, bar)) {
        if (bar[0] <= x && x <= bar[0] + bar[2]) return false;
    }
    return true;
}

void PanelFloat::clamp_to(float win_w, float win_h) {
    left = Clamp(left, 0.0f, win_w - w);
    bottom = Clamp(bottom, 0.0f, win_h - h);
}

void PanelFloat::begin_drag(float x, float y) {
    has_press = true;
    dragging = false;
    press_x = x;
    press_y = y;
    press_left = left;
    press_bottom = bottom;
}

bool PanelFloat::drag_to(float x, float y) {
    if (!has_press) return false;
    if (!dragging) {
        if (std::abs(x - press_x) + std::abs(y - press_y) < cfg::PANEL_DRAG_MIN)
            return false;
        dragging = true;
    }
    left = press_left + (x - press_x);
    bottom = press_bottom + (y - press_y);
    return true;
}

bool PanelFloat::end_drag() {
    const bool dragged = dragging;
    has_press = false;
    dragging = false;
    if (dragged) last_header_click = 0.0;   // 拖动不算点击
    return dragged;
}

std::string PanelFloat::resize_hit(float x, float y, float win_w, float win_h,
                                   bool collapsed) const {
    if (floating) {
        const float edge = static_cast<float>(cfg::PANEL_RESIZE_EDGE);
        if (!contains(x, y, win_w, win_h, false, edge)) return std::string();
        const auto r = rect(win_w, win_h, false);
        const bool near_left = x <= r[0] + edge;
        const bool near_right = x >= r[0] + r[2] - edge;
        const bool near_bottom = y <= r[1] + edge;
        const bool near_top = y >= r[1] + r[3] - edge;
        if (near_top && near_left) return "top_left";
        if (near_top && near_right) return "top_right";
        if (near_bottom && near_left) return "bottom_left";
        if (near_bottom && near_right) return "bottom_right";
        if (near_left) return "left";
        if (near_right) return "right";
        if (near_top) return "top";
        if (near_bottom) return "bottom";
        return std::string();
    }
    float bar[4] = {0, 0, 0, 0};
    if (!bar_rect(win_w, win_h, collapsed, bar)) return std::string();
    if (!(bar[0] <= x && x <= bar[0] + bar[2] && bar[1] <= y
          && y <= bar[1] + bar[3])) {
        return std::string();
    }
    return collapsed ? "bar" : "dock";
}

void PanelFloat::begin_resize(const std::string& edge, float x, float y) {
    has_resize = true;
    resizing = false;
    resize_edge = edge;
    resize_x = x;
    resize_y = y;
    resize_left = left;
    resize_bottom = bottom;
    resize_w = w;
    resize_h = h;
}

bool PanelFloat::resize_to(float x, float y, float win_w, float win_h,
                           float max_w) {
    if (!has_resize) return false;
    if (!resizing) {
        if (std::abs(x - resize_x) + std::abs(y - resize_y) < cfg::PANEL_DRAG_MIN)
            return false;
        resizing = true;
    }
    if (resize_edge == "dock") {
        const float width = side == "left" ? x : win_w - x;
        w = Clamp(width, static_cast<float>(cfg::PANEL_MIN_W), max_w);
        return true;
    }

    float new_left = resize_left;
    float new_bottom = resize_bottom;
    float new_w = resize_w;
    float new_h = resize_h;
    const std::string& edge = resize_edge;
    if (edge.find("left") != std::string::npos) {
        const float right = resize_left + resize_w;
        new_left = Clamp(x, 0.0f, right - cfg::PANEL_MIN_W);
        new_w = right - new_left;
    } else if (edge.find("right") != std::string::npos) {
        new_w = Clamp(x - resize_left, static_cast<float>(cfg::PANEL_MIN_W),
                      win_w - resize_left);
    }
    if (edge.find("top") != std::string::npos) {
        const float top =
            Clamp(y, resize_bottom + cfg::PANEL_MIN_FLOAT_H, win_h);
        new_h = top - resize_bottom;
    } else if (edge.find("bottom") != std::string::npos) {
        const float top = resize_bottom + resize_h;
        new_bottom = Clamp(y, 0.0f, top - cfg::PANEL_MIN_FLOAT_H);
        new_h = top - new_bottom;
    }
    left = new_left;
    bottom = new_bottom;
    w = new_w;
    h = new_h;
    return true;
}

bool PanelFloat::end_resize() {
    const bool resized = resizing;
    has_resize = false;
    resizing = false;
    return resized;
}

bool PanelFloat::take_double_click(double now, double threshold) {
    const double last = last_header_click;
    last_header_click = now;
    return last != 0.0 && now - last <= threshold;
}

}  // namespace bl::ui
