#include "render/framing.h"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace bl {
namespace {

struct RectF {
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
};

}  // namespace

ContentFrame content_frame(const Document& doc, AssetStore& assets) {
    std::vector<RectF> rects;
    std::vector<RectF> fallback;
    for (Layer* layer : doc.flatten()) {
        if (!layer->is_visible_chain()) continue;
        const Asset& asset = assets.get(layer->asset_key);
        if (asset.content_w > 0 && asset.content_h > 0) {
            const auto a = layer->to_world(asset.content_x, asset.content_y);
            const auto b = layer->to_world(asset.content_x + asset.content_w,
                                           asset.content_y + asset.content_h);
            rects.push_back(RectF{a.first, a.second, b.first, b.second});
        }
        // 全部透明时退回整图范围，至少让导出流程能跑完。
        const Rect world = layer->world_rect();
        fallback.push_back(RectF{world.x0, world.y0, world.x1, world.y1});
    }
    if (rects.empty()) rects = fallback;
    if (rects.empty()) throw std::runtime_error("没有可见图层，无法导出");

    double x0 = rects.front().x0;
    double y0 = rects.front().y0;
    double x1 = rects.front().x1;
    double y1 = rects.front().y1;
    for (const RectF& rect : rects) {
        x0 = std::min(x0, rect.x0);
        y0 = std::min(y0, rect.y0);
        x1 = std::max(x1, rect.x1);
        y1 = std::max(y1, rect.y1);
    }
    return ContentFrame{x0, y0, x1 - x0, y1 - y0};
}

}  // namespace bl
