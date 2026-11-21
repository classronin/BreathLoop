#include "ui/scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

#include "domain/seamless.h"
#include "io/blob.h"
#include "io/paths.h"
#include "render/backdrop.h"
#include "render/cutout.h"
#include "render/eraser.h"
#include "render/exporter.h"
#include "render/framing.h"
#include "render/gfx.h"
#include "render/imgtool.h"
#include "ui/cursors.h"
#include "ui/input.h"
#include "ui/layer_panel.h"
#include "ui/overlay.h"
#include "ui/param_panel.h"
#include "ui/status.h"
#include "ui/tool_board.h"

namespace bl::ui {
namespace {

Renderer& canvas_renderer() {
    static Renderer renderer;
    return renderer;
}

// 剩余时间的说法：不足 1 秒不给小数，超过 1 分钟换成分秒。
std::string RemainingText(double seconds) {
    if (!(seconds > 0.0)) return "不到 1 秒";
    char buf[64];
    if (seconds < 60.0) {
        std::snprintf(buf, sizeof(buf), "%.0f 秒", seconds);
    } else {
        const int total = static_cast<int>(std::lround(seconds));
        std::snprintf(buf, sizeof(buf), "%d 分 %d 秒", total / 60, total % 60);
    }
    return buf;
}

}  // namespace

Scene::Scene() = default;

Scene::~Scene() {
    // 后台编码线程还活着就先等它收尾，否则 std::thread 析构会直接终止进程
    if (encode_thread_.joinable()) encode_thread_.join();
    delete layer_panel;
    delete param_panel;
    delete tool_board;
    delete status;
    delete cursors;
    layer_views.release_all();
}

bool Scene::init(GLFWwindow* window_handle, std::string* error) {
    window_ = window_handle;
    int w = 0;
    int h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    width_ = std::max(1, w);
    height_ = std::max(1, h);
    // 档位基线在这里取真值：程序以最大化方式启动时，首帧 set_size 看到
    // GLFW_MAXIMIZED=true 也不该算"用户刚最大化"，否则会白改一次视图。
    was_maximized_ =
        glfwGetWindowAttrib(window_, GLFW_MAXIMIZED) == GLFW_TRUE;
    now = glfwGetTime();

    state.ms_per_frame = cfg::DEFAULT_MS_PER_FRAME;
    state.frames = cfg::DEFAULT_FRAMES;
    state.export_lossless = cfg::EXPORT_DEFAULT_LOSSLESS;
    state.export_quality = cfg::EXPORT_DEFAULT_QUALITY;
    state.export_effort = cfg::EXPORT_DEFAULT_EFFORT;
    state.export_alpha_q = cfg::EXPORT_DEFAULT_ALPHA_Q;
    if (state.doc == nullptr) {
        if (error != nullptr) *error = "文档初始化失败";
        return false;
    }

    param_panel = new ParamPanel();
    layer_panel = new LayerPanel();
    tool_board = new ToolBoard();
    status = new CenterStatus();
    cursors = new SceneCursors();
    param_panel->init(this);
    layer_panel->init(this);
    tool_board->init(this);
    status->init(this);
    cursors->init(window_);
    relayout(height_);
    param_panel->set_has_layers(has_layers());
    param_panel->sync_export();
    param_panel->refresh_fps_info();
    return true;
}

// ---- 坐标 ----
// 输入的 x/y 已经在 bootstrap 边界换算成"原点左下、y 向上"的窗口坐标，
// 与场景坐标同一朝向，纵向不再翻一次。
std::pair<double, double> Scene::window_to_stage(double x, double y) const {
    return {x, y};
}

std::pair<double, double> Scene::stage_to_world(double x, double y) const {
    const double s = view.scale > 0.0 ? view.scale : 1.0;
    return {(x - view.x) / s, (y - view.y) / s};
}

std::pair<double, double> Scene::window_to_world(double x, double y) const {
    const auto stage = window_to_stage(x, y);
    return stage_to_world(stage.first, stage.second);
}

std::pair<double, double> Scene::world_to_stage(double x, double y) const {
    const double s = view.scale > 0.0 ? view.scale : 1.0;
    return {view.x + x * s, view.y + y * s};
}

std::pair<double, double> Scene::world_to_window(double x, double y) const {
    return world_to_stage(x, y);
}

std::pair<double, double> Scene::canvas_edges() const {
    return input::CanvasEdges(*this);
}

bool Scene::in_canvas(double x) const { return input::InCanvas(*this, x); }

bool Scene::over_panel(float x, float y) const {
    const float w = static_cast<float>(width_);
    const float h = static_cast<float>(height_);
    for (const std::string& side : {"left", "right"}) {
        const PanelFloat& panel = side == "left" ? param_float : layer_float;
        if (panel.contains(x, y, w, h, !panel.floating && is_collapsed(side)))
            return true;
    }
    return false;
}

Rect Scene::canvas_rect() const {
    if (state.doc == nullptr) return Rect{-540.0, -960.0, 540.0, 960.0};
    return state.doc->canvas_rect();
}

std::pair<int, int> Scene::canvas_size() const {
    if (state.doc == nullptr) return {cfg::CANVAS_DEFAULT_W, cfg::CANVAS_DEFAULT_H};
    return {state.doc->canvas_w, state.doc->canvas_h};
}

void Scene::set_canvas_size(int width, int height) {
    if (state.doc == nullptr) return;
    state.doc->canvas_w = std::max(1, width);
    state.doc->canvas_h = std::max(1, height);
    state.export_w = state.doc->canvas_w;
    state.export_h = state.doc->canvas_h;
    state.export_size_auto = false;
    if (param_panel != nullptr) param_panel->refresh_export_size();
}

bool Scene::adopt_canvas_from_layer(Layer* layer) {
    if (layer == nullptr) {
        set_status("先选中一个图层");
        return false;
    }
    Asset* asset = nullptr;
    try {
        asset = &assets.get(layer->asset_key);
    } catch (const std::exception& error) {
        set_status(std::string("读不到这个图层的位图：") + error.what());
        return false;
    }
    const int w = asset->width;
    const int h = asset->height;
    if (w <= 0 || h <= 0) {
        set_status("这个图层没有可用的位图尺寸");
        return false;
    }
    const std::pair<int, int> old = canvas_size();
    const std::string label = std::to_string(old.first) + " × " +
                              std::to_string(old.second) + " → " +
                              std::to_string(w) + " × " + std::to_string(h) +
                              "（" + layer->name + "）";
    if (old.first == w && old.second == h) {
        set_status("画布已经是 " + label);
        return true;
    }
    // 画布是文档的一部分（Document::copy 会带上 canvas_w/h），所以压撤销。
    push_undo();
    set_canvas_size(w, h);
    fit_view();
    if (layer_panel != nullptr) layer_panel->rebuild();
    set_status("画布 " + label);
    return true;
}

void Scene::set_size(int width, int height) {
    width_ = std::max(1, width);
    height_ = std::max(1, height);
    clamp_floats(height_);
    relayout(height_);

    // 最大化/还原是"窗口档位"变化，不是普通拖动缩放：档位一变就把视图
    // 重新居中并按新客户区适配（fit_view 自己算 scale 与偏移）。
    // 只认档位，所以手动拖窗口边框时视图保持原样。
    if (window_ != nullptr) {
        const bool maximized =
            glfwGetWindowAttrib(window_, GLFW_MAXIMIZED) == GLFW_TRUE;
        if (maximized != was_maximized_) {
            was_maximized_ = maximized;
            fit_view();
        }
    }
}

void Scene::relayout(int height) {
    height_ = std::max(1, height);
    clamp_floats(height_);
    param_panel->relayout(height_);
    layer_panel->relayout(height_);
    status->relayout(height_);
}

void Scene::clamp_floats(int height) {
    const float win_w = static_cast<float>(width_);
    const float win_h = static_cast<float>(height);
    for (PanelFloat* panel : float_states()) {
        panel->clamp_to(win_w, win_h);
        if (panel->floating) {
            panel->h = std::max(static_cast<float>(cfg::PANEL_MIN_FLOAT_H),
                                panel->h);
        }
    }
}

// ---- 折叠 ----
bool Scene::is_collapsed(const std::string& side) const {
    auto it = collapsed_.find(side);
    return it != collapsed_.end() && it->second;
}

void Scene::set_collapsed(const std::string& side, bool value) {
    collapsed_[side] = value;
    relayout(height_);
}

void Scene::apply_saved_collapsed(bool left, bool right) {
    collapsed_["left"] = left;
    collapsed_["right"] = right;
    relayout(height_);
}

// ---- 浮动面板 ----
PanelFloat& Scene::panel_float(const std::string& which) {
    return which == "param" ? param_float : layer_float;
}

const PanelFloat& Scene::panel_float(const std::string& which) const {
    return which == "param" ? param_float : layer_float;
}

std::vector<PanelFloat*> Scene::float_states() {
    return {&param_float, &layer_float};
}

void Scene::set_panel_floating(const std::string& which, bool floating) {
    PanelFloat& panel = panel_float(which);
    if (panel.floating == floating) return;
    const float win_w = static_cast<float>(width_);
    const float win_h = static_cast<float>(height_);
    const bool collapsed = !panel.floating && is_collapsed(panel.side);
    const auto rect = panel.rect(win_w, win_h, collapsed);
    panel.floating = floating;
    panel.w = rect[2];
    panel.h = rect[3];
    panel.left = rect[0];
    panel.bottom = rect[1];
    if (floating) {
        panel.h = std::max(static_cast<float>(cfg::PANEL_MIN_FLOAT_H),
                           std::min(panel.h, static_cast<float>(height_)
                                                - 2.0f * cfg::PANEL_FLOAT_MARGIN));
        raise_float(which);
    } else {
        panel.w = panel.default_w;
        panel.h = static_cast<float>(height_);
    }
    panel.clamp_to(win_w, win_h);
    relayout(height_);
}

std::string Scene::float_at(float x, float y) const {
    for (auto it = float_order.rbegin(); it != float_order.rend(); ++it) {
        const std::string& which = *it;
        PanelFloat& panel = which == "param"
                                ? const_cast<PanelFloat&>(param_float)
                                : const_cast<PanelFloat&>(layer_float);
        if (!panel.floating) continue;
        if (panel.contains(x, y, static_cast<float>(width_),
                           static_cast<float>(height_), false)) {
            return which;
        }
    }
    return std::string();
}

void Scene::raise_float(const std::string& which) {
    auto it = std::find(float_order.begin(), float_order.end(), which);
    if (it == float_order.end()) {
        float_order.push_back(which);
        return;
    }
    if (it + 1 != float_order.end()) {
        float_order.erase(it);
        float_order.push_back(which);
    }
}

// ---- 选择 ----
void Scene::selection_changed() {
    Layer* layer = selection.layer();
    if (layer != nullptr && selection.region() != nullptr) {
        param_panel->show_region(selection.region());
    } else {
        param_panel->show_region(nullptr);
        if (layer != nullptr && !selection.multi()) {
            layer_panel->rebuild();
        }
    }
    layer_panel->rebuild();
}

void Scene::select_layer(Layer* layer) {
    selection.only(layer, nullptr);
    // 换选中层时"保持比例"回到默认打勾；变形把它弄成非等比时 refresh 会自动取消
    layer_lock_ = true;
    // 选择是选择，套索是套索：套索存的是世界坐标、目标现推，换选中层不改形状，
    // 所以这里既不重置套索，也不把"正在画"抹掉。
    cancel_bbox_drag();
    selection_changed();
}

void Scene::select_region(Layer* layer, Region* region) {
    selection.only(layer, region);
    cancel_bbox_drag();
    selection_changed();
}

void Scene::clear_selection() {
    selection.clear();
    cancel_bbox_drag();
    selection_changed();
}

void Scene::toggle_pick(Layer* layer, Region* region) {
    selection.toggle(layer, region);
    selection_changed();
}

void Scene::select_all() {
    if (state.doc != nullptr) selection.select_all(*state.doc);
    selection_changed();
}

// ---- 整组变换 ----
std::vector<Layer*> Scene::group_movers(std::vector<Layer*>* covered) const {
    std::vector<Pick> picks = selection.picks;
    std::vector<Layer*> movers;
    covered->clear();
    for (const Pick& pick : picks) {
        if (pick.layer == nullptr) continue;
        Layer* candidate = pick.layer;
        bool inside = false;
        for (const Layer* moved : *covered) {
            for (Layer* descendant : moved->descendants()) {
                if (descendant == candidate) inside = true;
            }
            if (moved == candidate) inside = true;
        }
        if (inside) continue;
        movers.push_back(candidate);
        covered->push_back(candidate);
    }
    return movers;
}

void Scene::move_group(double dx, double dy) {
    // BBOX 中心住在图层图像坐标，世界位移要按该图层缩放折算
    for (const Pick& pick : selection.regions()) {
        if (pick.region == nullptr || pick.layer == nullptr) continue;
        const double f = 1.0 / std::max(1e-9, pick.layer->scale);
        pick.region->cx += dx * f;
        pick.region->cy += dy * f;
    }
    std::vector<Layer*> covered;
    for (Layer* layer : group_movers(&covered)) layer->move_by(dx, dy);
}

void Scene::scale_group(double factor) {
    for (const Pick& pick : selection.regions()) {
        if (pick.region == nullptr || pick.layer == nullptr) continue;
        const double rmax = std::max<double>(static_cast<double>(pick.layer->w),
                                             static_cast<double>(pick.layer->h));
        pick.region->rx = std::min(
            std::max(pick.region->rx * factor, cfg::RADIUS_MIN), rmax);
        pick.region->ry = std::min(
            std::max(pick.region->ry * factor, cfg::RADIUS_MIN), rmax);
    }
    std::vector<std::string> keys;
    for (Layer* layer : selection.layers()) {
        layer->set_scale(layer->scale * factor);
    }
    state.mark_physics_dirty();
}

void Scene::delete_selection() {
    if (!selection.multi()) {
        if (selection.region() != nullptr) {
            remove_bbox(selection.layer(), selection.region());
        } else if (selection.layer() != nullptr) {
            delete_layer(selection.layer());
        }
        return;
    }
    push_undo();
    std::vector<Pick> picks = selection.picks;
    for (const Pick& pick : picks) {
        if (pick.region != nullptr && pick.layer != nullptr) {
            const int region_uid = pick.region->uid;
            Layer* layer = state.doc->layer_of(pick.region);
            if (layer != nullptr) {
                selection.drop_region(pick.region);
                state.remove_bbox(layer, pick.region);
                (void)region_uid;
            }
        }
    }
    for (const Pick& pick : picks) {
        if (pick.region != nullptr || pick.layer == nullptr) continue;
        if (!selection.has_layer(pick.layer)) continue;
        selection.drop_layer(pick.layer);
        state.remove_layer(pick.layer);
    }
    state.refresh_regions();
    rebuild_views();
    layer_panel->rebuild();
    param_panel->show_region(nullptr);
    param_panel->set_has_layers(has_layers());
}

int Scene::region_index(const Region* region) const {
    if (region == nullptr || state.doc == nullptr) return 0;
    const auto all = state.doc->all_regions();
    for (size_t i = 0; i < all.size(); ++i) {
        if (all[i] == region) return static_cast<int>(i);
    }
    return 0;
}

cfg::Rgba Scene::region_color(const Region* region) const {
    const int index = region_index(region);
    return cfg::REGION_COLORS[static_cast<size_t>(index)
                              % cfg::REGION_COLORS.size()];
}

// ---- 撤销 / 重做 ----
// 两个栈存的是同一种快照（AppState::snapshot 会深拷贝整份文档 + 导出/循环参数）。
// 新操作压栈时重做链立刻失效，这是"分支历史"的常规口径，不是丢失。
void Scene::push_undo() {
    undo_stack.push_back(state.snapshot());
    if (static_cast<int>(undo_stack.size()) > cfg::UNDO_LIMIT) {
        undo_stack.erase(undo_stack.begin());
    }
    redo_stack.clear();
    drag_undo_pushed = true;
}

void Scene::undo() {
    if (exporting_) {
        set_status("导出中不能撤销");
        return;
    }
    if (undo_stack.empty()) {
        set_status("没有可撤销的操作");
        return;
    }
    // 一笔橡皮还在手上：撤销的语义是"取消这一笔"。落笔时压过一条快照，
    // 它和当前状态一模一样，弹掉才不会让下一次撤销多走空步。
    if (eraser_active_) {
        eraser_abort();
        if (!undo_stack.empty()) undo_stack.pop_back();
        set_status("已取消这一笔橡皮");
        return;
    }
    // 换文档前先把"指向旧文档对象"的活动状态断掉：拖动中的图层/形变图层/套索
    // 都是裸指针，快照一换就成了悬空指针（原来 Ctrl+Z 也有这个隐患，一并处理）。
    cancel_bbox_drag();
    transform_mode = false;
    transform_layer = nullptr;

    const auto uids = selection.uids();
    redo_stack.push_back(state.snapshot());
    if (static_cast<int>(redo_stack.size()) > cfg::UNDO_LIMIT) {
        redo_stack.erase(redo_stack.begin());
    }
    UndoSnapshot snap = std::move(undo_stack.back());
    undo_stack.pop_back();
    state.restore(snap);
    selection.resolve(*state.doc, uids);
    rebuild_views();
    rebuild_panels();
    set_status("已撤销");
}

void Scene::redo() {
    if (exporting_) {
        set_status("导出中不能重做");
        return;
    }
    if (redo_stack.empty()) {
        set_status("没有可重做的操作");
        return;
    }
    // 同理：笔画还没收尾时先把它丢掉，否则换文档后 eraser_layer_ 就是悬空的
    if (eraser_active_) {
        eraser_abort();
        if (!undo_stack.empty()) undo_stack.pop_back();
        set_status("已取消这一笔橡皮");
        return;
    }
    cancel_bbox_drag();
    transform_mode = false;
    transform_layer = nullptr;

    const auto uids = selection.uids();
    undo_stack.push_back(state.snapshot());
    if (static_cast<int>(undo_stack.size()) > cfg::UNDO_LIMIT) {
        undo_stack.erase(undo_stack.begin());
    }
    UndoSnapshot snap = std::move(redo_stack.back());
    redo_stack.pop_back();
    state.restore(snap);
    selection.resolve(*state.doc, uids);
    rebuild_views();
    rebuild_panels();
    set_status("已重做");
}

// ---- 视图 ----
void Scene::rebuild_views() {
    layer_views.rebuild(*state.doc, assets);
    sync_views();
}

// 形变永远由同一个开关驱动：X 隐藏 BBOX 与形变（图像照旧显示，只是不再呼吸）。
void Scene::sync_views() { layer_views.sync(!x_hidden); }

void Scene::fit_view() {
    const Rect canvas = canvas_rect();
    const auto edges = canvas_edges();
    const double avail_w = std::max(1.0, edges.second - edges.first);
    const double avail_h = std::max(1.0, static_cast<double>(height_) - 40.0);
    const double frame_w = std::max(1e-6, canvas.x1 - canvas.x0);
    const double frame_h = std::max(1e-6, canvas.y1 - canvas.y0);
    const double scale = std::min(avail_w / frame_w, avail_h / frame_h)
                         * cfg::FIT_MARGIN;
    view.scale = std::min(std::max(scale, cfg::ZOOM_MIN), cfg::ZOOM_MAX);
    view.x = edges.first + (avail_w - frame_w * view.scale) / 2.0
             - canvas.x0 * view.scale;
    view.y = static_cast<double>(height_) / 2.0
             - (canvas.y0 + frame_h / 2.0) * view.scale;
}

void Scene::toggle_x() {
    x_hidden = !x_hidden;
    if (x_hidden) cancel_bbox_drag();
    // Python 口径（breathloop/ui/scene.py:553-560）：X 只隐藏 BBOX 与形变，
    // 图像照旧画，只是不呼吸；不是"隐藏图层"。
    sync_views();
    set_status(x_hidden ? "X 模式：隐藏 BBOX 与形变"
                        : "X 模式：恢复 BBOX 与形变");
}

void Scene::toggle_bbox_overlay() {
    bbox_hidden = !bbox_hidden;
    set_status(bbox_hidden ? "已隐藏 BBOX 覆盖层（空格恢复）"
                           : "已显示 BBOX 覆盖层");
}

void Scene::cancel_bbox_drag() {
    drag_kind.clear();
    drag_layer = nullptr;
    drag_region = nullptr;
    drag_handle.clear();
    drag_undo_pushed = false;
}

// ---- 自由变换（Ctrl+T） ----
void Scene::begin_transform() {
    if (transform_mode) return;
    if (is_lasso_mode()) set_tool(Tool::kNone);
    Layer* layer = selected_layer();
    if (layer == nullptr || !layer->is_visible_chain()) {
        set_status("先选中一个图层再自由变换");
        return;
    }
    begin_transform_for(layer);
}

void Scene::begin_transform_for(Layer* layer) {
    if (transform_mode || layer == nullptr || !layer->is_visible_chain()) return;
    if (is_lasso_mode()) set_tool(Tool::kNone);
    if (!selection.has_layer(layer)) select_layer(layer);
    transform_mode = true;
    transform_layer = layer;
    transform_start_x = layer->x;
    transform_start_y = layer->y;
    transform_start_sx = layer->sx;
    transform_start_sy = layer->sy;
    transform_start_rot = layer->rot;
    // 记下父层与子层的起点：拖动中每帧都从起点重算，不会一层层叠加漂移
    transform_basis_.x = layer->x;
    transform_basis_.y = layer->y;
    transform_basis_.kx = layer->scale_x();
    transform_basis_.ky = layer->scale_y();
    transform_basis_.rot = layer->rot;
    transform_kids_.clear();
    for (Layer* kid : layer->descendants()) {
        KidBasis basis;
        basis.layer = kid;
        basis.x = kid->x;
        basis.y = kid->y;
        basis.sx = kid->sx;
        basis.sy = kid->sy;
        basis.rot = kid->rot;
        transform_kids_.push_back(basis);
    }
    const Rect rect = layer->world_rect();
    transform_rect_w = std::max(1e-6, rect.x1 - rect.x0);
    transform_rect_h = std::max(1e-6, rect.y1 - rect.y0);
    set_status("自由变换：拖角缩放（Ctrl 解除比例）、拖图外旋转、拖图内移动，回车提交");
}

void Scene::end_transform(bool commit) {
    if (!transform_mode || transform_layer == nullptr) return;
    Layer* layer = transform_layer;
    if (!commit) {
        layer->x = transform_start_x;
        layer->y = transform_start_y;
        layer->sx = transform_start_sx;
        layer->sy = transform_start_sy;
        layer->rot = transform_start_rot;
        carry_transform_children();   // 子层跟着退回起点
        state.mark_physics_dirty();
        sync_views();
        set_status("已放弃变换");
    } else {
        set_status("变换已提交");
    }
    transform_mode = false;
    transform_layer = nullptr;
    guide_x = false;
    guide_y = false;
    drag_undo_pushed = false;
    drag_kind.clear();
    drag_layer = nullptr;
    transform_kids_.clear();
}

// 父层在变形中（缩放/旋转/拖动），子层跟着走：位置绕父中心按同一变换搬，
// 自己的 sx/sy 乘同一对比例，旋转量加上父层的旋转差。全部按"变形开始时"的
// 起点重算，连续拖动不会累积误差。
void Scene::carry_transform_children() {
    if (!transform_mode || transform_layer == nullptr) return;
    if (transform_kids_.empty()) return;
    Layer* layer = transform_layer;
    const double kx = layer->scale_x();
    const double ky = layer->scale_y();
    const double c0 = std::cos(transform_basis_.rot);
    const double s0 = std::sin(transform_basis_.rot);
    const double okx = std::fabs(transform_basis_.kx) > 1e-9
                           ? transform_basis_.kx : 1.0;
    const double oky = std::fabs(transform_basis_.ky) > 1e-9
                           ? transform_basis_.ky : 1.0;
    const double ratio_x = kx / okx;
    const double ratio_y = ky / oky;
    const double d_rot = layer->rot - transform_basis_.rot;
    const double half_w = static_cast<double>(layer->w) * 0.5;
    const double half_h = static_cast<double>(layer->h) * 0.5;
    for (const KidBasis& basis : transform_kids_) {
        if (basis.layer == nullptr) continue;
        // 子层起点 → 父层图像坐标（用起点变换反解），再进父层新变换
        const double dx = basis.x - transform_basis_.x;
        const double dy = basis.y - transform_basis_.y;
        const double px = (dx * c0 + dy * s0) / okx + half_w;
        const double py = (-dx * s0 + dy * c0) / oky + half_h;
        const auto world = layer->to_world(px, py);
        basis.layer->x = world.first;
        basis.layer->y = world.second;
        basis.layer->sx = basis.sx * ratio_x;
        basis.layer->sy = basis.sy * ratio_y;
        basis.layer->rot = basis.rot + d_rot;
    }
    state.mark_physics_dirty();
}

// 吸附：把移动后的包围盒的左/中/右、上/中/下 六条线，对齐到
// 画布四边+中心、以及其它可见图层的四边+中心（参照 ImageEditor
// app/Editor.cpp:1784-1853 的 snapMoveDelta：同样的三锚点×目标集合）。
// 只在按住 Ctrl 时生效：不按就是自由拖动，不会被 2px 阈值悄悄拽走。
void Scene::snap_move(Layer* layer, double* dx, double* dy, bool allow_snap) {
    guide_x = false;
    guide_y = false;
    if (!allow_snap) return;
    if (layer == nullptr || dx == nullptr || dy == nullptr) return;
    const Rect r = layer->world_rect();
    if (r.x1 <= r.x0 || r.y1 <= r.y0) return;
    const double mx = *dx;
    const double my = *dy;
    const Rect moved{r.x0 + mx, r.y0 + my, r.x1 + mx, r.y1 + my};

    const Rect canvas = canvas_rect();
    std::vector<double> xs{canvas.x0, 0.0, canvas.x1};
    std::vector<double> ys{canvas.y0, 0.0, canvas.y1};
    for (Layer* other : state.doc->flatten()) {
        if (other == layer || !other->is_visible_chain()) continue;
        const Rect b = other->world_rect();
        if (b.x1 <= b.x0 || b.y1 <= b.y0) continue;
        xs.push_back(b.x0);
        xs.push_back((b.x0 + b.x1) * 0.5);
        xs.push_back(b.x1);
        ys.push_back(b.y0);
        ys.push_back((b.y0 + b.y1) * 0.5);
        ys.push_back(b.y1);
    }

    const double tol = cfg::GUIDE_SNAP_PX / std::max(view.scale, 1e-6);
    const double anchors_x[3] = {moved.x0, (moved.x0 + moved.x1) * 0.5, moved.x1};
    const double anchors_y[3] = {moved.y0, (moved.y0 + moved.y1) * 0.5, moved.y1};

    double best_x = tol + 1.0;
    double best_y = tol + 1.0;
    double fix_x = 0.0;
    double fix_y = 0.0;
    for (double a : anchors_x) {
        for (double t : xs) {
            const double d = std::abs(t - a);
            if (d <= tol && d < best_x) {
                best_x = d;
                fix_x = t - a;
                guide_wx = t;
            }
        }
    }
    for (double a : anchors_y) {
        for (double t : ys) {
            const double d = std::abs(t - a);
            if (d <= tol && d < best_y) {
                best_y = d;
                fix_y = t - a;
                guide_wy = t;
            }
        }
    }
    if (best_x <= tol) {
        *dx += fix_x;
        guide_x = true;
    }
    if (best_y <= tol) {
        *dy += fix_y;
        guide_y = true;
    }
}

// ---- 工具 ----
void Scene::set_tool(Tool tool) {
    if (tool_ == tool) return;
    // 先收笔：橡皮拖到一半就收回这一笔（还原实时预览改过的纹理，不留半截），
    // 套索画到一半就丢路径（本来就没提交过像素）。
    if (eraser_active_) eraser_abort();
    board_capture_ = false;
    // 离开套索模式：未提交的路径直接丢；已经闭合生效的路径保留（切到橡皮才清）
    if (tool_ == Tool::kLasso) {
        lasso_path.clear();
        lasso_closed_ = false;
    }
    if (tool == Tool::kEraser) {   // 橡皮模式下不存在套索
        lasso_path.clear();
        lasso_closed_ = false;
    }
    tool_ = tool;
    switch (tool_) {
        case Tool::kEraser:
            set_status("橡皮：拖动擦除选中图层（半径 "
                       + std::to_string(state.eraser_diameter / 2) + "，羽化 "
                       + std::to_string(state.eraser_feather) + "%）");
            break;
        case Tool::kLasso:
            set_status("多边形套索：左键加点，双击闭合，Delete 退点，E 切回橡皮");
            break;
        case Tool::kNone:
            set_status("橡皮已关闭");
            break;
    }
    update_cursor();
}

// ---- 套索 ----
void Scene::reset_lasso() {
    lasso_path.clear();
    lasso_closed_ = false;
    if (tool_ == Tool::kLasso) tool_ = Tool::kNone;
}

Scene::LassoPhase Scene::lasso_phase() const {
    if (tool_ == Tool::kLasso) return LassoPhase::kDrawing;
    if (lasso_closed_ && lasso_path.size() >= static_cast<size_t>(kMinLassoPoints))
        return LassoPhase::kClosed;
    return LassoPhase::kNone;
}

// 套索的作用图层只由两件事决定：眼睛（可见性）和图层上下顺序。
// 选中层可见就用它；选中层不可见（或没选中）就顺延到最上面的可见层。
Layer* Scene::lasso_target() const {
    if (state.doc == nullptr) return nullptr;
    Layer* sel = selected_layer();
    if (sel != nullptr && sel->is_visible_chain()) return sel;
    Layer* top = nullptr;
    for (Layer* layer : state.doc->flatten()) {   // 底 → 顶，最后一个可见的就是最上面
        if (layer->is_visible_chain()) top = layer;
    }
    return top;
}

// 世界坐标路径 → 该图层自己的图像坐标（mask/cutout 那套既有算法只认本地坐标）
bool Scene::local_lasso(const Layer& layer, Lasso* out) const {
    if (out == nullptr || lasso_path.empty()) return false;
    out->points.clear();
    out->points.reserve(lasso_path.size());
    for (const auto& p : lasso_path) {
        const auto local = layer.from_world(p.first, p.second);
        out->points.push_back(Point{local.first, local.second});
    }
    return out->points.size() >= static_cast<size_t>(kMinLassoPoints);
}

void Scene::toggle_lasso_mode() {
    if (tool_ == Tool::kLasso) {   // A 在套索模式里再按：丢掉这一笔
        cancel_lasso();
        return;
    }
    // 走 set_tool：从橡皮切过来时先把橡皮那一笔收掉（还原纹理、不落半截）
    set_tool(Tool::kLasso);
    lasso_path.clear();
    lasso_closed_ = false;
    const Layer* target = lasso_target();
    set_status(target != nullptr
                   ? "多边形套索：左键加点，双击闭合，Delete 退点，A 取消"
                   : "多边形套索：当前没有可见图层，先点开某个图层的眼睛");
}

void Scene::add_lasso_point(double wx, double wy) {
    if (tool_ != Tool::kLasso) return;
    lasso_path.emplace_back(wx, wy);
}

void Scene::remove_lasso_point() {
    if (lasso_path.empty()) return;  // 没有点时按 Delete 不退任何东西
    lasso_path.pop_back();
}

void Scene::commit_lasso() {
    if (lasso_path.empty()) {
        cancel_lasso();
        return;
    }
    if (static_cast<int>(lasso_path.size()) < kMinLassoPoints) {
        set_status("套索至少需要 3 个点");
        return;
    }
    if (lasso_target() == nullptr) {
        set_status("当前没有可见图层，套索无处生效");
        return;
    }
    push_undo();
    lasso_closed_ = true;
    tool_ = Tool::kNone;   // 提交后退出套索模式，路径留在场景里继续生效
    selection_changed();
    set_status("套索已生效（A 再次按下取消并清除）");
}

void Scene::cancel_lasso() {
    reset_lasso();
    set_status("已取消套索");
}

// 双击 / A（第二次按下）：把已经生效的套索整个清掉（不动像素）
void Scene::clear_lasso() {
    if (!lasso_closed_) {
        set_status("当前没有套索");
        return;
    }
    push_undo();
    lasso_path.clear();
    lasso_closed_ = false;
    selection_changed();
    set_status("已清除套索");
}

// 粘贴之后收尾：套索这一趟用完就清，工具退回空态。
// 不 push_undo —— 套索是 UI 选区，不进文档快照。
void Scene::finish_lasso() {
    if (tool_ == Tool::kLasso) {
        lasso_path.clear();
        lasso_closed_ = false;
        tool_ = Tool::kNone;
        selection_changed();
        return;
    }
    if (lasso_closed_) {
        lasso_path.clear();
        lasso_closed_ = false;
        selection_changed();
    }
}

bool Scene::erase_lasso_area() {
    Layer* layer = lasso_target();
    Lasso lasso;
    if (layer == nullptr || !lasso_closed_ || !local_lasso(*layer, &lasso)) {
        return false;
    }
    Asset& asset = assets.get(layer->asset_key);
    const auto rgba = asset.rgba();
    if (rgba.empty()) return false;
    const auto pixels = erase_inside(rgba, asset.width, asset.height, lasso);
    const int aw = asset.width;
    const int ah = asset.height;

    // 剪完之后把位图裁到"剩下的内容"的包围盒（四周补 pad）：
    // 位图若还留着原来那整幅，四角点就画在透明空处，看着跟内容对不上。
    // 裁的时候按像素原尺寸搬移，世界位置不变（内容像素映射完全一致）。
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    alpha_bounds(pixels.data(), aw, ah, &x0, &y0, &x1, &y1);
    // 子层也照裁：子层的位置也是世界坐标，新位图中心同样按旧变换求世界点对齐，
    // 所以不存在"只对顶层生效"的理由。以前把子层挡在外面，剪完四角点就落在
    // 原来的空白位图边上，看着跟剩下的内容对不上。
    const bool can_crop =
        x1 > x0 && y1 > y0 && layer->w == aw && layer->h == ah;
    std::vector<unsigned char> cropped;
    int nw = aw;
    int nh = ah;
    int pd = 0;
    if (can_crop) {
        pd = std::max(0, static_cast<int>(std::rint(cfg::CUTOUT_PAD)));
        nw = (x1 - x0) + 2 * pd;
        nh = (y1 - y0) + 2 * pd;
        cropped.assign(static_cast<size_t>(nw) * nh * 4, 0);
        for (int row = y0; row < y1; ++row) {
            const unsigned char* src =
                pixels.data() + (static_cast<size_t>(row) * aw + x0) * 4;
            unsigned char* dst = cropped.data() +
                                 (static_cast<size_t>(row - y0 + pd) * nw + pd) * 4;
            std::copy(src, src + static_cast<size_t>(x1 - x0) * 4, dst);
        }
    }
    push_undo();
    std::string path = layer->path;
    const auto dot = path.find_last_of('.');
    const std::string base = dot == std::string::npos ? path : path.substr(0, dot);
    Asset* next = nullptr;
    if (can_crop) {
        next = &assets.put(base + ".erase" + std::to_string(layer->uid) + ".png",
                           std::move(cropped), nw, nh);
    } else {
        next = &assets.put(base + ".erase" + std::to_string(layer->uid) + ".png",
                           pixels, nw, nh);
    }
    if (can_crop) {
        // 新位图中心 = 被裁内容的中心：按旧变换求它原来的世界位置，中心对齐即可
        const double ccx = (static_cast<double>(x0) + x1) * 0.5;
        const double ccy = static_cast<double>(ah)
                           - (static_cast<double>(y0) + y1) * 0.5;
        const auto world = layer->to_world(ccx, ccy);   // 旧 w/h/rot/sx/sy
        layer->w = nw;
        layer->h = nh;
        layer->pad = pd;
        layer->x = world.first;
        layer->y = world.second;
    }
    layer->asset_key = next->key;
    layer->path = next->path;
    layer->name = next->name;
    state.mark_physics_dirty();
    rebuild_views();
    layer_panel->rebuild();
    return true;
}

// ---- 橡皮 ----
void Scene::set_eraser(bool on) {
    if (on == eraser_on()) return;
    if (on) {
        if (transform_mode) end_transform(false);
        set_tool(Tool::kEraser);   // 进橡皮时把套索一并清掉
    } else {
        // E 键可能在一笔中间按下：先把这一笔收尾（落成资产、进撤销），再关工具
        if (eraser_active_) eraser_release();
        tool_ = Tool::kNone;
        set_status("橡皮已关闭");
    }
}

void Scene::eraser_stamp_at(double lx, double ly) {
    if (!eraser_active_ || eraser_w_ <= 0 || eraser_h_ <= 0) return;
    const double radius = std::max(0.5, state.eraser_diameter * 0.5);
    // 羽化是百分比（0 = 硬边，100 = 从圆心就开始过渡），换算成像素带宽度。
    // 与别的编辑器 0.00–1.00 的硬边/软边一个含义。
    const double feather_pct =
        std::min(100.0, std::max(0.0, static_cast<double>(state.eraser_feather)));
    const double feather = radius * feather_pct / 100.0;
    // 图层局部 y 向上，位图行自顶向下：翻一次再印
    const double row = static_cast<double>(eraser_h_) - ly;
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    // 只往覆盖率遮罩上盖印：同一笔内取最大值，重叠不会把软边越擦越硬
    if (!stamp_coverage(eraser_cov_, eraser_w_, eraser_h_, lx, row, radius,
                        feather, &x0, &y0, &x1, &y1)) {
        return;
    }
    if (eraser_dirty_[2] <= eraser_dirty_[0]) {
        eraser_dirty_[0] = x0;
        eraser_dirty_[1] = y0;
        eraser_dirty_[2] = x1;
        eraser_dirty_[3] = y1;
        return;
    }
    eraser_dirty_[0] = std::min(eraser_dirty_[0], x0);
    eraser_dirty_[1] = std::min(eraser_dirty_[1], y0);
    eraser_dirty_[2] = std::max(eraser_dirty_[2], x1);
    eraser_dirty_[3] = std::max(eraser_dirty_[3], y1);
}

void Scene::eraser_flush() {
    if (!eraser_active_ || eraser_layer_ == nullptr) return;
    if (eraser_dirty_[2] <= eraser_dirty_[0]
        || eraser_dirty_[3] <= eraser_dirty_[1]) {
        return;
    }
    const int x = eraser_dirty_[0];
    const int y = eraser_dirty_[1];
    const int w = eraser_dirty_[2] - x;
    const int h = eraser_dirty_[3] - y;
    // 遮罩 → 结果：alpha = 本笔基线 alpha × (1 - 覆盖率)。基线整笔不变，遮罩在
    // 同一笔里取最大值，所以笔迹稳定、不随拖动速度变；跨笔擦除才继续累积。
    if (eraser_cov_.size() == eraser_base_.size() / 4
        && eraser_base_.size() == eraser_buf_.size()) {
        for (int yy = y; yy < y + h; ++yy) {
            for (int xx = x; xx < x + w; ++xx) {
                const size_t p = static_cast<size_t>(yy) * eraser_w_ + xx;
                const unsigned char c = eraser_cov_[p];
                if (c == 0) continue;
                const unsigned char base = eraser_base_[p * 4 + 3];
                // (base * (255 - c) + 127) / 255：四舍五入的定点乘
                const int keep = 255 - static_cast<int>(c);
                eraser_buf_[p * 4 + 3] = static_cast<unsigned char>(
                    (static_cast<int>(base) * keep + 127) / 255);
            }
        }
    }
    Asset& asset = assets.get(eraser_layer_->asset_key);
    if (asset.texture && asset.texture->valid()) {
        const unsigned char* pixels =
            eraser_buf_.data() + (static_cast<size_t>(y) * eraser_w_ + x) * 4;
        asset.texture->update_region(x, y, w, h, pixels, 4, eraser_w_);
    }
    eraser_dirty_[0] = eraser_dirty_[1] = 0;
    eraser_dirty_[2] = eraser_dirty_[3] = 0;
}

bool Scene::eraser_press(double x, double y) {
    if (!eraser_on()) return false;
    if (exporting_) {
        // 导出逐帧取当前文档：中途改像素会让帧之间不一致
        set_status("导出中不能擦除");
        return true;
    }
    const auto world = window_to_world(x, y);
    // 橡皮是"笔刷"，按 PS 口径作用在选中图层上；落点在选中层矩形内就用它，
    // 不再靠 alpha 掩膜去猜（擦到透明洞上时 LayerAt 会命中失败，一笔就白拖）。
    Layer* layer = nullptr;
    Layer* sel = selected_layer();
    if (sel != nullptr && sel->is_visible_chain()) {
        const auto l = sel->from_world(world.first, world.second);
        if (sel->local_contains(l.first, l.second)) layer = sel;
    }
    if (layer == nullptr) layer = input::LayerAt(*this, world.first, world.second);
    if (layer == nullptr) {
        set_status("橡皮：指针下没有像素");
        return true;
    }
    Asset& asset = assets.get(layer->asset_key);
    auto rgba = asset.rgba();
    if (rgba.empty()) {
        set_status("橡皮：这个图层的像素不可用");
        return true;
    }
    // 一笔一条撤销：落笔前压一次，拖动过程中不再压
    push_undo();
    if (!selection.has_layer(layer)) select_layer(layer);
    eraser_layer_ = layer;
    eraser_w_ = asset.width;
    eraser_h_ = asset.height;
    eraser_base_ = rgba;            // 本笔基线：整笔只读，合成时当乘数用
    eraser_buf_ = std::move(rgba);  // 结果缓冲（预览与落盘都用它）
    eraser_cov_.assign(static_cast<size_t>(eraser_w_) * eraser_h_, 0);
    eraser_dirty_[0] = eraser_dirty_[1] = 0;
    eraser_dirty_[2] = eraser_dirty_[3] = 0;
    eraser_active_ = true;
    const auto local = layer->from_world(world.first, world.second);
    eraser_last_lx_ = local.first;
    eraser_last_ly_ = local.second;
    eraser_stamp_at(local.first, local.second);
    eraser_flush();
    return true;
}

void Scene::eraser_drag(double x, double y) {
    if (!eraser_active_ || eraser_layer_ == nullptr) return;
    const auto world = window_to_world(x, y);
    const auto local = eraser_layer_->from_world(world.first, world.second);
    const double radius = std::max(0.5, state.eraser_diameter * 0.5);
    // 两点之间按半径的 1/4 补印：快速拖动也不会断成一串点
    const double step = std::max(1.0, radius * 0.25);
    const double dx = local.first - eraser_last_lx_;
    const double dy = local.second - eraser_last_ly_;
    const double dist = std::sqrt(dx * dx + dy * dy);
    const int count = std::max(1, static_cast<int>(dist / step));
    for (int i = 1; i <= count; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(count);
        eraser_stamp_at(eraser_last_lx_ + dx * t, eraser_last_ly_ + dy * t);
    }
    eraser_last_lx_ = local.first;
    eraser_last_ly_ = local.second;
    eraser_flush();
}

void Scene::eraser_release() {
    if (!eraser_active_) return;
    eraser_flush();
    Layer* layer = eraser_layer_;
    if (layer != nullptr && eraser_w_ > 0 && eraser_h_ > 0) {
        const int old_key = layer->asset_key;
        // 一笔擦完才落成一个新资产：撤销栈里旧资产还在，能整笔回退
        std::string path = layer->path;
        const auto dot = path.find_last_of('.');
        const std::string base =
            dot == std::string::npos ? path : path.substr(0, dot);
        Asset& next = assets.put(
            base + ".erasebrush" + std::to_string(layer->uid) + ".png",
            std::move(eraser_buf_), eraser_w_, eraser_h_);
        layer->asset_key = next.key;
        layer->path = next.path;
        layer->name = next.name;
        // 拖动时是直接改旧资产的纹理（实时预览）。撤销要回到旧资产，那张纹理
        // 必须还原成落笔前的像素，否则撤销回去了却还是"擦掉的样子"。
        if (old_key != next.key
            && eraser_base_.size()
                   == static_cast<size_t>(eraser_w_) * eraser_h_ * 4) {
            Asset& old = assets.get(old_key);
            if (old.texture && old.texture->valid()
                && old.width == eraser_w_ && old.height == eraser_h_) {
                old.texture->update_region(0, 0, eraser_w_, eraser_h_,
                                           eraser_base_.data(), 4);
            }
        }
        state.mark_physics_dirty();
        rebuild_views();
        layer_panel->rebuild();
        set_status("已擦除一笔（Ctrl+Z 撤销 / Ctrl+Shift+Z 重做）");
    }
    eraser_active_ = false;
    eraser_layer_ = nullptr;
    eraser_buf_.clear();
    eraser_base_.clear();
    eraser_cov_.clear();
    eraser_w_ = 0;
    eraser_h_ = 0;
    drag_undo_pushed = false;
}

void Scene::eraser_abort() {
    if (!eraser_active_) return;
    // 实时预览直接改过资产纹理：撤销/重做换文档前必须把它还原成本笔起点的像素
    if (eraser_layer_ != nullptr && eraser_w_ > 0 && eraser_h_ > 0
        && eraser_base_.size() == static_cast<size_t>(eraser_w_) * eraser_h_ * 4) {
        Asset& asset = assets.get(eraser_layer_->asset_key);
        if (asset.texture && asset.texture->valid()
            && asset.width == eraser_w_ && asset.height == eraser_h_) {
            asset.texture->update_region(0, 0, eraser_w_, eraser_h_,
                                         eraser_base_.data(), 4);
        }
    }
    eraser_active_ = false;
    eraser_layer_ = nullptr;
    eraser_buf_.clear();
    eraser_base_.clear();
    eraser_cov_.clear();
    eraser_w_ = 0;
    eraser_h_ = 0;
    eraser_dirty_[0] = eraser_dirty_[1] = 0;
    eraser_dirty_[2] = eraser_dirty_[3] = 0;
    drag_undo_pushed = false;
}

bool Scene::cut_out_to_clipboard() {
    Layer* layer = lasso_target();
    Lasso lasso;
    if (layer == nullptr || !lasso_closed_ || !local_lasso(*layer, &lasso)) {
        return false;
    }
    Asset& asset = assets.get(layer->asset_key);
    const auto rgba = asset.rgba();
    if (rgba.empty()) return false;
    const auto piece = cut_out(rgba, asset.width, asset.height, lasso);
    if (!piece.has_value()) {
        set_status("套索与图像没有交集");
        return false;
    }
    std::string path = layer->path;
    const auto dot = path.find_last_of('.');
    const std::string base = dot == std::string::npos ? path : path.substr(0, dot);
    Asset& next = assets.put(base + ".cut" + std::to_string(layer->uid) + ".png",
                             piece->pixels, piece->width, piece->height);

    LayerClip clip;
    clip.name = next.name;
    clip.path = next.path;
    clip.asset_key = next.key;
    clip.w = next.width;
    clip.h = next.height;
    clip.scale = layer->scale;
    clip.sx = layer->sx;
    clip.sy = layer->sy;
    clip.rot = layer->rot;
    clip.visible = true;
    // 抠出来的小块四周带透明补边（cutout.cpp 的 kPad），变形手柄要按内容框画
    clip.pad = static_cast<int>(cfg::CUTOUT_PAD);
    // 抠出来的小块是「套索外接框（含两侧等量补边）」，它的中心是裁框中心，
    // 不是整个图层的中心；先把裁框中心换回图层局部坐标（y 向上），再进世界。
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    crop_bounds(lasso, asset.width, asset.height, &left, &top, &right,
                &bottom);
    const double local_x = (static_cast<double>(left) + right) * 0.5;
    const double local_y = static_cast<double>(asset.height)
                           - (static_cast<double>(top) + bottom) * 0.5;
    const auto world = layer->to_world(local_x, local_y);
    clip.x = world.first;
    clip.y = world.second;
    clip.in_place = true;
    clipboard_layer = std::make_unique<LayerClip>(clip);
    clipboard_region.reset();
    return true;
}

// ---- 图层 ----
bool Scene::has_layers() const {
    return state.doc != nullptr && !state.doc->flatten().empty();
}

void Scene::add_layer(const std::string& path) {
    // 路径是 UTF-8，exists 必须显式按 Unicode 走（path(char*) 在 MSVC 上按 ANSI 解，中文名会假报不存在）
    if (!std::filesystem::exists(paths::Utf8Path(path))) {
        set_status("文件不存在：" + path);
        return;
    }
    Asset* asset = nullptr;
    try {
        asset = &assets.load(path);
    } catch (const std::exception& error) {
        set_status(std::string("无法读取图片：") + error.what());
        return;
    }
    // 画布尺寸跟着"第一张进空文档的图"走：加入前文档没有图层，就按这张图重取。
    // 不是"本次运行只取一次"——删空之后拖进来的图同样按自己的尺寸改画布。
    const bool canvas_from_new_doc = !has_layers();
    if (canvas_from_new_doc) {
        set_canvas_size(asset->width, asset->height);
        fit_view();
    }
    push_undo();
    // 落在画布中心
    double cx = 0.0;
    double cy = 0.0;
    canvas_center(&cx, &cy);
    auto layer = state.doc->new_layer(asset->name, asset->path, asset->key,
                                      asset->width, asset->height, cx, cy);
    Layer* added = state.doc->add_layer_top(std::move(layer));
    state.refresh_regions();
    rebuild_views();
    select_layer(added);
    param_panel->set_has_layers(true);
    param_panel->refresh_export_size();
    set_status("已添加 " + added->name);
}

void Scene::canvas_center(double* x, double* y) const {
    // 画布中心就是世界原点
    *x = 0.0;
    *y = 0.0;
}

void Scene::delete_layer(Layer* layer) {
    if (layer == nullptr) return;
    push_undo();
    selection.drop_layer(layer);
    state.remove_layer(layer);
    state.refresh_regions();
    selection.clear();
    rebuild_views();
    layer_panel->rebuild();
    param_panel->show_region(nullptr);
    param_panel->set_has_layers(has_layers());
}

void Scene::paste_layer(const LayerClip& clip, double wx, double wy) {
    // 粘贴位置跟着"被复制的图层"走，不看光标：套索抠图和普通图层复制是同一个
    // 口径，粘出来的东西落在原来那一块上（再拖才动）。wx/wy 只为旧调用方保留。
    (void)wx;
    (void)wy;
    const double px = clip.x;
    const double py = clip.y;
    push_undo();
    auto result = state.doc->add_layer_copy(clip, px, py);
    state.refresh_regions();
    rebuild_views();
    layer_panel->rebuild();
    if (result.first != nullptr) select_layer(result.first);
    std::string text = "已粘贴图层";
    if (result.second > 0) {
        text += "（" + std::to_string(result.second) + " 个 BBOX 达到上限被跳过）";
    }
    set_status(text);
}

void Scene::move_bbox(Layer* owner, Region* region, Layer* target, int index) {
    if (owner == nullptr || region == nullptr || target == nullptr) return;
    if (state.move_bbox_to(owner, region, target, index)) {
        state.refresh_regions();
        selection_changed();
    }
}

void Scene::reorder_layer(Layer* layer, Layer* parent, int index) {
    if (layer == nullptr || state.doc == nullptr) return;
    if (state.move_layer(layer, parent, index)) {
        state.refresh_regions();
        rebuild_views();
        layer_panel->rebuild();
    }
}

void Scene::toggle_layer_visible(Layer* layer) {
    if (layer == nullptr) return;
    layer->visible = !layer->visible;
    // 图层眼睛是单向的：关掉时它的 BBOX 也一起停（BBOX 自己的开关值不动，
    // 图层重新打开时原来关着的那几个不会自己复活）
    state.refresh_regions();
    sync_views();
    layer_panel->rebuild();
    param_panel->show_region(selected_region());   // 参数跟着变灰/恢复
    param_panel->refresh_export_size();
}

void Scene::toggle_region_visible(Region* region) {
    if (region == nullptr) return;
    region->visible = !region->visible;
    state.refresh_regions();
    sync_views();
    layer_panel->rebuild();
    param_panel->show_region(selected_region());
    param_panel->refresh_export_size();
}

// ---- 图层尺寸（图像卡口径：画布世界像素、按内容框算，不含套索抠图的透明补边）----
std::pair<double, double> Scene::layer_size(const Layer* layer) const {
    if (layer == nullptr) return {0.0, 0.0};
    const double cw = static_cast<double>(std::max(1, layer->w - layer->pad * 2));
    const double ch = static_cast<double>(std::max(1, layer->h - layer->pad * 2));
    return {cw * layer->scale_x(), ch * layer->scale_y()};
}

bool Scene::layer_ratio_locked(const Layer* layer) const {
    if (layer == nullptr) return false;
    // 浮点阈值：拖手柄回来常留 0.99999 的尾巴，用 1e-6 之外的值当"非等比"
    return std::fabs(layer->sx - layer->sy) < 1e-6;
}

// 写回：把横纵倍率直接写成"目标世界尺寸 / 原图像素"，整体缩放归一为 1。
// 以中心为锚（不改 x/y）；父层变小会连带子层（子层坐标住在父图像像素里）。
void Scene::set_layer_size(Layer* layer, double w, double h) {
    if (layer == nullptr || layer->w <= 0 || layer->h <= 0) return;
    w = std::max(1.0, w);
    h = std::max(1.0, h);
    push_undo();
    // 口径与图像卡一致：给的是可见内容的世界宽高，补边不参与
    const double cw = static_cast<double>(std::max(1, layer->w - layer->pad * 2));
    const double ch = static_cast<double>(std::max(1, layer->h - layer->pad * 2));
    // 父层改尺寸，子层要一起承担（含子层自己的缩放）：先把旧变换的量记下来
    const double old_kx = layer->scale_x();
    const double old_ky = layer->scale_y();
    const double ox = layer->x;
    const double oy = layer->y;
    const double rot = layer->rot;
    const double bw = static_cast<double>(layer->w) * 0.5;
    const double bh = static_cast<double>(layer->h) * 0.5;
    layer->scale = 1.0;
    layer->sx = w / cw;
    layer->sy = h / ch;
    const double ratio_x = layer->scale_x() / (std::fabs(old_kx) > 1e-9 ? old_kx : 1.0);
    const double ratio_y = layer->scale_y() / (std::fabs(old_ky) > 1e-9 ? old_ky : 1.0);
    if (std::fabs(ratio_x - 1.0) > 1e-9 || std::fabs(ratio_y - 1.0) > 1e-9) {
        const double c = std::cos(rot);
        const double s = std::sin(rot);
        const double kx = std::fabs(old_kx) > 1e-9 ? old_kx : 1.0;
        const double ky = std::fabs(old_ky) > 1e-9 ? old_ky : 1.0;
        for (Layer* child : layer->descendants()) {
            // 子层的世界位置换算成父层图像坐标，再用新尺寸换回世界：
            // 父层放大多少，子层离父层中心的距离与自己的尺寸就同步放大多少
            const double dx = child->x - ox;
            const double dy = child->y - oy;
            const double px = (dx * c + dy * s) / kx + bw;
            const double py = (-dx * s + dy * c) / ky + bh;
            const auto world = layer->to_world(px, py);
            child->x = world.first;
            child->y = world.second;
            child->sx *= ratio_x;
            child->sy *= ratio_y;
        }
    }
    state.mark_physics_dirty();
    sync_views();
    layer_panel->rebuild();
}

// ↪ 的语义是“把图层摆回画布正中”，不是复位缩放：本项目的画布缩放属于视图，
// 图层的 scale 是对象自身的变形（Ctrl+T 写入 sx/sy），没有“复位”这个业务动作。
// 图层 x/y 一律是世界坐标（父层只负责驱动子层，不构成坐标系）；
// 顶层重置回画布中心 (0,0)，子层重置回父层中心（parent->x/y 即父层世界中心）。
void Scene::center_layer(Layer* layer) {
    if (layer == nullptr) {
        set_status("没有可居中的图层");
        return;
    }
    push_undo();
    const double old_x = layer->x;
    const double old_y = layer->y;
    if (layer->parent != nullptr) {
        layer->x = layer->parent->x;
        layer->y = layer->parent->y;
    } else {
        layer->x = 0.0;
        layer->y = 0.0;
    }
    // 父层被搬走了，整棵子树跟着走：子层的世界坐标按同一个位移平移，
    // 相对父层的关系一点没变（等比、旋转都不用动）。
    const double dx = layer->x - old_x;
    const double dy = layer->y - old_y;
    if (dx != 0.0 || dy != 0.0) {
        for (Layer* kid : layer->descendants()) {
            kid->x += dx;
            kid->y += dy;
        }
    }
    state.mark_physics_dirty();
    sync_views();
    layer_panel->rebuild();
    set_status(layer->parent != nullptr
                   ? "已把「" + layer->name + "」居中到父层"
                   : "已把「" + layer->name + "」居中到画布");
}

void Scene::add_bbox(Layer* layer) {
    if (layer == nullptr) return;
    Region* source = selection.sole_region();
    const double cx = layer->w / 2.0;
    const double cy = layer->h / 2.0;
    if (!state.can_add_bbox()) {
        set_status("BBOX 数量已达上限（" + std::to_string(state.bbox_limit) + "）");
        return;
    }
    push_undo();
    Region* region = state.add_bbox(layer, cx, cy, source);
    if (region == nullptr) {
        undo_stack.pop_back();
        set_status("BBOX 数量已达上限（" + std::to_string(state.bbox_limit) + "）");
        return;
    }
    sync_views();
    layer_panel->rebuild();
    select_region(layer, region);
}

void Scene::remove_bbox(Layer* layer, Region* region) {
    if (layer == nullptr || region == nullptr) return;
    push_undo();
    selection.drop_region(region);
    state.remove_bbox(layer, region);
    state.refresh_regions();
    sync_views();
    layer_panel->rebuild();
    param_panel->show_region(selection.sole_region());
    set_status("已删除 BBOX");
}

// ---- 参数 ----
void Scene::on_param(const std::string& key, double value) {
    Region* region = selection.sole_region();
    if (region != nullptr) {
        double* member = member_by_key(*region, key);
        if (member != nullptr) *member = value;
        state.sync_param_changed(region, key);
        param_panel->refresh_values();
    } else {
        for (Region* other : state.regions) {
            double* member = member_by_key(*other, key);
            if (member != nullptr) *member = value;
        }
    }
    state.mark_physics_dirty();
}

void Scene::on_sync_toggle(bool enabled) {
    state.set_sync(enabled, selection.sole_region());
    param_panel->refresh_values();
}

void Scene::on_preset(const std::string& name) {
    Region* region = selection.sole_region();
    if (region == nullptr) return;
    push_undo();
    region->apply_preset(name);
    state.sync_param_changed(region, "amp");
    state.mark_physics_dirty();
    param_panel->refresh_values();
    set_status("已套用预设 " + name);
}

void Scene::set_status(const std::string& text) {
    status_text_ = text;
    if (status != nullptr) status->set(text);
}

// ---- 导出 ----
std::pair<int, int> Scene::default_export_size() {
    // 导出尺寸 = 画布边界，不再是内容包围盒。
    return canvas_size();
}

bool Scene::ensure_export_size() {
    if (state.doc == nullptr) return false;
    const auto size = canvas_size();
    state.export_w = size.first;
    state.export_h = size.second;
    return state.export_w > 0 && state.export_h > 0;
}

void Scene::start_export() {
    if (!has_layers() || exporting_) {
        set_status("没有可导出的图层");
        return;
    }
    ensure_export_size();
    if (state.export_w <= 0 || state.export_h <= 0) {
        set_status("导出尺寸无效");
        return;
    }

    ExportOptions options;
    options.out_w = state.export_w;
    options.out_h = state.export_h;
    // 导出落点跟 Python 一致：第一个根图层的源文件旁边。
    const auto& roots = state.doc->roots();
    if (!roots.empty()) options.out_base = roots.front()->path;
    options.lossless = state.export_lossless;
    options.quality = state.export_quality;
    options.effort = state.export_effort;
    options.alpha_q = state.export_alpha_q;
    options.ms_per_frame = state.ms_per_frame;

    try {
        exporter_ = std::make_unique<Exporter>(*state.doc, assets, state.frames,
                                               options);
    } catch (const std::exception& error) {
        exporter_.reset();
        set_status(std::string("导出启动失败：") + error.what());
        param_panel->set_status(std::string("导出启动失败：") + error.what());
        return;
    }

    exporting_ = true;
    encode_finished_ = false;
    enc_t0 = glfwGetTime();
    param_panel->set_exporting(true);
    param_panel->set_progress(0, exporter_->total_frames());
    const std::string first = "导出中 0/" + std::to_string(exporter_->total_frames());
    param_panel->set_status(first);
    set_status(first);
}

void Scene::cancel_export() {
    if (!exporting_) return;
    // 编码一旦开始就不再打断：libvips 那一步没有中断点，等它写完比杀线程安全
    if (encode_thread_.joinable()) return;
    exporter_.reset();
    exporting_ = false;
    param_panel->set_exporting(false);
    param_panel->set_status("已取消导出");
    set_status("已取消导出");
}

// 每帧推进一步：GL 渲染在主线程一帧一帧走，渲完把编码交给后台线程，
// 这样导出期间窗口照常刷新、进度条与状态文字都能跟着走。
void Scene::pump_export() {
    if (!exporting_ || exporter_ == nullptr) return;

    if (encode_thread_.joinable()) {
        if (!encode_finished_.load()) {
            // 编码是逐帧喂给 WebPAnimEncoder 的：已编码帧数、总帧数都是真值，
            // 剩余时间 = 剩余帧数 ÷ 实测每秒编码帧数（刚开始没有样本就只说已用时）。
            const int edone = exporter_->encode_done();
            const int etotal = exporter_->encode_total();
            const double elapsed = glfwGetTime() - encode_t0_;
            std::string busy = "编码中";
            if (etotal > 0) {
                busy += " " + std::to_string(edone) + "/" +
                        std::to_string(etotal) + " 帧";
                param_panel->set_progress(etotal + 9 * edone, 10 * etotal);
                if (edone >= etotal) {
                    busy += "，组装文件…";
                } else if (edone > 0 && elapsed > 1e-3) {
                    const double remain =
                        elapsed / static_cast<double>(edone) *
                        static_cast<double>(etotal - edone);
                    busy += "，剩余约 " + RemainingText(remain);
                }
            } else {
                busy += "… " + std::to_string(static_cast<int>(elapsed)) + "s";
            }
            param_panel->set_status(busy);
            set_status(busy);
            return;
        }
        encode_thread_.join();
        std::string error;
        {
            std::lock_guard<std::mutex> lock(encode_mutex_);
            error = encode_error_;
        }
        if (error.empty()) {
            char info[512];
            std::snprintf(
                info, sizeof(info),
                "导出完成：%d×%d，文件 %d 帧 / %.2f 秒；渲染 %d 帧，每帧 %d ms，周期 %.2f 秒",
                encode_result_.width,
                encode_result_.height, encode_result_.file_frames,
                encode_result_.duration_ms / 1000.0, encode_result_.frames,
                encode_result_.frame_ms,
                encode_result_.period_ms / 1000.0);
            std::string full = info;
            if (encode_result_.frame_capped) {
                full += "（帧数已按内存上限下调，循环仍是一整个周期）";
            }
            if (encode_result_.frames <= 1) {
                std::snprintf(info, sizeof(info), "导出完成：%d×%d，静态图（没有 BBOX）",
                              encode_result_.width, encode_result_.height);
                full = info;
            }
            set_status(full);
            param_panel->set_status(encode_result_.path);
        } else {
            set_status("导出失败：" + error);
            param_panel->set_status("导出失败：" + error);
        }
        exporter_.reset();
        exporting_ = false;
        param_panel->set_exporting(false);
        return;
    }

    bool more = false;
    try {
        more = exporter_->step();
    } catch (const std::exception& error) {
        exporter_.reset();
        exporting_ = false;
        param_panel->set_exporting(false);
        param_panel->set_status("导出失败");
        set_status(std::string("导出失败：") + error.what());
        return;
    }
    const int done = exporter_->done_frames();
    const int total = exporter_->total_frames();
    // 进度条分两段：渲染占前 90%，编码占最后 10%（编码本身也有真进度了）
    param_panel->set_progress(done, total * 10);
    std::string progress =
        "导出中 " + std::to_string(done) + "/" + std::to_string(total) + " 帧";
    if (done > 0 && total > done) {
        // 渲染每帧都在测时：剩余 = 已渲平均每帧 × 还没渲的帧数（首帧含着色器
        // 编译与 FBO 建立，所以前几帧会偏大，几帧之后就稳住）
        const double elapsed = glfwGetTime() - enc_t0;
        const double remain = elapsed / static_cast<double>(done) *
                              static_cast<double>(total - done);
        progress += "，剩余约 " + RemainingText(remain);
    }
    param_panel->set_status(progress);
    set_status(progress);

    if (more) return;

    Exporter* exporter = exporter_.get();
    encode_t0_ = glfwGetTime();
    encode_thread_ = std::thread([this, exporter]() {
        std::string error;
        ExportResult result;
        try {
            result = exporter->finish();
        } catch (const std::exception& failure) {
            error = failure.what();
        } catch (...) {
            error = "编码线程未知异常";
        }
        {
            std::lock_guard<std::mutex> lock(encode_mutex_);
            encode_result_ = result;
            encode_error_ = error;
        }
        encode_finished_ = true;
    });
}

void Scene::estimate_and_align() {
    if (state.doc == nullptr) return;
    // 无缝对齐只算参与呼吸的 BBOX（眼睛关掉的区域不产生周期，不该进速度表）
    const auto regions = state.doc->visible_regions();
    std::vector<double> speeds;
    for (const Region* region : regions) speeds.push_back(region->speed);
    const LoopPlan plan = plan_loop(state.frames, state.ms_per_frame, speeds);
    if (!plan.ok) {
        set_status("无法对齐：" + plan.hint);
        return;
    }
    push_undo();
    state.frames = plan.n;
    param_panel->refresh_fps_info();
    set_status("已对齐 " + std::to_string(plan.n) + " 帧（"
               + format_duration(plan.duration) + "）");
}

// ---- 面板重建 ----
void Scene::rebuild_panels() {
    param_panel->set_has_layers(has_layers());
    param_panel->sync_export();
    param_panel->refresh_fps_info();
    param_panel->show_region(selection.sole_region());
    layer_panel->rebuild();
}

// ---- 帧循环 ----
void Scene::update(double dt) {

    now = glfwGetTime();
    state.time += dt;
    state.update(state.time);

    layer_views.sync(!x_hidden);

    fps_timer += dt;
    ++fps_frames;
    if (fps_timer >= cfg::FPS_REFRESH_INTERVAL) {
        param_panel->set_render_fps(static_cast<double>(fps_frames) / fps_timer);
        fps_timer = 0.0;
        fps_frames = 0;
    }

    // 导出逐帧推进：一帧渲染 → 进度回调 → 渲完转后台编码
    pump_export();
}

void Scene::draw_canvas() {
    glClearColor(cfg::CANVAS_DARK.r / 255.0f, cfg::CANVAS_DARK.g / 255.0f,
                 cfg::CANVAS_DARK.b / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    Renderer& renderer = canvas_renderer();
    renderer.begin(width_, height_);
    glEnable(GL_BLEND);
    gl::BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                          GL_ONE_MINUS_SRC_ALPHA);

    // 画布底：整屏暗色 + 只在本机画布矩形内铺棋盘格；界外不盖内容，
    // 越界像素照样可见（对齐 ImageEditor CanvasNode::paint 的顺序：
    // 先填底、再画图层，没有二次遮挡）。
    const Rect canvas = canvas_rect();
    const auto p0 = world_to_window(canvas.x0, canvas.y0);
    const auto p1 = world_to_window(canvas.x1, canvas.y1);
    Backdrop::draw(renderer, width_, height_, std::min(p0.first, p1.first),
                   std::min(p0.second, p1.second),
                   std::max(p0.first, p1.first),
                   std::max(p0.second, p1.second));

    if (!layer_views.empty()) {
        Program& program = BreathStyle::program();
        for (const auto& view : layer_views.views()) {
            if (!view->visible || view->asset == nullptr
                || !view->asset->texture) {
                continue;
            }
            double corners[8];
            view->world_corners(corners);
            double quad[8];
            for (int i = 0; i < 4; ++i) {
                const auto s = world_to_window(corners[i * 2], corners[i * 2 + 1]);
                quad[i * 2] = s.first;
                quad[i * 2 + 1] = s.second;
            }
            view->style.bind(program, inside_all_mask());
            renderer.draw_quad(program, *view->asset->texture, kLayerUnit, quad);
        }
    }

    // 边界不是 UI 层另画的一条线，而是画布底自己的 1px 边：同一对坐标、
    // 同一个着色器，画在内容之上（对齐 ImageEditor 把底色/棋盘/边界都归
    // CanvasNode::paint 与 recordCanvasBorder 一处负责的职责划分）。
    Backdrop::draw(renderer, width_, height_, std::min(p0.first, p1.first),
                   std::min(p0.second, p1.second),
                   std::max(p0.first, p1.first),
                   std::max(p0.second, p1.second), true);
}

void Scene::draw_overlay(Painter& painter) { DrawOverlay(*this, painter); }

void Scene::draw_ui(Painter& painter) {

    draw_overlay(painter);

    for (const std::string& which : float_order) {
        if (which == "param") {
            if (param_float.floating) continue;
            param_panel->draw(painter);
        } else {
            if (layer_float.floating) continue;
            layer_panel->draw(painter);
        }
    }
    for (const std::string& which : float_order) {
        if (which == "param") {
            if (!param_float.floating) continue;
            PanelFloat& panel = param_float;
            painter.push_clip(panel.left, panel.bottom, panel.w, panel.h);
            param_panel->draw(painter);
            painter.pop_clip();
        } else {
            if (!layer_float.floating) continue;
            PanelFloat& panel = layer_float;
            painter.push_clip(panel.left, panel.bottom, panel.w, panel.h);
            layer_panel->draw(painter);
            painter.pop_clip();
        }
    }

    status->draw(painter);

    // 面板拖动浮层：最后画、不裁剪，指针带出面板/带出窗口也要看得见
    layer_panel->draw_ghost(painter);
    // 参数面板的帧率弹层同口径：最后画、不裁剪，才能盖过画布与图层面板
    param_panel->draw_popup(painter);
    // 工具小板（橡皮）压在最上面：它是画布的操作条，不是面板的一部分
    if (tool_board != nullptr) tool_board->draw(painter);
    // 作者按钮 + 作者卡：压在所有面板之上，位置固定，不受面板拖动影响
    draw_author(painter);
}

// ---- 拖拽 ----
void Scene::begin_drag(const std::string& kind, Layer* layer,
                       const std::string& handle, double wx, double wy) {
    cancel_bbox_drag();
    drag_kind = kind;
    drag_layer = layer;
    drag_handle = handle;
    drag_last = {wx, wy};
    drag_offset = {0.0, 0.0};
    drag_region = nullptr;
    if (layer != nullptr && kind == "bbox") {
        drag_region = selection.sole_region();
    }
    if (layer != nullptr && kind == "handle") {
        drag_region = selection.sole_region();
        if (drag_region != nullptr) {
            handle_start_rot = drag_region->rot;
            handle_start_rx = drag_region->rx;
            handle_start_ry = drag_region->ry;
            const auto local = layer->from_world(wx, wy);
            handle_start_angle =
                std::atan2(local.second - drag_region->cy,
                           local.first - drag_region->cx)
                * 180.0 / 3.14159265358979323846;
        }
    }
}

// ---- 事件 ----
bool Scene::resize_hit(float x, float y, std::string* which,
                       std::string* edge) const {
    for (const std::string& key : {"param", "layer"}) {
        PanelFloat& panel =
            key == "param" ? const_cast<PanelFloat&>(param_float)
                           : const_cast<PanelFloat&>(layer_float);
        // 停靠态也要能命中：内侧收起条（拖动改宽 / 原地单击收起）
        const bool collapsed = !panel.floating && is_collapsed(panel.side);
        const std::string found =
            panel.resize_hit(x, y, static_cast<float>(width_),
                             static_cast<float>(height_), collapsed);
        if (!found.empty()) {
            *which = key;
            *edge = found;
            return true;
        }
    }
    return false;
}

void Scene::resize_drag(double x, double y) {
    if (!resize_active) return;
    PanelFloat& panel = panel_float(resize_which);
    const float max_w = static_cast<float>(width_)
                        - 2.0f * cfg::PANEL_FLOAT_MARGIN;
    panel.resize_to(x, y, static_cast<float>(width_),
                    static_cast<float>(height_), max_w);
    relayout(height_);
}

void Scene::resize_release() {
    if (!resize_active) return;
    PanelFloat& panel = panel_float(resize_which);
    const bool resized = panel.end_resize();
    resize_active = false;
    resize_which.clear();
    const std::string edge = resize_edge;
    resize_edge.clear();
    // 拖动收起条 = 改宽度；原地单击 = 收起/展开（与 Python 同一条口径）
    if (!resized && edge == "dock") {
        set_collapsed(panel.side, !is_collapsed(panel.side));
    }
    relayout(height_);
}

bool Scene::panel_header_press(const std::string& which, double x, double y,
                               int button) {
    (void)x;
    (void)y;
    if (button != GLFW_MOUSE_BUTTON_LEFT) return false;
    PanelFloat& panel = panel_float(which);
    if (!panel.contains_header(static_cast<float>(mouse_x),
                               static_cast<float>(mouse_y),
                               static_cast<float>(width_),
                               static_cast<float>(height_),
                               !panel.floating && is_collapsed(panel.side))) {
        return false;
    }
    if (panel.take_double_click(now, cfg::DOUBLE_CLICK_SECONDS)) {
        set_panel_floating(which, !panel.floating);
        return true;
    }
    raise_float(which);
    header_drag_active = true;
    header_drag_which = which;
    panel.begin_drag(static_cast<float>(x), static_cast<float>(y));
    return true;
}

void Scene::panel_header_drag(double x, double y) {
    if (!header_drag_active) return;
    PanelFloat& panel = panel_float(header_drag_which);
    if (!panel.floating) {
        if (std::fabs(x - panel.press_x) < cfg::PANEL_DRAG_MIN
            && std::fabs(y - panel.press_y) < cfg::PANEL_DRAG_MIN) {
            return;
        }
        set_panel_floating(header_drag_which, true);
        panel.begin_drag(static_cast<float>(x), static_cast<float>(y));
    }
    panel.drag_to(x, y);
    relayout(height_);
}

void Scene::panel_header_release() {
    if (!header_drag_active) return;
    PanelFloat& panel = panel_float(header_drag_which);
    panel.end_drag();
    header_drag_active = false;
    header_drag_which.clear();
    relayout(height_);
}

void Scene::mouse_press(double x, double y, int button, int mods) {
    // 作者按钮与作者卡压在最上层，先问它们
    if (button == GLFW_MOUSE_BUTTON_LEFT && author_press(x, y, button)) return;
    mouse_x = x;
    mouse_y = y;
    now = glfwGetTime();

    std::string which;
    std::string edge;
    layer_panel->commit_rename_outside(static_cast<float>(x),
                                       static_cast<float>(y));
    // 参数面板的弹层是模态：命中选择、以及"点在弹层外就关掉并吞掉这次按下"
    // 都要在下面所有分流（标题/浮动/控件/画布）之前决定。
    if (param_panel->popup_press(static_cast<float>(x), static_cast<float>(y),
                                 button)) {
        return;
    }
    if (button == GLFW_MOUSE_BUTTON_LEFT && resize_hit(static_cast<float>(x),
                                                       static_cast<float>(y),
                                                       &which, &edge)) {
        if (edge == "bar") {   // 收起条上单击 = 展开
            set_collapsed(panel_float(which).side, false);
            return;
        }
        resize_active = true;
        resize_which = which;
        resize_edge = edge;
        panel_float(which).begin_resize(edge, x, y);
        return;
    }

    if (param_panel->header_hit(static_cast<float>(x), static_cast<float>(y))) {
        panel_header_press("param", x, y, button);
        return;
    }
    if (layer_panel->header_hit(static_cast<float>(x), static_cast<float>(y))) {
        panel_header_press("layer", x, y, button);
        return;
    }

    const std::string floating = float_at(static_cast<float>(x),
                                          static_cast<float>(y));
    if (!floating.empty()) raise_float(floating);

    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        if (param_panel->entry_press(static_cast<float>(x),
                                     static_cast<float>(y), button)) {
            return;
        }
    }
    if (param_panel->press(static_cast<float>(x), static_cast<float>(y),
                           button, mods)) {
        return;
    }
    if (param_float.contains(static_cast<float>(x), static_cast<float>(y),
                             static_cast<float>(width_),
                             static_cast<float>(height_),
                             !param_float.floating
                                 && is_collapsed(param_float.side))) {
        return;
    }
    if (layer_panel->press(static_cast<float>(x), static_cast<float>(y),
                           button, mods)) {
        return;
    }
    if (layer_float.contains(static_cast<float>(x), static_cast<float>(y),
                             static_cast<float>(width_),
                             static_cast<float>(height_),
                             !layer_float.floating
                                 && is_collapsed(layer_float.side))) {
        return;
    }
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        // 工具小板压在画布上面：按下先问它，它说"是我"就由它接管整次拖动
        if (tool_board != nullptr
            && tool_board->press(static_cast<float>(x),
                                 static_cast<float>(y))) {
            board_capture_ = true;
            return;
        }
    }
    if (!in_canvas(x)) return;
    // 橡皮开着时左键归它：擦除优先于点选/平移（中键平移与空格仍然可用）
    if (button == GLFW_MOUSE_BUTTON_LEFT && eraser_on()) {
        eraser_press(x, y);
        return;
    }
    // 中键按住拖 = 整个视图平移（不区分点到了图像还是空白）
    if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
        const auto world = window_to_world(x, y);
        begin_drag("pan", nullptr, "", world.first, world.second);
        return;
    }
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        input::HandlePress(*this, x, y, button, mods);
    }
}

void Scene::mouse_motion(double x, double y, double dx, double dy) {
    mouse_x = x;
    mouse_y = y;
    param_panel->motion(static_cast<float>(x), static_cast<float>(y));
    layer_panel->motion(static_cast<float>(x), static_cast<float>(y));
    update_cursor();
    (void)dx;
    (void)dy;
}

void Scene::mouse_drag(double x, double y, double dx, double dy, int mods) {
    mouse_x = x;
    mouse_y = y;
    if (resize_active) {
        resize_drag(x, y);
        return;
    }
    if (header_drag_active) {
        panel_header_drag(x, y);
        return;
    }
    if (board_capture_) {
        if (tool_board != nullptr)
            tool_board->drag(static_cast<float>(x), static_cast<float>(y));
        return;
    }
    if (eraser_active_) {
        eraser_drag(static_cast<double>(x), static_cast<double>(y));
        return;
    }
    if (layer_panel->press_active()) {
        layer_panel->drag(static_cast<float>(x), static_cast<float>(y), dx, dy,
                          mods);
        return;
    }
    if (param_panel->slider_hit(static_cast<float>(x), static_cast<float>(y))
        || param_drag_active_) {
        param_drag_active_ = true;
        param_panel->drag(static_cast<float>(x), static_cast<float>(y), dx, dy,
                          mods);
        return;
    }
    if (param_panel->entry_drag(static_cast<float>(x), static_cast<float>(y),
                                dx, dy)) {
        return;
    }
    input::HandleDrag(*this, x, y, dx, dy, mods);
}

void Scene::mouse_release(double x, double y, int button, int mods) {
    mouse_x = x;
    mouse_y = y;
    if (resize_active) {
        resize_release();
        return;
    }
    if (header_drag_active) {
        panel_header_release();
        return;
    }
    if (board_capture_) {
        if (tool_board != nullptr)
            tool_board->release(static_cast<float>(x), static_cast<float>(y));
        board_capture_ = false;
        return;
    }
    if (eraser_active_) {
        eraser_release();
        active_which.clear();
        return;
    }
    layer_panel->release(static_cast<float>(x), static_cast<float>(y), button,
                         mods);
    param_panel->release(static_cast<float>(x), static_cast<float>(y), button);
    param_panel->entry_release(static_cast<float>(x), static_cast<float>(y),
                               button);
    param_drag_active_ = false;
    param_panel->set_has_layers(has_layers());
    input::HandleRelease(*this, x, y, button, mods);
    active_which.clear();
}

void Scene::mouse_scroll(double x, double y, double dy) {
    mouse_x = x;
    mouse_y = y;
    // 弹层开着时滚轮归它：先收起，不缩放/不平移画布
    if (param_panel->popup_open()) {
        param_panel->close_popup();
        return;
    }
    // GLFW 的 yoffset 向上为正；面板里 scroll_ 是"内容上移量"，
    // 向上滚要看到更前面的条目，所以取负（ImageEditor 的列表滚动同此方向）。
    if (param_float.contains(static_cast<float>(x), static_cast<float>(y),
                             static_cast<float>(width_),
                             static_cast<float>(height_),
                             !param_float.floating
                                 && is_collapsed(param_float.side))) {
        param_panel->scroll_by(-dy * cfg::SCROLL_STEP);
        return;
    }
    if (layer_float.contains(static_cast<float>(x), static_cast<float>(y),
                             static_cast<float>(width_),
                             static_cast<float>(height_),
                             !layer_float.floating
                                 && is_collapsed(layer_float.side))) {
        layer_panel->scroll_by(-dy * cfg::SCROLL_STEP);
        return;
    }
    input::HandleScroll(*this, x, y, dy);
}

namespace {

// 键盘事件到输入框动作的唯一映射；参数面板与图层改名共用。
bool MapEditKey(int key, int mods, EditKey* action) {
    const bool ctrl = (mods & GLFW_MOD_CONTROL) != 0;
    switch (key) {
        case GLFW_KEY_ENTER:
        case GLFW_KEY_KP_ENTER:
            *action = EditKey::kEnter;
            return true;
        case GLFW_KEY_ESCAPE:
            *action = EditKey::kEscape;
            return true;
        case GLFW_KEY_BACKSPACE:
            *action = EditKey::kBackspace;
            return true;
        case GLFW_KEY_DELETE:
            *action = EditKey::kDelete;
            return true;
        case GLFW_KEY_LEFT:
            *action = EditKey::kLeft;
            return true;
        case GLFW_KEY_RIGHT:
            *action = EditKey::kRight;
            return true;
        case GLFW_KEY_HOME:
            *action = EditKey::kHome;
            return true;
        case GLFW_KEY_END:
            *action = EditKey::kEnd;
            return true;
        case GLFW_KEY_A:
            if (!ctrl) return false;
            *action = EditKey::kSelectAll;
            return true;
        default:
            return false;
    }
}

}  // namespace

void Scene::key_press(int key, int mods) {
    // 导出期间 Esc = 取消（Python 版没有中断点，这条是本项目可选增强）
    if (exporting_ && key == GLFW_KEY_ESCAPE) {
        cancel_export();
        return;
    }
    // 下拉弹层开着时 Esc 先收起它，不吃别的键
    if (param_panel->popup_key(key, mods)) return;
    // 作者卡开着时 Esc 先关它
    if (author_open_ && key == GLFW_KEY_ESCAPE) {
        author_open_ = false;
        return;
    }
    EditKey action = EditKey::kEnter;
    const bool mapped = MapEditKey(key, mods, &action);
    if (layer_panel->rename_focused()) {
        if (mapped) layer_panel->rename_key(action);
        return;
    }
    IntEntry* entry = param_panel->focused_entry();
    if (entry != nullptr) {
        if (mapped) entry->key(action);
        return;
    }
    input::HandleKey(*this, key, mods);
    update_cursor();
}

void Scene::key_release(int key, int mods) {
    input::HandleKeyRelease(*this, key, mods);
    update_cursor();
}

void Scene::text_input(const std::string& text) {
    if (layer_panel->rename_text(text)) return;
    IntEntry* entry = param_panel->focused_entry();
    if (entry != nullptr) entry->insert_text(text);
}

void Scene::file_drop(const std::vector<std::string>& paths) {
    for (const std::string& path : paths) {
        add_layer(path);
        param_panel->refresh_export_size();
    }
}

void Scene::deactivate() {
    IntEntry* entry = param_panel->focused_entry();
    if (entry != nullptr) entry->unfocus();
    // 丢焦点时把板子的拖动收掉，否则再回到窗口会接着拖滑块
    if (board_capture_) {
        if (tool_board != nullptr) tool_board->release(0.0f, 0.0f);
        board_capture_ = false;
    }
    alt_held = false;
    space_held = false;
    space_used = false;
}

// ---- 光标 ----
void Scene::update_cursor() {
    if (cursors == nullptr) return;
    std::string name;
    if (space_held) {
        name = drag_kind == "content" || drag_kind == "pan" ? "grabbing" : "grab";
    } else {
        std::string which;
        std::string edge;
        if (resize_hit(static_cast<float>(mouse_x),
                       static_cast<float>(mouse_y), &which, &edge)) {
            const char* cursor = EdgeCursor(edge);   // bar/dock 之外可能是空
            if (cursor != nullptr) name = cursor;
        } else if (in_canvas(mouse_x)) {
            const auto world = window_to_world(mouse_x, mouse_y);
            if (transform_mode && transform_layer != nullptr) {
                const std::string th =
                    input::TransformHandleAt(*this, world.first, world.second);
                if (th == "rot") {
                    name = "rotate";
                } else if (th == "move") {
                    name = "grab";
                } else if (!th.empty()) {
                    // 角光标跟旋转后的真实对角方向走：从图层中心量角度，
                    // 每 45° 一档取双箭头（横/斜/竖/斜）。
                    const auto c = world_to_window(transform_layer->x,
                                                   transform_layer->y);
                    const double ang =
                        std::atan2(mouse_y - c.second, mouse_x - c.first)
                        * 180.0 / 3.14159265358979323846;
                    int k = static_cast<int>(std::lround(ang / 45.0));
                    k = ((k % 4) + 4) % 4;
                    const char* arrows[4] = {"size_h", "size_nesw", "size_v",
                                             "size_nwse"};
                    name = arrows[k];
                }
            } else if (!bbox_overlay_hidden()) {
                const std::string handle =
                    input::HandleAt(*this, world.first, world.second);
                if (!handle.empty()) {
                    if (handle == "left" || handle == "right") name = "size_h";
                    else if (handle == "top" || handle == "bottom")
                        name = "size_v";
                    else if (handle == "rotate") name = "rotate";
                }
            }
        }
    }
    cursors->apply(name);
}

void Scene::save_settings() {
    // 真正的落盘在 io/settings::Save；这里只暴露状态。
}

// ---- 面板输入转发 ----
void Scene::panel_drag(const std::string& which, double x, double y, double dx,
                       double dy, int mods) {
    (void)which;
    (void)x;
    (void)y;
    (void)dx;
    (void)dy;
    (void)mods;
}

void Scene::panel_release(const std::string& which, double x, double y,
                          int button, int mods) {
    (void)which;
    (void)x;
    (void)y;
    (void)button;
    (void)mods;
}

bool Scene::panel_press(const std::string& which, double x, double y, int button,
                        int mods) {
    (void)which;
    (void)x;
    (void)y;
    (void)button;
    (void)mods;
    return false;
}

// ---- 作者卡 -------------------------------------------------------------
// 版面固定：按钮贴窗口左下角；卡宽 200（文字宽 200），图 200×200 顶边紧贴文字下方。
namespace {
constexpr float kAuthorBtnW = 64.0f;
constexpr float kAuthorBtnH = 22.0f;
constexpr float kAuthorMargin = 8.0f;
constexpr float kAuthorCardW = 200.0f;
constexpr float kAuthorPad = 10.0f;   // 卡左右内边距：200 是文字/图片的净宽
constexpr float kAuthorRow = 4.0f;    // 行与行之间的空隙
}  // namespace

void Scene::author_button_rect(float* x, float* y, float* w, float* h) const {
    // 贴窗口右下角
    if (x != nullptr) {
        *x = static_cast<float>(width_) - kAuthorMargin - kAuthorBtnW;
    }
    if (y != nullptr) *y = kAuthorMargin;
    if (w != nullptr) *w = kAuthorBtnW;
    if (h != nullptr) *h = kAuthorBtnH;
}

void Scene::author_card_rect(float* x, float* y, float* w, float* h) const {
    // 卡在按钮正上方，底边留 6px；高度按内容算：标题 + 副标题 + 图 200
    const float title_h = 20.0f;
    const float sub_h = 18.0f;
    const float img = static_cast<float>(std::max(1, author_sh_ > 0 ? author_sh_
                                                                   : 200));
    const float card_w = kAuthorCardW + kAuthorPad * 2.0f;
    const float card_h = kAuthorPad * 2.0f + title_h + kAuthorRow + sub_h
                         + kAuthorRow + img;
    float bx = 0.0f, by = 0.0f, bw = 0.0f, bh = 0.0f;
    author_button_rect(&bx, &by, &bw, &bh);
    // 卡右边缘与按钮右边缘对齐，向上弹
    if (x != nullptr) *x = bx + bw - card_w;
    if (y != nullptr) *y = by + bh + 6.0f;
    if (w != nullptr) *w = card_w;
    if (h != nullptr) *h = card_h;
}

void Scene::ensure_author_texture() {
    if (author_tex_ != 0) return;
    std::vector<unsigned char> bytes;
    if (!io::ReadResourceBytes(L"AUTHOR_PNG", &bytes)) return;
    try {
        const RgbaImage image =
            load_image_rgba_memory(bytes.data(), bytes.size());
        Texture* texture = new Texture();
        texture->create(image.width, image.height, 4, image.pixels.data());
        author_tex_ = texture->id();
        author_sw_ = image.width;
        author_sh_ = image.height;
        author_holder_.reset(texture);   // 跟着 Scene 生命周期释放
    } catch (const std::exception&) {
        author_tex_ = 0;
    }
}

void Scene::draw_author(Painter& painter) {
    ensure_author_texture();
    float bx = 0.0f, by = 0.0f, bw = 0.0f, bh = 0.0f;
    author_button_rect(&bx, &by, &bw, &bh);
    const bool hover = mouse_x >= bx && mouse_x <= bx + bw && mouse_y >= by
                       && mouse_y <= by + bh;
    // 按钮无框无底，只给文字做 1px 黑色外描边（同 CenterStatus 的画法）
    const float tx = bx + bw * 0.5f;
    const float ty = by + bh * 0.5f;
    const float offsets[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1},
                                 {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    for (const auto& offset : offsets) {
        painter.text("作者", tx + offset[0], ty + offset[1], cfg::FontRole::Body,
                     false, cfg::FromRgb(0, 0, 0), Align::kCenter);
    }
    painter.text("作者", tx, ty, cfg::FontRole::Body, false,
                 hover ? cfg::TEXT : cfg::TEXT_DIM, Align::kCenter);
    if (!author_open_) return;

    float cx = 0.0f, cy = 0.0f, cw = 0.0f, ch = 0.0f;
    author_card_rect(&cx, &cy, &cw, &ch);
    painter.rect(cx, cy, cw, ch, cfg::CARD_BG);
    painter.rect(cx, cy, cw, 1.0f, cfg::PANEL_BORDER);
    painter.rect(cx, cy + ch - 1.0f, cw, 1.0f, cfg::PANEL_BORDER);
    painter.rect(cx, cy, 1.0f, ch, cfg::PANEL_BORDER);
    painter.rect(cx + cw - 1.0f, cy, 1.0f, ch, cfg::PANEL_BORDER);

    const float inner_x = cx + kAuthorPad;
    const float title_h = 20.0f;
    const float sub_h = 18.0f;
    // 自上而下排：卡顶 → 标题 → 副标题 → 图
    const float title_y = cy + ch - kAuthorPad - title_h * 0.5f;
    painter.text("ClassRonin", cx + cw * 0.5f, title_y, cfg::FontRole::Title,
                 false, cfg::TEXT, Align::kCenter);
    const float sub_y = title_y - title_h * 0.5f - kAuthorRow - sub_h * 0.5f;
    painter.text("请我喝杯咖啡", cx + cw * 0.5f, sub_y, cfg::FontRole::Body,
                 false, cfg::TEXT, Align::kCenter);
    const float img = static_cast<float>(std::max(1, author_sh_ > 0 ? author_sh_
                                                                   : 200));
    const float img_x = inner_x;
    const float img_y = cy + kAuthorPad;
    if (author_tex_ != 0) {
        painter.sprite(author_tex_, img_x, img_y,
                       static_cast<float>(author_sw_), img);
    }
}

bool Scene::author_press(double x, double y, int button) {
    (void)button;
    ensure_author_texture();
    if (author_open_) {
        float cx = 0.0f, cy = 0.0f, cw = 0.0f, ch = 0.0f;
        author_card_rect(&cx, &cy, &cw, &ch);
        if (x >= cx && x <= cx + cw && y >= cy && y <= cy + ch) return true;
    }
    float bx = 0.0f, by = 0.0f, bw = 0.0f, bh = 0.0f;
    author_button_rect(&bx, &by, &bw, &bh);
    if (x >= bx && x <= bx + bw && y >= by && y <= by + bh) {
        author_open_ = !author_open_;
        return true;
    }
    if (author_open_) author_open_ = false;   // 点别处就收起来
    return false;
}

}  // namespace bl::ui
