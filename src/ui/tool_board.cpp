#include "ui/tool_board.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "config.h"
#include "ui/painter.h"
#include "ui/scene.h"

namespace bl::ui {
namespace {

// 一行两块：半径 / 羽化，各 300px（窗口太窄时按可用宽度等比缩）。
constexpr float kSliderW = 300.0f;
constexpr float kGap = 10.0f;
constexpr float kPad = 12.0f;
constexpr float kWidth = kPad * 2.0f + kSliderW * 2.0f + kGap;   // 634
constexpr float kHeight = 34.0f;
constexpr float kBottom = 10.0f;   // 贴着窗口底边
constexpr float kRowH = Slider::kH;

}  // namespace

void ToolBoard::init(Scene* scene) {
    scene_ = scene;
    if (scene_ == nullptr) return;

    // 滑块上写"半径"，内部仍存直径（2–500）：两者差一个因数 2，界面上给人看的
    // 数字必须和光标圆环的半径一致，所以显示/回写都在这里换算。
    radius_.label = "半径";
    radius_.vmin = 1.0f;
    radius_.vmax = 250.0f;
    radius_.step = 1.0f;
    radius_.format = "{:.0f}";
    radius_.value = static_cast<float>(scene_->state.eraser_diameter) * 0.5f;
    radius_.on_change = [this](float value) {
        const int d = static_cast<int>(std::lround(value)) * 2;
        scene_->state.eraser_diameter = d < 2 ? 2 : (d > 500 ? 500 : d);
    };

    feather_.label = "羽化";
    feather_.vmin = 0.0f;
    feather_.vmax = 100.0f;
    feather_.step = 1.0f;
    feather_.format = "{:.0f}";
    feather_.value = static_cast<float>(scene_->state.eraser_feather);
    feather_.on_change = [this](float value) {
        scene_->state.eraser_feather = static_cast<int>(std::lround(value));
    };

    layout();
}

bool ToolBoard::visible() const {
    if (scene_ == nullptr) return false;
    // 导出时不给动手：逐帧取当前文档，中途改像素会让帧之间不一致
    return scene_->eraser_on() && !scene_->exporting();
}

void ToolBoard::layout() {
    if (scene_ == nullptr) return;
    const auto edges = scene_->canvas_edges();
    const float center = static_cast<float>((edges.first + edges.second) * 0.5);
    const float avail = static_cast<float>(scene_->width()) - kPad * 2.0f - 16.0f;
    const float slider_w =
        std::max(80.0f, std::min(kSliderW, (avail - kGap) / 2.0f));
    w_ = kPad * 2.0f + slider_w * 2.0f + kGap;
    h_ = kHeight;
    x_ = center - w_ * 0.5f;
    x_ = std::max(8.0f, std::min(x_, static_cast<float>(scene_->width()) - w_ - 8.0f));
    y_ = kBottom;

    const float row_y = y_ + 6.0f;
    radius_.x = x_ + kPad;
    radius_.y = row_y;
    radius_.w = slider_w;
    radius_.h = kRowH;

    feather_.x = radius_.x + slider_w + kGap;
    feather_.y = row_y;
    feather_.w = slider_w;
    feather_.h = kRowH;
}

bool ToolBoard::contains(float x, float y) const {
    return x >= x_ && x <= x_ + w_ && y >= y_ && y <= y_ + h_;
}

bool ToolBoard::press(float x, float y) {
    if (!visible()) return false;
    layout();
    if (!contains(x, y)) return false;
    if (radius_.press(x, y)) return true;
    if (feather_.press(x, y)) return true;
    // 点在板上但没点到滑块：也吃掉，别让这一下变成擦除
    return true;
}

void ToolBoard::drag(float x, float y) {
    if (radius_.drag) radius_.drag_to(x, y);
    if (feather_.drag) feather_.drag_to(x, y);
}

void ToolBoard::release(float x, float y) {
    radius_.release(x, y);
    feather_.release(x, y);
}

void ToolBoard::draw(Painter& painter) {
    if (!visible()) return;
    layout();

    // 滑块只在没拖动时跟随 state：拖动中写回会把正在拖的值弹回去
    if (!radius_.drag)
        radius_.set_value(static_cast<float>(scene_->state.eraser_diameter) * 0.5f);
    if (!feather_.drag)
        feather_.set_value(static_cast<float>(scene_->state.eraser_feather));

    painter.rect(x_, y_, w_, h_, cfg::PANEL_BG);
    painter.rect(x_, y_, w_, 1.0f, cfg::PANEL_BORDER);
    painter.rect(x_, y_ + h_ - 1.0f, w_, 1.0f, cfg::PANEL_BORDER);
    painter.rect(x_, y_, 1.0f, h_, cfg::PANEL_BORDER);
    painter.rect(x_ + w_ - 1.0f, y_, 1.0f, h_, cfg::PANEL_BORDER);

    radius_.draw(painter);
    feather_.draw(painter);
}

}  // namespace bl::ui
