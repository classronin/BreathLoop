#include "ui/input.h"

#include <cmath>

#include <GLFW/glfw3.h>

#include "config.h"
#include "ui/layer_panel.h"
#include "ui/overlay.h"
#include "ui/scene.h"

namespace bl::ui::input {
namespace {

void PushOnce(Scene& scene) {
    if (!scene.drag_undo_pushed) {
        scene.push_undo();
        scene.drag_undo_pushed = true;
    }
}

std::pair<double, double> MouseWorld(Scene& scene) {
    return scene.window_to_world(scene.mouse_x, scene.mouse_y);
}

// 套索操作（抠图 / 剪走 / Del）的目标：只有"套索已闭合生效"时才算，目标图层
// 由 Scene 现推（选中且可见 → 否则最上面的可见层）；有 BBOX 选中时不吃套索。
Layer* LassoTarget(Scene& scene) {
    if (scene.selected_region() != nullptr) return nullptr;
    if (!scene.lasso_closed()) return nullptr;
    return scene.lasso_target();
}

// 工具模式下的复制/粘贴/抠图禁令：橡皮模式一律禁；套索未闭合也禁。
// 返回 true = 这一次按键被策略吃掉（已写了状态栏提示）。
bool BlockedByTool(Scene& scene, const char* what) {
    if (scene.eraser_on()) {
        scene.set_status(std::string("橡皮模式下不可用：") + what
                         + "（按 A 切到套索）");
        return true;
    }
    if (scene.is_lasso_mode() && !scene.lasso_closed()) {
        scene.set_status(std::string("套索未闭合，不可用：") + what
                         + "（双击或回车闭合）");
        return true;
    }
    return false;
}

void CopySelection(Scene& scene) {
    Layer* layer = nullptr;
    Region* region = scene.selected_region();
    if (region == nullptr) layer = scene.selected_layer();
    if (region == nullptr && layer == nullptr) {
        const auto world = MouseWorld(scene);
        Layer* hit_layer = nullptr;
        Region* hit_region = nullptr;
        if (BboxAt(scene, world.first, world.second, &hit_layer, &hit_region)) {
            layer = hit_layer;
            region = hit_region;
        } else {
            layer = LayerAt(scene, world.first, world.second);
        }
    }
    if (region != nullptr) {
        scene.clipboard_region = std::make_unique<Region>(*region);
        scene.clipboard_layer.reset();
        scene.set_status("已复制 BBOX");
        return;
    }
    if (layer == nullptr) {
        scene.set_status("未找到可复制的内容");
        return;
    }
    scene.clipboard_layer = std::make_unique<LayerClip>(layer->copy_snapshot());
    scene.clipboard_region.reset();
    scene.set_status("已复制 " + layer->name);
}

void PasteBbox(Scene& scene, const Region& source) {
    if (!InCanvas(scene, scene.mouse_x) || scene.mouse_y < 0.0
        || scene.mouse_y > static_cast<double>(scene.height())) {
        scene.set_status("请把鼠标移到画布上再粘贴");
        return;
    }
    const auto world = MouseWorld(scene);
    Layer* layer = LayerAt(scene, world.first, world.second);
    if (layer == nullptr && scene.selected_region() != nullptr) {
        layer = scene.state.doc->layer_of(scene.selected_region());
    }
    if (layer == nullptr) layer = scene.selected_layer();
    if (layer == nullptr) {
        scene.set_status("请把鼠标移到要粘贴的图层上");
        return;
    }
    scene.push_undo();
    const auto local = layer->from_world(world.first, world.second);
    Region* region = scene.state.add_bbox(layer, local.first, local.second);
    if (region == nullptr) {
        scene.undo_stack.pop_back();
        scene.set_status("BBOX 数量已达上限");
        return;
    }
    region->copy_params_from(source);
    scene.sync_views();
    scene.layer_panel->rebuild();
    scene.select_region(layer, region);
    scene.set_status("已粘贴 " + region->name);
}

bool ZoomView(Scene& scene, double x, double y, double dy) {
    const double old_scale = scene.view.scale;
    const double new_scale = std::min(
        std::max(old_scale * std::pow(cfg::WHEEL_VIEW, dy), cfg::ZOOM_MIN),
        cfg::ZOOM_MAX);
    if (new_scale == old_scale) return true;
    const auto stage = scene.window_to_stage(x, y);
    const auto world = scene.stage_to_world(stage.first, stage.second);
    scene.view.scale = new_scale;
    scene.view.x = stage.first - world.first * new_scale;
    scene.view.y = stage.second - world.second * new_scale;
    return true;
}

}  // namespace

std::pair<double, double> CanvasEdges(const Scene& scene) {
    double left = 0.0;
    double right = static_cast<double>(scene.width());
    for (const std::string& which : {"param", "layer"}) {
        const auto& f = (which == "param") ? scene.panel_float("param")
                                           : scene.panel_float("layer");
        if (f.floating) continue;
        const double width = scene.is_collapsed(f.side)
                                 ? static_cast<double>(cfg::COLLAPSED_W)
                                 : static_cast<double>(f.w);
        if (f.side == "left") {
            left = std::max(left, width);
        } else {
            right = std::min(right, scene.width() - width);
        }
    }
    return {left, right};
}

bool InCanvas(const Scene& scene, double x) {
    const auto edges = CanvasEdges(scene);
    return edges.first <= x && x <= edges.second;
}

Layer* LayerAt(Scene& scene, double wx, double wy) {
    const auto layers = scene.state.doc->flatten();
    for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
        Layer* layer = *it;
        if (!layer->is_visible_chain()) continue;
        const auto local = layer->from_world(wx, wy);
        if (!layer->local_contains(local.first, local.second)) continue;
        const Asset& asset = scene.assets.get(layer->asset_key);
        // 图层局部坐标的 y 向上（0 = 图像底边，见 Layer::to_world），位图行号
        // 却是自顶向下。这里必须翻一次再取样，否则命中掩膜上下镜像：点形状
        // 上缘取到的是下缘的 alpha，多边形的边缘就点不中、直接穿透到下层。
        const double row = static_cast<double>(asset.height) - local.second;
        if (asset.alpha_at(local.first, row) > cfg::ALPHA_HIT)
            return layer;
    }
    return nullptr;
}

bool BboxAt(Scene& scene, double wx, double wy, Layer** out_layer,
            Region** out_region) {
    if (scene.bbox_overlay_hidden()) return false;
    const auto layers = scene.state.doc->flatten();
    for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
        Layer* layer = *it;
        if (!layer->is_visible_chain()) continue;
        const auto local = layer->from_world(wx, wy);
        for (auto rit = layer->bboxes.rbegin(); rit != layer->bboxes.rend();
             ++rit) {
            Region* region = rit->get();
            if (!region->visible) continue;   // 眼睛关掉：画面上不存在，点不到
            if (region->contains(local.first, local.second)) {
                *out_layer = layer;
                *out_region = region;
                return true;
            }
        }
    }
    return false;
}

std::string HandleAt(Scene& scene, double wx, double wy) {
    (void)wx;
    (void)wy;
    Layer* layer = scene.selected_layer();
    Region* region = scene.selection.sole_region();
    if (scene.bbox_overlay_hidden() || layer == nullptr || region == nullptr)
        return std::string();
    if (!layer->is_visible_chain() || !region->visible) return std::string();
    // 命中与绘制同一条链：手柄点走 region->to_world → layer->to_world →
    // world_to_window，再和光标比屏幕距离。半径 10px 是屏幕像素，不折回局部，
    // 各向异性 / 缩放 / 画布尺寸怎么变都不会变形。
    const auto& handles =
        HandleList(*region, RegionUpScale(scene, *layer, *region));
    const double r2 = cfg::HANDLE_HIT_RADIUS * cfg::HANDLE_HIT_RADIUS;
    for (const Handle& handle : handles) {
        const auto offset = region->to_world(handle.x, handle.y);
        const auto world = layer->to_world(region->cx + offset.first,
                                           region->cy + offset.second);
        const auto screen = scene.world_to_window(world.first, world.second);
        const double dx = screen.first - scene.mouse_x;
        const double dy = screen.second - scene.mouse_y;
        if (dx * dx + dy * dy <= r2) return std::string(handle.name);
    }
    return std::string();
}

std::string TransformHandleAt(Scene& scene, double wx, double wy) {
    if (!scene.transform_mode || scene.transform_layer == nullptr) return "";
    Layer* layer = scene.transform_layer;
    // 与绘制同源：内容框 = 位图去掉套索抠图留下的透明补边
    const double pad = static_cast<double>(layer->pad);
    const double w = std::max(1.0, static_cast<double>(layer->w) - pad * 2.0);
    const double h = std::max(1.0, static_cast<double>(layer->h) - pad * 2.0);
    const double corner_lx[4] = {pad, pad + w, pad + w, pad};
    const double corner_ly[4] = {pad, pad, pad + h, pad + h};
    const char* names[4] = {"tl", "tr", "br", "bl"};

    const double hit = cfg::HANDLE_HIT_RADIUS;
    double best = hit * hit;
    std::string corner;
    for (int i = 0; i < 4; ++i) {
        const auto pw = layer->to_world(corner_lx[i], corner_ly[i]);
        const auto s = scene.world_to_window(pw.first, pw.second);
        const double dx = s.first - scene.mouse_x;
        const double dy = s.second - scene.mouse_y;
        const double d2 = dx * dx + dy * dy;
        if (d2 <= best) {
            best = d2;
            corner = names[i];
        }
    }
    if (!corner.empty()) return corner;

    const auto local = layer->from_world(wx, wy);
    if (!layer->local_contains(local.first, local.second)) {
        const double reach = cfg::HANDLE_HIT_RADIUS + cfg::HANDLE_ROT_GAP;
        double best2 = reach * reach;
        bool rot = false;
        for (int i = 0; i < 4; ++i) {
            const auto pw = layer->to_world(corner_lx[i], corner_ly[i]);
            const auto s = scene.world_to_window(pw.first, pw.second);
            const double dx = s.first - scene.mouse_x;
            const double dy = s.second - scene.mouse_y;
            const double d2 = dx * dx + dy * dy;
            if (d2 <= best2) {
                best2 = d2;
                rot = true;
            }
        }
        if (rot) return "rot";
        return "";
    }
    return "move";
}

void HandleKey(Scene& scene, int key, int mods) {
    if (key == GLFW_KEY_LEFT_ALT || key == GLFW_KEY_RIGHT_ALT) {
        scene.alt_held = true;
        return;
    }
    if (key == GLFW_KEY_SPACE) {
        if (!scene.space_held) {
            scene.space_held = true;
            scene.space_used = false;
        }
        return;
    }
    const bool ctrl = (mods & GLFW_MOD_CONTROL) != 0;
    const bool shift = (mods & GLFW_MOD_SHIFT) != 0;
    if (ctrl && key == GLFW_KEY_T) {
        scene.begin_transform();
        return;
    }
    if (scene.transform_mode && key == GLFW_KEY_ENTER) {
        scene.end_transform(true);
        return;
    }
    if (scene.transform_mode && key == GLFW_KEY_ESCAPE) {
        scene.end_transform(false);
        return;
    }
    if (scene.transform_mode) {
        // 变换中只认回车/Esc/Ctrl+T，避免误删或误改选择
        return;
    }
    if (!ctrl && !shift && key == GLFW_KEY_E) {
        // E = 切成橡皮（可见窗口底部工具小板：半径/羽化）。
        // 从套索切过来时套索一律清掉（橡皮模式下不存在套索）；在橡皮模式里
        // 再按 E = 关掉橡皮。
        scene.set_eraser(!scene.eraser_on());
        return;
    }
    if (key == GLFW_KEY_X) {
        if (ctrl) {
            if (BlockedByTool(scene, "Cut")) return;
            if (scene.cut_out_to_clipboard() && scene.erase_lasso_area()) {
                scene.set_status("已剪走套索内的像素（Ctrl+V 贴成新层）");
            } else {
                scene.set_status("当前没有闭合的套索");
            }
            return;
        }
        scene.toggle_x();
        return;
    }
    if (ctrl && key == GLFW_KEY_Z) {
        // Shift 先判：否则 Ctrl+Shift+Z 会被 Ctrl+Z 吞掉，变成又撤销一步
        if (shift) {
            scene.redo();
        } else {
            scene.undo();
        }
        return;
    }
    if (ctrl && key == GLFW_KEY_C) {
        if (BlockedByTool(scene, "Copy")) return;
        if (LassoTarget(scene) != nullptr && scene.lasso_closed()) {
            scene.cut_out_to_clipboard();
        } else {
            CopySelection(scene);
        }
        return;
    }
    if (ctrl && key == GLFW_KEY_V) {
        if (BlockedByTool(scene, "Paste")) return;
        if (scene.clipboard_region) {
            PasteBbox(scene, *scene.clipboard_region);
            scene.finish_lasso();   // 粘贴完就退出套索：选区用完即清
            return;
        }
        if (scene.clipboard_layer) {
            // 图层的落点由剪贴板里的原位决定，不再跟光标
            scene.paste_layer(*scene.clipboard_layer, 0.0, 0.0);
            scene.finish_lasso();
            return;
        }
        scene.set_status("剪贴板为空");
        return;
    }
    if (ctrl && key == GLFW_KEY_A) {
        scene.select_all();
        return;
    }
    if (!ctrl && key == GLFW_KEY_A) {
        if (scene.transform_mode) return;
        // A 的语义只由套索自身状态决定：
        //   正在画   → 丢掉这一笔（路径没提交过像素）；
        //   已闭合   → 清除它；
        //   都没有   → 开始画。
        // 从橡皮切过来也是这一条：切换工具会先收掉橡皮那一笔。
        switch (scene.lasso_phase()) {
            case Scene::LassoPhase::kDrawing:
                scene.cancel_lasso();
                return;
            case Scene::LassoPhase::kClosed:
                scene.clear_lasso();
                return;
            case Scene::LassoPhase::kNone:
                scene.toggle_lasso_mode();
                return;
        }
        return;
    }
    if (scene.is_lasso_mode() && key == GLFW_KEY_ENTER) {
        scene.commit_lasso();
        return;
    }
    if (scene.is_lasso_mode() && key == GLFW_KEY_ESCAPE) {
        scene.cancel_lasso();
        return;
    }
    if (scene.is_lasso_mode() && key == GLFW_KEY_DELETE) {
        scene.remove_lasso_point();
        return;
    }
    if (key == GLFW_KEY_DELETE) {
        // 橡皮模式下 Del 只删图层，不改像素（套索在橡皮模式下已被清掉，
        // 所以这里天然不会走到"剪套索内像素"那一支）。
        if (scene.selection.multi()) {
            scene.delete_selection();
            return;
        }
        if (scene.selected_region() != nullptr) {
            Layer* layer = scene.state.doc->layer_of(scene.selected_region());
            if (layer != nullptr)
                scene.remove_bbox(layer, scene.selected_region());
            return;
        }
        if (!scene.eraser_on() && LassoTarget(scene) != nullptr) {
            scene.erase_lasso_area();
            scene.set_status("已剪掉套索内的像素（再按 Del 删除图层）");
        } else if (scene.selected_layer() != nullptr) {
            scene.delete_layer(scene.selected_layer());
        }
        return;
    }
    if (key == GLFW_KEY_HOME) {
        scene.fit_view();
        return;
    }
    if (key == GLFW_KEY_ESCAPE) {
        scene.clear_selection();
    }
}

void HandleKeyRelease(Scene& scene, int key, int mods) {
    (void)mods;
    if (key == GLFW_KEY_LEFT_ALT || key == GLFW_KEY_RIGHT_ALT) {
        scene.alt_held = false;
        return;
    }
    if (key != GLFW_KEY_SPACE || !scene.space_held) return;
    scene.space_held = false;
    if (!scene.space_used) scene.toggle_bbox_overlay();
    scene.space_used = false;
}

void HandlePress(Scene& scene, double x, double y, int button, int mods) {
    if (!InCanvas(scene, x) || button != GLFW_MOUSE_BUTTON_LEFT) return;

    const auto world = scene.window_to_world(x, y);
    if (scene.transform_mode) {
        Layer* layer = scene.transform_layer;
        if (layer == nullptr) return;
        const double w = static_cast<double>(layer->w);
        const double h = static_cast<double>(layer->h);
        // 内容四角（含旋转，去掉套索抠图的透明补边）：局部 (pad,pad)/(pad+w,pad)…
        const double pad = static_cast<double>(layer->pad);
        const double cw = std::max(1.0, w - pad * 2.0);
        const double ch = std::max(1.0, h - pad * 2.0);
        const double x0 = pad;
        const double x1 = pad + cw;
        const double y0 = pad;
        const double y1 = pad + ch;
        const double corner_lx[4] = {x0, x1, x1, x0};
        const double corner_ly[4] = {y0, y0, y1, y1};
        const double anchor_lx[4] = {x1, x0, x0, x1};
        const double anchor_ly[4] = {y1, y1, y0, y0};
        const char* names[4] = {"tl", "tr", "br", "bl"};

        double best = 1e18;
        int best_i = -1;
        for (int i = 0; i < 4; ++i) {
            const auto pw = layer->to_world(corner_lx[i], corner_ly[i]);
            const auto s = scene.world_to_window(pw.first, pw.second);
            const double dx = s.first - x;
            const double dy = s.second - y;
            const double d2 = dx * dx + dy * dy;
            if (d2 <= cfg::HANDLE_HIT_RADIUS * cfg::HANDLE_HIT_RADIUS
                && d2 < best) {
                best = d2;
                best_i = i;
            }
        }
        if (best_i >= 0) {   // 拖角：缩放（Ctrl 解除比例；ALT 决定主轴）
            scene.drag_kind = "transform";
            scene.drag_layer = layer;
            scene.drag_handle = names[best_i];
            scene.transform_corner_lx = corner_lx[best_i];
            scene.transform_corner_ly = corner_ly[best_i];
            // 主轴在按下时定死，拖动中不换：换主轴等于换坐标系，画面会跳。
            // ALT = 对角不动；没有 ALT = 图像中心不动（两边对称缩放）。
            const bool alt = (mods & GLFW_MOD_ALT) != 0;
            scene.transform_scale_center = !alt;
            if (alt) {
                scene.transform_anchor_lx = anchor_lx[best_i];
                scene.transform_anchor_ly = anchor_ly[best_i];
                const auto aw =
                    layer->to_world(anchor_lx[best_i], anchor_ly[best_i]);
                scene.transform_anchor_x = aw.first;
                scene.transform_anchor_y = aw.second;
            } else {
                scene.transform_anchor_lx = w / 2.0;
                scene.transform_anchor_ly = h / 2.0;
                const auto aw = layer->to_world(w / 2.0, h / 2.0);
                scene.transform_anchor_x = aw.first;
                scene.transform_anchor_y = aw.second;
            }
            scene.transform_drag_sx = layer->sx;
            scene.transform_drag_sy = layer->sy;
            scene.transform_drag_x = layer->x;
            scene.transform_drag_y = layer->y;
            scene.transform_drag_rot = layer->rot;
            scene.drag_undo_pushed = false;
            PushOnce(scene);
            return;
        }

        const auto local = layer->from_world(world.first, world.second);
        if (!layer->local_contains(local.first, local.second)) {
            // 图外、贴近某角：旋转（ImageEditor 的旋转热区在角外侧）
            double best2 = 1e18;
            int rot_i = -1;
            const double reach = cfg::HANDLE_HIT_RADIUS + cfg::HANDLE_ROT_GAP;
            for (int i = 0; i < 4; ++i) {
                const auto pw = layer->to_world(corner_lx[i], corner_ly[i]);
                const auto s = scene.world_to_window(pw.first, pw.second);
                const double dx = s.first - x;
                const double dy = s.second - y;
                const double d2 = dx * dx + dy * dy;
                if (d2 <= reach * reach && d2 < best2) {
                    best2 = d2;
                    rot_i = i;
                }
            }
            if (rot_i >= 0) {
                scene.drag_kind = "transform";
                scene.drag_layer = layer;
                scene.drag_handle = "rot";
                scene.transform_drag_rot = layer->rot;
                const auto c = scene.world_to_window(layer->x, layer->y);
                scene.transform_drag_angle =
                    std::atan2(y - c.second, x - c.first);
                scene.drag_undo_pushed = false;
                PushOnce(scene);
            }
            return;
        }

        // 图内：移动位置（带四边/中心吸附）
        scene.drag_kind = "transform-move";
        scene.drag_layer = layer;
        scene.drag_last = {world.first, world.second};
        scene.drag_undo_pushed = false;
        PushOnce(scene);
        return;
    }
    if (scene.space_held) {
        // 空格 + 拖 = 和鼠标中键同一件事：搬整个视图，不动任何图层
        scene.space_used = true;
        scene.begin_drag("pan", nullptr, "", world.first, world.second);
        return;
    }
    if (scene.is_lasso_mode()) {
        // 双击闭合：同一点、间隔内第二次按下即闭合多边形
        const double now = glfwGetTime();
        const double dx = x - scene.last_press_x;
        const double dy = y - scene.last_press_y;
        const bool same_spot = dx * dx + dy * dy
                               <= cfg::HANDLE_HIT_RADIUS * cfg::HANDLE_HIT_RADIUS;
        if (scene.last_press_time >= 0.0 && same_spot
            && now - scene.last_press_time <= cfg::DOUBLE_CLICK_SECONDS
            && scene.lasso_path.size() >= 3) {
            scene.commit_lasso();
            scene.last_press_time = -1.0;
            return;
        }
        // 路径存画布世界坐标：点下去就记这个点，不再换算到某个图层里
        scene.add_lasso_point(world.first, world.second);
        scene.last_press_time = now;
        scene.last_press_x = x;
        scene.last_press_y = y;
        return;
    }

    const std::string handle = HandleAt(scene, world.first, world.second);
    if (!handle.empty()) {
        scene.begin_drag("handle", scene.selected_layer(), handle, world.first,
                         world.second);
        return;
    }

    Layer* layer = nullptr;
    Region* region = nullptr;
    if (BboxAt(scene, world.first, world.second, &layer, &region)) {
        if ((mods & GLFW_MOD_CONTROL) != 0) {
            scene.toggle_pick(layer, region);
            return;
        }
        if (!scene.selection.has_region(region))
            scene.select_region(layer, region);
        if (scene.selection.multi()) {
            scene.begin_drag("group", nullptr, "", world.first, world.second);
            return;
        }
        scene.begin_drag("bbox", layer, "", world.first, world.second);
        const auto local = layer->from_world(world.first, world.second);
        scene.drag_offset =
            std::make_pair(region->cx - local.first, region->cy - local.second);
        return;
    }

    layer = LayerAt(scene, world.first, world.second);
    if (layer != nullptr) {
        // 双击图层：有套索就清套索，没有套索就进入自由变换
        const double now = glfwGetTime();
        const double ddx = x - scene.last_press_x;
        const double ddy = y - scene.last_press_y;
        const bool same = ddx * ddx + ddy * ddy
                          <= cfg::HANDLE_HIT_RADIUS * cfg::HANDLE_HIT_RADIUS;
        if (scene.last_press_time >= 0.0 && same
            && now - scene.last_press_time <= cfg::DOUBLE_CLICK_SECONDS) {
            scene.last_press_time = -1.0;
            if (scene.lasso_closed()) {
                scene.clear_lasso();
            } else {
                scene.begin_transform_for(layer);
            }
            return;
        }
        scene.last_press_time = now;
        scene.last_press_x = x;
        scene.last_press_y = y;
    }
    if (layer == nullptr) {
        if ((mods & GLFW_MOD_CONTROL) != 0) return;   // Ctrl+点空白：不动选择
        scene.clear_selection();
        scene.begin_drag("pan", nullptr, "", world.first, world.second);
        return;
    }
    if ((mods & GLFW_MOD_CONTROL) != 0) {
        scene.toggle_pick(layer, nullptr);
        return;
    }
    if (!scene.selection.has_layer(layer)) scene.select_layer(layer);
    if (scene.selection.multi()) {
        scene.begin_drag("group", nullptr, "", world.first, world.second);
        return;
    }
    scene.begin_drag("layer", layer, "", world.first, world.second);
}

void HandleDrag(Scene& scene, double x, double y, double dx, double dy,
                int mods) {
    const std::string& kind = scene.drag_kind;
    if (kind.empty()) return;
    if (kind == "pan") {
        // 场景是左下原点、y 向上；鼠标的 dy 也已在边界换算成 y 向上
        // （bootstrap.cpp 的 SceneY）。要让画面跟着手走，两轴都应是"加"：
        // 往下拖 → dy 为负 → view.y 减小 → 画面下移。
        scene.view.x += dx;
        scene.view.y += dy;
        return;
    }

    const auto world = scene.window_to_world(x, y);
    if (kind == "transform-move") {
        Layer* layer = scene.drag_layer;
        if (layer == nullptr) return;
        PushOnce(scene);
        double sx = world.first - scene.drag_last.first;
        double sy = world.second - scene.drag_last.second;
        scene.drag_last = {world.first, world.second};
        // 变形态归变形对象所有：Ctrl 在这里只表示"解除比例"，
        // 不能再兼做画布对齐（否则两个职责抢同一个键）。
        scene.snap_move(layer, &sx, &sy, false);
        layer->move_by(sx, sy);
        scene.carry_transform_children();   // 父层移动，子层一起挪
        scene.state.mark_physics_dirty();
        return;
    }
    if (kind == "transform") {
        Layer* layer = scene.drag_layer;
        if (layer == nullptr) return;
        PushOnce(scene);
        if (scene.drag_handle == "rot") {
            // 旋转：光标相对图层中心的角度变化就是旋转角
            const auto c = scene.world_to_window(layer->x, layer->y);
            const double ang = std::atan2(y - c.second, x - c.first);
            layer->rot = scene.transform_drag_rot
                         + (ang - scene.transform_drag_angle);
            scene.carry_transform_children();   // 父层转，子层跟着转
            scene.state.mark_physics_dirty();
            return;
        }
        // 拖角缩放：所有量都在"按下那一刻"的坐标系里算。
        // 不能用 layer->from_world()：它按**当前** sx/sy 反算局部坐标，而 sx/sy
        // 正是本帧要改的量 —— 每帧拿新比例去反解鼠标，形成自我反馈，画面就抖。
        const double w = static_cast<double>(layer->w);
        const double h = static_cast<double>(layer->h);
        // 主轴在按下时定死：ALT = 对角，否则 = 图像中心。
        const double ax = scene.transform_anchor_lx;
        const double ay = scene.transform_anchor_ly;
        // 世界 → 起始局部坐标（按下时的 x/y/rot/sx/sy 定义的坐标系）
        const double sr = std::cos(scene.transform_drag_rot);
        const double sn = std::sin(scene.transform_drag_rot);
        const double wdx = world.first - scene.transform_drag_x;
        const double wdy = world.second - scene.transform_drag_y;
        const double kx = std::abs(scene.transform_drag_sx) > 1e-9
                              ? scene.transform_drag_sx : 1.0;
        const double ky = std::abs(scene.transform_drag_sy) > 1e-9
                              ? scene.transform_drag_sy : 1.0;
        const double local_x = (wdx * sr + wdy * sn) / kx + w / 2.0;
        const double local_y = (-wdx * sn + wdy * sr) / ky + h / 2.0;
        const double base_x = scene.transform_corner_lx - ax;
        const double base_y = scene.transform_corner_ly - ay;
        double fx = (local_x - ax) / (std::abs(base_x) > 1e-6 ? base_x : 1.0);
        double fy = (local_y - ay) / (std::abs(base_y) > 1e-6 ? base_y : 1.0);
        if ((mods & GLFW_MOD_CONTROL) == 0) {   // 未按 Ctrl：等比
            // 沿"起始对角向量"做投影，得到一个单调的比例；
            // 取 max(fx, fy) 会让两个轴来回夺主导，同样会抖。
            const double bx = base_x * kx;
            const double by = base_y * ky;
            const double den = bx * bx + by * by;
            const double mx = (local_x - ax) * kx;
            const double my = (local_y - ay) * ky;
            const double f = den > 1e-9 ? (mx * bx + my * by) / den
                                        : std::max(fx, fy);
            fx = f;
            fy = f;
        }
        fx = std::max(fx, 0.001);   // 不让缩放穿到负值（避免镜像）
        fy = std::max(fy, 0.001);
        layer->sx = scene.transform_drag_sx * fx;
        layer->sy = scene.transform_drag_sy * fy;
        if (scene.transform_scale_center) {
            // 以图像中心为主：中心的世界位置本来就不该动，x/y 保持按下时的值即可
            layer->x = scene.transform_drag_x;
            layer->y = scene.transform_drag_y;
        } else {
            // 对角（锚点）的世界位置精确保持不动：一次算死，不靠每帧增量逼近
            const double cr = std::cos(layer->rot);
            const double ci = std::sin(layer->rot);
            const double alx = (ax - w / 2.0) * layer->sx;
            const double aly = (ay - h / 2.0) * layer->sy;
            layer->x = scene.transform_anchor_x - (alx * cr - aly * ci);
            layer->y = scene.transform_anchor_y - (alx * ci + aly * cr);
        }
        scene.carry_transform_children();   // 父层缩放/位移，子层同步
        scene.state.mark_physics_dirty();
        return;
    }
    if (kind == "content") {
        PushOnce(scene);
        const double step_x = world.first - scene.drag_last.first;
        const double step_y = world.second - scene.drag_last.second;
        for (auto& root : scene.state.doc->roots()) root->move_by(step_x, step_y);
        scene.drag_last = {world.first, world.second};
        scene.state.mark_physics_dirty();
        return;
    }
    if (kind == "group") {
        PushOnce(scene);
        const double step_x = world.first - scene.drag_last.first;
        const double step_y = world.second - scene.drag_last.second;
        scene.drag_last = {world.first, world.second};
        scene.move_group(step_x, step_y);
        scene.state.mark_physics_dirty();
        return;
    }
    if (kind == "layer") {
        Layer* layer = scene.drag_layer;
        if (layer == nullptr) return;
        PushOnce(scene);
        double step_x = world.first - scene.drag_last.first;
        double step_y = world.second - scene.drag_last.second;
        scene.drag_last = {world.first, world.second};
        scene.snap_move(layer, &step_x, &step_y,
                        (mods & GLFW_MOD_CONTROL) != 0);
        layer->move_by(step_x, step_y);
        scene.state.mark_physics_dirty();
        return;
    }

    Layer* layer = scene.drag_layer;
    Region* region = scene.drag_region;
    if (layer == nullptr || region == nullptr) return;
    PushOnce(scene);
    const auto local = layer->from_world(world.first, world.second);

    if (kind == "bbox") {
        region->cx = local.first + scene.drag_offset.first;
        region->cy = local.second + scene.drag_offset.second;
        scene.state.mark_physics_dirty();
        return;
    }

    if (scene.drag_handle == "rotate") {
        // 必须用未旋转的图层坐标量角度：to_local 会跟着 rot 转。
        const double angle =
            std::atan2(local.second - region->cy, local.first - region->cx)
            * 180.0 / 3.14159265358979323846;
        double delta = angle - scene.handle_start_angle;
        while (delta > 180.0) delta -= 360.0;
        while (delta < -180.0) delta += 360.0;
        double rot = scene.handle_start_rot + delta;
        while (rot > 180.0) rot -= 360.0;
        while (rot < -180.0) rot += 360.0;
        region->rot = rot;
    } else {
        const auto center = region->to_local(local.first, local.second);
        const double rmax = std::max<double>(layer->w, layer->h);
        // 默认按住手柄拖 = 自由（只改一根轴），按住 Ctrl 才保持比例（两根半径同倍率）。
        const bool keep_ratio = (mods & GLFW_MOD_CONTROL) != 0;
        const auto clamp_r = [rmax](double value) {
            return std::min(std::max(value, cfg::RADIUS_MIN), rmax);
        };
        const bool vertical =
            scene.drag_handle == "top" || scene.drag_handle == "bottom";
        const double target =
            vertical ? (scene.drag_handle == "top" ? -center.second
                                                   : center.second)
                     : (scene.drag_handle == "left" ? -center.first
                                                    : center.first);
        const double start_axis = vertical ? scene.handle_start_ry
                                           : scene.handle_start_rx;
        if (!keep_ratio || start_axis <= 1e-9) {
            if (vertical) {
                region->ry = clamp_r(target);
            } else {
                region->rx = clamp_r(target);
            }
        } else {
            const double f = target / start_axis;
            region->rx = clamp_r(scene.handle_start_rx * f);
            region->ry = clamp_r(scene.handle_start_ry * f);
        }
    }
    scene.state.mark_physics_dirty();
}

void HandleRelease(Scene& scene, double x, double y, int button, int mods) {
    (void)x;
    (void)y;
    (void)button;
    (void)mods;
    scene.guide_x = false;
    scene.guide_y = false;
    scene.drag_kind.clear();
    scene.drag_layer = nullptr;
    scene.drag_region = nullptr;
    scene.drag_handle.clear();
}

bool HandleScroll(Scene& scene, double x, double y, double dy) {
    if (!InCanvas(scene, x) || dy == 0.0) return false;
    // 滚轮只负责视图缩放：图层/BBOX 的大小不走滚轮（改尺寸走 Ctrl+T 与面板）。
    return ZoomView(scene, x, y, dy);
}

}  // namespace bl::ui::input

