#include "ui/status.h"

#include "config.h"
#include "ui/painter.h"
#include "ui/scene.h"

namespace bl::ui {

void CenterStatus::init(Scene* scene) { scene_ = scene; }

void CenterStatus::set(const std::string& value) { text = value; }

void CenterStatus::relayout(int height) {
    if (scene_ == nullptr) return;
    const auto edges = scene_->canvas_edges();
    x_ = static_cast<float>((edges.first + edges.second) / 2.0);
    y_ = static_cast<float>(height - 14);
}

void CenterStatus::draw(Painter& painter) {
    if (text.empty()) return;
    // 1px 黑色外描边：先用黑在八个方向各画一遍，再把白字压在上面。
    // 描边朝外，不垫背景条，浅色画布上也能读。
    const cfg::FontRole role = cfg::FontRole::Body;
    const float offsets[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1},
                                 {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    for (const auto& offset : offsets) {
        painter.text(text, x_ + offset[0], y_ + offset[1], role, false,
                     cfg::FromRgb(0, 0, 0), Align::kCenter);
    }
    painter.text(text, x_, y_, role, false, cfg::STATUS_TEXT, Align::kCenter);
}

}  // namespace bl::ui
