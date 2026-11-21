#include "ui/overlay.h"

#include <algorithm>
#include <cmath>

#include "config.h"
#include "ui/painter.h"
#include "ui/scene.h"

namespace bl::ui {
namespace {

// 每个 Region 一份手柄表：数量固定，坐标每次现算。
std::array<Handle, 5> g_handles{};

std::pair<double, double> Screen(const Scene& scene, const Layer& layer,
                                 double px, double py) {
    const auto world = layer.to_world(px, py);
    return scene.world_to_window(world.first, world.second);
}

// BBOX 唯一投影口：局部（区域坐标）→ 区域旋转 → 图层图像坐标 → 图层矩阵 → 屏幕。
// 框和五个手柄都必须从这里出去，画布/缩放/旋转/各向异性怎么变都只算一次。
std::pair<double, double> RegionScreen(const Scene& scene, const Layer& layer,
                                       const Region& region, double lx,
                                       double ly) {
    const auto offset = region.to_world(lx, ly);
    return Screen(scene, layer, region.cx + offset.first,
                  region.cy + offset.second);
}

void DrawAnts(Painter& painter,
              const std::vector<std::pair<double, double>>& points,
              double shift, double period, cfg::Rgba color) {
    double walked = 0.0;
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        const double x0 = points[i].first;
        const double y0 = points[i].second;
        const double x1 = points[i + 1].first;
        const double y1 = points[i + 1].second;
        const double dx = x1 - x0;
        const double dy = y1 - y0;
        const double length = std::sqrt(dx * dx + dy * dy);
        if (length > 1e-3) {
            const double ux = dx / length;
            const double uy = dy / length;
            double start = std::fmod(shift + walked, period);
            while (start < length) {
                const double end =
                    std::min(start + cfg::LASSO_ANT_DASH, length);
                painter.line(static_cast<float>(x0 + ux * start),
                             static_cast<float>(y0 + uy * start),
                             static_cast<float>(x0 + ux * end),
                             static_cast<float>(y0 + uy * end),
                             static_cast<float>(cfg::LASSO_THICKNESS), color);
                start += period;
            }
        }
        walked += length;
    }
}

void DrawLasso(Scene& scene, Painter& painter) {
    // 路径存的是画布世界坐标，所以绘制只过一次 world_to_window，
    // 不再依赖任何图层（图层只决定像素操作落到谁身上）。
    if (scene.is_lasso_mode() && !scene.lasso_path.empty()) {
        std::vector<std::pair<double, double>> points;
        points.reserve(scene.lasso_path.size());
        for (const auto& p : scene.lasso_path) {
            const auto screen = scene.world_to_window(p.first, p.second);
            points.emplace_back(screen.first, screen.second);
        }
        for (size_t i = 0; i + 1 < points.size(); ++i) {
            painter.line(static_cast<float>(points[i].first),
                         static_cast<float>(points[i].second),
                         static_cast<float>(points[i + 1].first),
                         static_cast<float>(points[i + 1].second),
                         static_cast<float>(cfg::LASSO_THICKNESS),
                         cfg::LASSO_LINE);
        }
        // 从最后一个点连到光标，随时看得出下一段会落在哪
        if (!points.empty()) {
            painter.line(static_cast<float>(points.back().first),
                         static_cast<float>(points.back().second),
                         static_cast<float>(scene.mouse_x),
                         static_cast<float>(scene.mouse_y),
                         static_cast<float>(cfg::LASSO_THICKNESS),
                         cfg::LASSO_LINE);
        }
        for (const auto& p : points) {
            painter.circle(static_cast<float>(p.first),
                           static_cast<float>(p.second),
                           static_cast<float>(cfg::LASSO_DOT_R),
                           cfg::LASSO_DOT);
        }
    }

    // 已闭合的套索：同样只画一条，颜色按"目标图层是否被选中"分深浅
    if (!scene.is_lasso_mode() && scene.lasso_closed() && !scene.lasso_path.empty()) {
        const double shift = std::fmod(scene.state.time * cfg::LASSO_ANT_SPEED,
                                       cfg::LASSO_ANT_DASH + cfg::LASSO_ANT_GAP);
        std::vector<std::pair<double, double>> points;
        points.reserve(scene.lasso_path.size() + 1);
        for (const auto& p : scene.lasso_path) {
            const auto screen = scene.world_to_window(p.first, p.second);
            points.emplace_back(screen.first, screen.second);
        }
        if (!points.empty()) points.push_back(points.front());
        Layer* target = scene.lasso_target();
        const cfg::Rgba color =
            target != nullptr && scene.selection.has_layer(target)
                ? cfg::LASSO_ANT_SELECTED
                : cfg::LASSO_ANT;
        DrawAnts(painter, points, shift,
                 cfg::LASSO_ANT_DASH + cfg::LASSO_ANT_GAP, color);
    }
}

void DrawBboxes(Scene& scene, Painter& painter) {
    if (scene.bbox_hidden) return;
    for (Layer* layer : scene.state.doc->flatten()) {
        if (!layer->is_visible_chain()) continue;
        for (const auto& region : layer->bboxes) {
            if (!region->visible) continue;   // 眼睛关掉的区域连框都不画
            const cfg::Rgba color = scene.region_color(region.get());
            const bool selected = scene.selection.has_region(region.get());
            const float alpha =
                (selected ? cfg::BBOX_OPACITY_SELECTED : cfg::BBOX_OPACITY)
                / 255.0f;
            // 框不能画成"屏幕空间轴对齐椭圆"：区域只有在图层图像坐标里才是真椭圆，
            // 图层一旋转、各向异性一拉，投影到屏幕就不再是椭圆。所以按同一段圆弧
            // 取点，每点走 RegionScreen，与五个手柄完全同一条链。
            const int kRingSegments = 48;
            std::vector<std::pair<float, float>> ring;
            ring.reserve(static_cast<size_t>(kRingSegments));
            for (int i = 0; i < kRingSegments; ++i) {
                const double a = 6.283185307179586 * i / kRingSegments;
                const auto p = RegionScreen(scene, *layer, *region,
                                            std::cos(a) * region->rx,
                                            std::sin(a) * region->ry);
                ring.emplace_back(static_cast<float>(p.first),
                                  static_cast<float>(p.second));
            }
            painter.polygon(ring, color, alpha);
        }
    }

    Layer* layer = scene.selected_layer();
    Region* region = scene.selection.sole_region();
    // 眼睛关掉的区域：框、五个手柄、旋转点一律不画（参数面板仍然可选中可灰显）
    if (region == nullptr || layer == nullptr || !layer->is_visible_chain()
        || !region->visible) {
        return;
    }

    const cfg::Rgba color = scene.region_color(region);
    const auto& handles = HandleList(*region, RegionUpScale(scene, *layer, *region));

    std::array<std::pair<double, double>, 5> screen_points{};
    for (size_t i = 0; i < handles.size(); ++i) {
        screen_points[i] = RegionScreen(scene, *layer, *region, handles[i].x,
                                        handles[i].y);
    }

    painter.line(static_cast<float>(screen_points[1].first),
                 static_cast<float>(screen_points[1].second),
                 static_cast<float>(screen_points[4].first),
                 static_cast<float>(screen_points[4].second), 1.0f, color);

    for (size_t i = 0; i < handles.size(); ++i) {
        const bool rotate = handles[i].name[0] == 'r';
        painter.circle(static_cast<float>(screen_points[i].first),
                       static_cast<float>(screen_points[i].second),
                       static_cast<float>(cfg::HANDLE_RADIUS),
                       rotate ? cfg::FromRgb(255, 255, 255) : color);
    }
}

// 画布边界不再由 UI 层画：它是画布底自己的 1px 边（render/backdrop.cpp 的
// u_frame 分支），和棋盘格同一对坐标、同一像素空间，职责归画布一处。

void DrawGuides(Scene& scene, Painter& painter) {
    if (!scene.guide_x && !scene.guide_y) return;
    const Rect canvas = scene.canvas_rect();
    const auto a = scene.world_to_window(canvas.x0, canvas.y0);
    const auto b = scene.world_to_window(canvas.x1, canvas.y1);
    const float left = static_cast<float>(std::min(a.first, b.first));
    const float right = static_cast<float>(std::max(a.first, b.first));
    const float bottom = static_cast<float>(std::min(a.second, b.second));
    const float top = static_cast<float>(std::max(a.second, b.second));
    if (scene.guide_x) {
        const auto p = scene.world_to_window(scene.guide_wx, 0.0);
        painter.line(static_cast<float>(p.first), bottom,
                     static_cast<float>(p.first), top, 1.0f,
                     cfg::GUIDE_LINE);
    }
    if (scene.guide_y) {
        const auto p = scene.world_to_window(0.0, scene.guide_wy);
        painter.line(left, static_cast<float>(p.second), right,
                     static_cast<float>(p.second), 1.0f,
                     cfg::GUIDE_LINE);
    }
}

void DrawTransform(Scene& scene, Painter& painter) {
    Layer* layer = scene.transform_layer;
    if (!scene.transform_mode || layer == nullptr) return;
    // 四个角必须画在"图像真实内容"的四个角上：
    // - world_rect() 是旋转后的轴对齐包围盒，一旋转就比内容大一圈（旧 bug）。
    // - 套索抠出来的小块四周带透明补边 pad，手柄要往里缩 pad，否则角点在补边上。
    // 命中（TransformHandleAt）用的是同一组点。
    const double pad = static_cast<double>(layer->pad);
    const double w = std::max(1.0, static_cast<double>(layer->w) - pad * 2.0);
    const double h = std::max(1.0, static_cast<double>(layer->h) - pad * 2.0);
    const double lx[4] = {pad, pad + w, pad + w, pad};
    const double ly[4] = {pad, pad, pad + h, pad + h};
    std::array<std::pair<float, float>, 4> corners{};
    for (int i = 0; i < 4; ++i) {
        const auto world = layer->to_world(lx[i], ly[i]);
        const auto screen = scene.world_to_window(world.first, world.second);
        corners[static_cast<size_t>(i)] = {static_cast<float>(screen.first),
                                           static_cast<float>(screen.second)};
    }
    for (int i = 0; i < 4; ++i) {
        const auto& a = corners[static_cast<size_t>(i)];
        const auto& b = corners[static_cast<size_t>((i + 1) % 4)];
        painter.line(a.first, a.second, b.first, b.second, 1.0f,
                     cfg::GUIDE_LINE);
    }
    for (const auto& p : corners) {
        painter.rect(p.first - 4.0f, p.second - 4.0f, 8.0f, 8.0f,
                     cfg::CANVAS_EDGE);
    }
}

}  // namespace

// 橡皮光标：屏幕上的等直径圆环（直径是图层图像像素，跟着缩放一起变）。
// 只画环，不画实心，免得挡住要擦的位置。
void DrawEraserRing(Scene& scene, Painter& painter) {
    if (!scene.eraser_on()) return;
    const Layer* layer = scene.selected_layer();
    double screen_scale = scene.view.scale;
    if (layer != nullptr) screen_scale *= layer->scale;
    const double radius = std::max(1.0, scene.state.eraser_diameter * 0.5)
                          * std::max(screen_scale, 1e-6);
    const float segments = 48.0f;
    const float pi = 3.14159265358979323846f;
    float px = static_cast<float>(scene.mouse_x);
    float py = static_cast<float>(scene.mouse_y);
    const cfg::Rgba ring = cfg::FromRgb(230, 240, 255);
    for (int i = 0; i < static_cast<int>(segments); ++i) {
        const float a0 = 2.0f * pi * static_cast<float>(i) / segments;
        const float a1 = 2.0f * pi * static_cast<float>(i + 1) / segments;
        painter.line(px + static_cast<float>(radius) * std::cos(a0),
                     py + static_cast<float>(radius) * std::sin(a0),
                     px + static_cast<float>(radius) * std::cos(a1),
                     py + static_cast<float>(radius) * std::sin(a1), 1.4f,
                     ring, 0.9f);
    }
    painter.circle(px, py, 1.5f, ring, 0.9f);
}

// 区域局部 +y 方向 1 单位对应多少屏幕像素（含图层矩阵与视图缩放）
double RegionUpScale(const Scene& scene, const Layer& layer,
                     const Region& region) {
    const auto a = RegionScreen(scene, layer, region, 0.0, 0.0);
    const auto b = RegionScreen(scene, layer, region, 0.0, 1.0);
    const double dx = b.first - a.first;
    const double dy = b.second - a.second;
    const double length = std::sqrt(dx * dx + dy * dy);
    return std::max(length, 1e-6);
}

const std::array<Handle, 5>& HandleList(const Region& region, double up_scale) {
    const double gap = cfg::HANDLE_ROT_GAP / std::max(up_scale, 1e-6);
    // 旋转点放在椭圆上方：局部 +y 在屏幕上朝下，所以“上”= +ry + gap，
    // 连线接到同一个方向的手柄（handles[1]）上。
    g_handles[0] = Handle{"top", 0.0, -region.ry};
    g_handles[1] = Handle{"bottom", 0.0, region.ry};
    g_handles[2] = Handle{"left", -region.rx, 0.0};
    g_handles[3] = Handle{"right", region.rx, 0.0};
    g_handles[4] = Handle{"rotate", 0.0, region.ry + gap};
    return g_handles;
}

void DrawOverlay(Scene& scene, Painter& painter) {
    if (!scene.x_hidden) {
        DrawLasso(scene, painter);
        DrawBboxes(scene, painter);
        DrawTransform(scene, painter);
    }
    DrawGuides(scene, painter);
    DrawEraserRing(scene, painter);
}

}  // namespace bl::ui
