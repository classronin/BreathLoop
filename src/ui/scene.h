// 场景：文档、视图、面板与导出编排的汇合点。
//
// 数据流单向：输入改 state/doc → 场景重建视图与面板 → 面板只读 state。
// 坐标换算集中在这里（窗口 ↔ 世界），其他模块不得重推。
#pragma once

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <GLFW/glfw3.h>

#include "config.h"
#include "domain/lasso.h"
#include "domain/layers.h"
#include "domain/region.h"
#include "domain/selection.h"
#include "domain/state.h"
#include "render/assets.h"
#include "render/exporter.h"
#include "render/layer_view.h"
#include "ui/floating.h"
#include "ui/painter.h"

namespace bl {
class Texture;
}

namespace bl::ui {

class LayerPanel;
class ParamPanel;
class ToolBoard;
class CenterStatus;
class SceneCursors;

class Scene {
public:
    Scene();
    ~Scene();

    bool init(GLFWwindow* window_handle, std::string* error);

    // ---- 基本属性与坐标 ----
    int width() const { return width_; }
    int height() const { return height_; }
    void set_size(int width, int height);

    std::pair<double, double> window_to_stage(double x, double y) const;
    std::pair<double, double> window_to_world(double x, double y) const;
    std::pair<double, double> stage_to_world(double x, double y) const;
    std::pair<double, double> world_to_stage(double x, double y) const;
    std::pair<double, double> world_to_window(double x, double y) const;
    std::pair<double, double> canvas_edges() const;
    bool in_canvas(double x) const;
    // 光标是否压在任一面板（含折叠/浮动状态），粘贴落点据此决定跟光标还是画布中心。
    bool over_panel(float x, float y) const;
    // 画布边界（世界坐标）与尺寸；两者唯一来源都是 Document。
    Rect canvas_rect() const;
    std::pair<int, int> canvas_size() const;
    void set_canvas_size(int width, int height);
    // 把画布改成某图层位图的原生尺寸（导出面板 ↪ 的动作）。可撤销，附带状态反馈。
    bool adopt_canvas_from_layer(Layer* layer);

    void relayout(int height);
    void update_cursor();
    void set_collapsed(const std::string& side, bool value);
    bool is_collapsed(const std::string& side) const;

    // ---- 浮动面板 ----
    PanelFloat& panel_float(const std::string& which);
    const PanelFloat& panel_float(const std::string& which) const;
    std::vector<PanelFloat*> float_states();
    void set_panel_floating(const std::string& which, bool floating);
    std::string float_at(float x, float y) const;   // "" = 没有
    void raise_float(const std::string& which);

    // ---- 选择 ----
    Layer* selected_layer() const { return selection.layer(); }
    Region* selected_region() const { return selection.region(); }
    void select_layer(Layer* layer);
    void select_region(Layer* layer, Region* region);
    void clear_selection();
    void toggle_pick(Layer* layer, Region* region);
    void select_all();

    // ---- 整组变换 ----
    void move_group(double dx, double dy);
    void scale_group(double factor);
    void delete_selection();
    int region_index(const Region* region) const;
    cfg::Rgba region_color(const Region* region) const;

    // ---- 撤销 ----
    void push_undo();
    void undo();
    void redo();

    // ---- 视图与渲染 ----
    void rebuild_views();
    void sync_views();
    void fit_view();
    void toggle_x();
    bool bbox_overlay_hidden() const { return x_hidden || bbox_hidden; }
    void toggle_bbox_overlay();
    void cancel_bbox_drag();

    // ---- 工具（套索 / 橡皮：同一时刻只有一个）----
    // 工具是唯一状态，套索和橡皮不各存一个布尔，所以"两边同时开着"不存在。
    enum class Tool { kNone, kEraser, kLasso };
    Tool tool() const { return tool_; }
    bool eraser_on() const { return tool_ == Tool::kEraser; }
    bool is_lasso_mode() const { return tool_ == Tool::kLasso; }
    // 切换工具：先收笔（进行中的橡皮笔画 / 未提交的套索路径），再换状态。
    // 进橡皮时套索一律清掉（橡皮模式下不存在套索）。
    void set_tool(Tool tool);

    // ---- 套索 ----
    // 套索不挂在任何图层上：路径存的是画布世界坐标，目标图层每次现推。
    enum class LassoPhase { kNone, kDrawing, kClosed };
    LassoPhase lasso_phase() const;
    // 套索的作用图层：选中且可见就用它，否则用最上面的可见图层；都没有 nullptr
    Layer* lasso_target() const;
    bool lasso_closed() const { return lasso_closed_; }
    void toggle_lasso_mode();
    void add_lasso_point(double wx, double wy);
    void remove_lasso_point();
    void commit_lasso();
    void cancel_lasso();
    bool erase_lasso_area();
    void clear_lasso();   // 清掉已经生效的套索（A 第二次按下 / 双击）
    // 粘贴之后收尾：套索用完即清，工具退回空态（不再挂着选区、A 从头开始）
    void finish_lasso();
    bool cut_out_to_clipboard();
    // 把世界坐标路径投到某个图层自己的图像坐标，喂给既有 mask/cutout 算法
    bool local_lasso(const Layer& layer, Lasso* out) const;

    // ---- 橡皮（一笔一次撤销）----
    void set_eraser(bool on);
    bool eraser_press(double x, double y);
    void eraser_drag(double x, double y);
    void eraser_release();
    // 丢掉正在进行的这一笔（还原被实时预览改过的纹理），不落资产、不进撤销
    void eraser_abort();
    bool eraser_active() const { return eraser_active_; }

    // ---- 图层与 BBOX ----
    bool has_layers() const;
    void add_layer(const std::string& path);
    void delete_layer(Layer* layer);
    void paste_layer(const LayerClip& clip, double wx, double wy);
    void move_bbox(Layer* owner, Region* region, Layer* target, int index);
    void reorder_layer(Layer* layer, Layer* parent, int index);
    void toggle_layer_visible(Layer* layer);
    // BBOX 自己的眼睛：关掉 = 这块区域不显示、不呼吸（图层眼睛仍然单向压它）
    void toggle_region_visible(Region* region);
    // 图层"图像"尺寸：框里读的是画布世界像素（含变形），写回横纵倍率
    std::pair<double, double> layer_size(const Layer* layer) const;
    void set_layer_size(Layer* layer, double w, double h);
    bool layer_ratio_locked(const Layer* layer) const;   // 横纵倍率一致
    bool layer_keep_ratio() const { return layer_lock_; }
    void set_layer_keep(bool on) { layer_lock_ = on; }
    void center_layer(Layer* layer);
    void add_bbox(Layer* layer);
    void remove_bbox(Layer* layer, Region* region);

    // ---- 参数 ----
    void on_param(const std::string& key, double value);
    void on_sync_toggle(bool enabled);
    void on_preset(const std::string& name);
    void set_status(const std::string& text);
    const std::string& status_text() const { return status_text_; }

    // ---- 导出 ----
    std::pair<int, int> default_export_size();
    bool ensure_export_size();
    void start_export();
    void cancel_export();
    void pump_export();     // 每帧推一帧渲染，渲完转后台编码
    bool exporting() const { return exporting_; }
    void estimate_and_align();

    // ---- 面板重建 ----
    void rebuild_panels();

    // ---- 帧循环 ----
    void update(double dt);
    void draw_canvas();
    void draw_ui(Painter& painter);

    // ---- 拖拽起始（供 input 调用） ----
    void begin_drag(const std::string& kind, Layer* layer,
                    const std::string& handle, double wx, double wy);

    // ---- 事件 ----
    void key_press(int key, int mods);
    void key_release(int key, int mods);
    void text_input(const std::string& text);
    void mouse_press(double x, double y, int button, int mods);
    void mouse_motion(double x, double y, double dx, double dy);
    void mouse_drag(double x, double y, double dx, double dy, int mods);
    void mouse_release(double x, double y, int button, int mods);
    void mouse_scroll(double x, double y, double dy);
    void file_drop(const std::vector<std::string>& paths);
    void deactivate();

    // ---- 设置（io/settings 调用） ----
    void save_settings();
    std::vector<std::string> float_order_state() const { return float_order; }
    void set_float_order(const std::vector<std::string>& order) {
        float_order = order;
    }
    std::map<std::string, bool> collapsed_state() const { return collapsed_; }
    void apply_saved_collapsed(bool left, bool right);

    // ---- 公共状态（面板与输入读它） ----
    AppState state;
    AssetStore assets;
    Selection selection;
    LayerViews layer_views;

    bool x_hidden = false;
    bool bbox_hidden = false;
    // "图像"卡的保持比例开关：数据上由横纵倍率是否一致推导，用户点一下才改这个意愿。
    // 变形让它变成非等比时，refresh 会把它同步回未勾选。
    bool layer_lock_ = true;
    bool space_held = false;
    bool space_used = false;
    bool alt_held = false;

    // ---- 自由变换（Ctrl+T）：四角缩放、Ctrl 解除比例、旋转、拖动位置 ----
    void begin_transform();
    void begin_transform_for(Layer* layer);
    void end_transform(bool commit);
    // 变形中途把父层当前的旋转/缩放/位置传给子层（按开始时的起点重算，不叠加）
    void carry_transform_children();
    // 移动吸附：画布四边/中心 + 其它图层的四边/中心，返回调整后的位移
    void snap_move(Layer* layer, double* dx, double* dy, bool allow_snap);
    bool transform_mode = false;
    Layer* transform_layer = nullptr;

    // ---- 参考线（吸附命中时记录世界坐标，参照 ImageEditor 的 snapGuide） ----
    bool guide_x = false;
    bool guide_y = false;
    double guide_wx = 0.0;
    double guide_wy = 0.0;

    Tool tool_ = Tool::kNone;
    bool lasso_closed_ = false;                       // 路径已提交生效
    std::vector<std::pair<double, double>> lasso_path;  // 画布世界坐标
    std::unique_ptr<Region> clipboard_region;
    std::unique_ptr<LayerClip> clipboard_layer;

    // 橡皮的一笔：先在本笔覆盖率遮罩上取最大值盖印，再一次性乘到基线的 alpha 上；
    // 拖动时按脏矩形重传纹理，松手才落成新资产
    bool eraser_active_ = false;
    Layer* eraser_layer_ = nullptr;
    std::vector<unsigned char> eraser_buf_;    // 结果 RGBA（预览/落盘）
    std::vector<unsigned char> eraser_base_;   // 本笔起点 RGBA（alpha 只读）
    std::vector<unsigned char> eraser_cov_;    // 本笔覆盖率 0..255
    int eraser_w_ = 0;
    int eraser_h_ = 0;
    double eraser_last_lx_ = 0.0;
    double eraser_last_ly_ = 0.0;
    int eraser_dirty_[4] = {0, 0, 0, 0};   // x0,y0,x1,y1（右/下开区间）

    struct ViewState {
        double x = 0.0;
        double y = 0.0;
        double scale = 1.0;
    } view;

    std::string drag_kind;
    Layer* drag_layer = nullptr;
    Region* drag_region = nullptr;
    std::string drag_handle;
    std::pair<double, double> drag_last{0.0, 0.0};
    std::pair<double, double> drag_offset{0.0, 0.0};
    bool drag_undo_pushed = false;
    double handle_start_rot = 0.0;
    double handle_start_angle = 0.0;
    // BBOX 手柄拖动起点半径：默认自由（只改一根轴），Ctrl 保持比例。
    double handle_start_rx = 0.0;
    double handle_start_ry = 0.0;

    // 自由变换起点（Enter 前撤销就靠这一组）
    double transform_start_x = 0.0;
    double transform_start_y = 0.0;
    double transform_start_sx = 1.0;
    double transform_start_sy = 1.0;
    double transform_rect_w = 1.0;
    double transform_rect_h = 1.0;
    double transform_anchor_x = 0.0;
    double transform_anchor_y = 0.0;
    // 单次拖角起点（连续拖多个角时每次都从这里算，不叠加）
    double transform_drag_x = 0.0;
    double transform_drag_y = 0.0;
    double transform_drag_sx = 1.0;
    double transform_drag_sy = 1.0;
    double transform_start_rot = 0.0;
    double transform_drag_rot = 0.0;
    double transform_drag_angle = 0.0;   // 旋转：按下时光标相对中心的角度
    double transform_corner_lx = 0.0;    // 被拖角在图层局部坐标的位置
    double transform_corner_ly = 0.0;
    // 拖角缩放的主轴（按下那一刻定死）：
    //   ALT        → 对角不动（锚点 = 对角，当前 ImageEditor 的口径）
    //   无 ALT     → 图像中心不动（对称缩放，位置不变）
    bool transform_scale_center = false;
    double transform_anchor_lx = 0.0;    // 主轴在图层局部坐标的位置
    double transform_anchor_ly = 0.0;

    // 变形态里父层带着子层一起动：开始变形时记下父层与每个子层的起点，
    // 拖动中每帧都用起点重算（不叠加），旋转/缩放/位移都传下去。
    struct KidBasis {
        Layer* layer = nullptr;
        double x = 0.0;
        double y = 0.0;
        double sx = 1.0;
        double sy = 1.0;
        double rot = 0.0;
    };
    struct ParentBasis {
        double x = 0.0;
        double y = 0.0;
        double kx = 1.0;
        double ky = 1.0;
        double rot = 0.0;
    };
    ParentBasis transform_basis_;
    std::vector<KidBasis> transform_kids_;

    // 多边形套索双击判定
    double last_press_time = -1.0;
    double last_press_x = 0.0;
    double last_press_y = 0.0;
    // 画布尺寸不再用"本次运行只取一次"的锁：只要加入图片前文档是空的就重取。

    double mouse_x = 0.0;
    double mouse_y = 0.0;
    double now = 0.0;      // 单调时钟（秒）
    double fps_timer = 0.0;
    int fps_frames = 0;

    std::vector<UndoSnapshot> undo_stack;
    std::vector<UndoSnapshot> redo_stack;   // 撤销过的状态，Ctrl+Shift+Z 重做
    std::vector<std::string> float_order{"param", "layer"};

    bool left_collapsed_tmp = false;

    LayerPanel* layer_panel = nullptr;
    ParamPanel* param_panel = nullptr;
    ToolBoard* tool_board = nullptr;
    CenterStatus* status = nullptr;
    SceneCursors* cursors = nullptr;
    GLFWwindow* window_ = nullptr;

private:
    void clamp_floats(int height);
    void selection_changed();
    void eraser_stamp_at(double lx, double ly);
    void eraser_flush();
    std::vector<Layer*> group_movers(std::vector<Layer*>* covered) const;
    void reset_lasso();
    void canvas_center(double* x, double* y) const;
    void draw_overlay(Painter& painter);
    bool resize_hit(float x, float y, std::string* which,
                    std::string* edge) const;
    void resize_drag(double x, double y);
    void resize_release();
    bool panel_header_press(const std::string& which, double x, double y,
                            int button);
    void panel_header_drag(double x, double y);
    void panel_header_release();
    bool panel_press(const std::string& which, double x, double y, int button,
                     int mods);
    void panel_drag(const std::string& which, double x, double y, double dx,
                    double dy, int mods);
    void panel_release(const std::string& which, double x, double y, int button,
                       int mods);

    int width_ = cfg::WIN_W;
    int height_ = cfg::WIN_H;
    bool was_maximized_ = false;   // 上一帧的最大化档位，用来识别最大化/还原
    std::map<std::string, bool> collapsed_{{"left", false}, {"right", false}};
    PanelFloat param_float{"left", static_cast<float>(cfg::PANEL_W)};
    PanelFloat layer_float{"right", static_cast<float>(cfg::LAYER_PANEL_W)};
    bool header_drag_active = false;
    std::string header_drag_which;
    bool resize_active = false;
    std::string resize_which;
    std::string resize_edge;
    std::string active_which;
    bool exporting_ = false;
    std::unique_ptr<Exporter> exporter_;   // 逐帧渲染，渲完交给后台线程编码
    std::thread encode_thread_;
    std::atomic<bool> encode_finished_{false};
    ExportResult encode_result_;
    std::string encode_error_;
    std::mutex encode_mutex_;
    double encode_t0_ = 0.0;
    bool param_drag_active_ = false;
    // 工具小板的鼠标归属：按下那一刻落在板上就归它，之后拖到哪都不换 owner
    bool board_capture_ = false;
    std::string status_text_;
    double enc_t0 = 0.0;

    // ---- 作者卡（左下角按钮 + 弹出小板）----
    // 图片以 RCDATA 嵌在 exe 里，第一次打开时解码成纹理，之后一直复用。
    void ensure_author_texture();
    void author_button_rect(float* x, float* y, float* w, float* h) const;
    void author_card_rect(float* x, float* y, float* w, float* h) const;
    void draw_author(Painter& painter);
    bool author_press(double x, double y, int button);

    unsigned int author_tex_ = 0;
    int author_sw_ = 0;
    int author_sh_ = 0;
    bool author_open_ = false;
    std::unique_ptr<Texture> author_holder_;   // 内嵌作者图的纹理，随 Scene 释放
};

}  // namespace bl::ui
