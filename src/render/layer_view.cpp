#include "render/layer_view.h"

#include <cmath>

#include "config.h"

namespace bl {

LayerView::LayerView(Layer* layer_, Asset* asset_)
    : layer(layer_), asset(asset_) {
    x = layer->x;
    y = layer->y;
    scale = layer->scale;
    scale_x = layer->scale_x();
    scale_y = layer->scale_y();
    visible = layer->is_visible_chain();
    style.img_w = static_cast<double>(layer->w);
    style.img_h = static_cast<double>(layer->h);
}

void LayerView::sync(bool deform) {
    x = layer->x;
    y = layer->y;
    scale = layer->scale;
    scale_x = layer->scale_x();
    scale_y = layer->scale_y();
    rot = layer->rot;
    visible = layer->is_visible_chain();
    style.views = layer->bbox_views();
    style.deform = deform;
}

void LayerView::release() { style.mask_texture.reset(); }

void LayerView::world_corners(double* p) const {
    const double hw = layer->w / 2.0 * scale_x;
    const double hh = layer->h / 2.0 * scale_y;
    const double c = std::cos(rot);
    const double s = std::sin(rot);
    // 图层坐标（y 向下）的四角：左上 (-hw,-hh)、右上 (+hw,-hh)、
    // 右下 (+hw,+hh)、左下 (-hw,+hh)，再旋转到世界坐标。
    const double lx[4] = {-hw, hw, hw, -hw};
    const double ly[4] = {hh, hh, -hh, -hh};   // uv(0,0) 在图像顶行 = 世界 y 的 +hh
    for (int i = 0; i < 4; ++i) {
        p[i * 2] = x + lx[i] * c - ly[i] * s;
        p[i * 2 + 1] = y + lx[i] * s + ly[i] * c;
    }
}

void LayerView::world_quad(double* left, double* bottom, double* right,
                           double* top) const {
    const double w = static_cast<double>(layer->w);
    const double h = static_cast<double>(layer->h);
    *left = x - (w / 2.0) * scale_x;
    *right = x + (w / 2.0) * scale_x;
    *bottom = y - (h / 2.0) * scale_y;
    *top = y + (h / 2.0) * scale_y;
}

void LayerViews::rebuild(const Document& doc, AssetStore& assets) {
    release_all();
    views_.clear();
    by_uid_.clear();
    for (Layer* layer : doc.flatten()) {
        Asset& asset = assets.get(layer->asset_key);
        views_.push_back(std::make_unique<LayerView>(layer, &asset));
        by_uid_[layer->uid] = views_.back().get();
    }
}

LayerView* LayerViews::get(int uid) {
    auto it = by_uid_.find(uid);
    return it == by_uid_.end() ? nullptr : it->second;
}

void LayerViews::sync(bool deform) {
    for (auto& view : views_) view->sync(deform);
}

void LayerViews::release_all() {
    for (auto& view : views_) view->release();
}

std::vector<LayerView*> LayerViews::top_down() const {
    std::vector<LayerView*> out;
    out.reserve(views_.size());
    for (auto it = views_.rbegin(); it != views_.rend(); ++it) {
        out.push_back(it->get());
    }
    return out;
}

}  // namespace bl
