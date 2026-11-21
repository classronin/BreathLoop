// 参数面板：无缝循环、当前 BBOX 参数、导出（WebP 动画）。
//
// 宽度由 PanelFloat 决定：浮动/停靠拖宽后，卡片与控件按新宽度重排；
// 内容超出高度时整列滚动。所有控件从 state 单向取值，回调只把变化交回 Scene。
//
// 卡片各自申报内容高度（建卡时算一次，每帧重排累加），标题行整行可点，
// 只收缩卡片自己：折叠后卡片退化成一行标题，后面的卡片跟着往上提。
#pragma once

#include <array>
#include <map>
#include <string>
#include <vector>

#include "config.h"
#include "ui/widgets.h"

namespace bl {
struct Region;
namespace ui {
class Scene;
class Painter;

class ParamPanel {
public:
    void init(Scene* scene);

    void relayout(int height);
    void draw(Painter& painter);

    bool header_hit(float x, float y);
    float natural_height() const;

    // ---- 状态同步 ----
    void set_has_layers(bool has);
    void show_region(Region* region);
    void refresh_values();
    void refresh_fps_info();
    void set_render_fps(double fps);
    void refresh_export_size();
    void sync_export();
    void set_exporting(bool on);
    void set_progress(int done, int total);
    void set_status(const std::string& text);

    // ---- 输入 ----
    IntEntry* focused_entry();
    bool entry_press(float x, float y, int button);
    bool entry_drag(float x, float y, double dx, double dy);
    bool entry_release(float x, float y, int button);
    bool slider_hit(float x, float y) const;
    bool press(float x, float y, int button, int mods);
    void drag(float x, float y, double dx, double dy, int mods);
    void release(float x, float y, int button);
    void motion(float x, float y);
    void scroll_by(double dy);

    // ---- 帧率下拉（轻量自绘）----
    // 面板里只有一个按钮，选项列表由 Scene 在最后一层（不裁剪）画，
    // 命中也要在 Scene 的 press 顺序最前面判：弹层伸出面板的那截不能漏给画布。
    bool popup_open() const { return fps_popup_.open; }
    bool popup_press(float x, float y, int button);
    bool popup_key(int key, int mods);
    void draw_popup(Painter& painter);
    void close_popup();

    // ---- 卡片折叠 ----
    std::map<std::string, bool> expansion_state() const;
    void set_expansion(const std::map<std::string, bool>& state);
    void toggle_card(int index);

    float panel_left() const { return left_; }
    float panel_bottom() const { return bottom_; }
    float panel_w() const { return panel_w_; }
    float panel_h() const { return view_h_; }

private:
    enum class Kind { kSlider, kToggle, kButton, kTiny, kEntry, kCaret };

    struct Card {
        std::string key;
        std::string title;
        float top = 0.0f;          // 全展开布局下距面板顶的距离
        float height_full = 0.0f;
        bool expanded = true;
        float delta = 0.0f;
        HeaderToggle caret;
    };

    struct Slot {
        void* obj = nullptr;
        Kind kind = Kind::kButton;
        float base = 0.0f;   // 距面板顶（全展开）到控件顶部
        float lx = 0.0f;
        int card = 0;
        bool head = false;   // 折叠时仍显示（标题行）
        // 宽度规则：0=固定（build 时定死）；1=占满内容宽；n>=2 表示一行 n 等分
        // 的第 col 格；负数=固定宽度 |n| 且贴右。
        int cols = 1;
        int col = 0;
        Widget* widget() const { return static_cast<Widget*>(obj); }
    };

    struct Rect4 {
        float x, y, w, h;
    };

    // 帧率下拉的弹层：位置由下拉按钮的矩形推出，每帧/每次命中前重算
    struct Popup {
        bool open = false;
        float x = 0.0f, bottom = 0.0f, w = 0.0f, h = 0.0f;
        float item_h = 0.0f;
        int hover = -1;
    };

    void build();
    bool panel_floating() const;
    void layout_widths();
    void repack();
    void place_slots();
    void add(Widget& widget, Kind kind, float base, float lx, int card,
             int cols = 1, int col = 0);
    void set_fps(int fps);
    void layout_popup();
    int popup_index_at(float x, float y) const;
    int nearest_preset() const;
    void on_export_w(int value);
    void on_export_h(int value);
    void refresh_layer_size();
    void on_layer_w(int value);
    void on_layer_h(int value);
    void on_layer_fit();
    void on_layer_keep(bool value);
    void on_keep_ratio(bool value);
    void on_lossless(bool value);

    Scene* scene_ = nullptr;

    std::vector<Card> cards_;
    std::vector<Slot> slots_;
    std::vector<Slider*> sliders_;
    std::vector<Button*> buttons_;

    Button fps_combo_;
    Button estimate_btn_;
    Toggle sync_toggle_;
    std::vector<Button> preset_buttons_;
    std::vector<Slider> param_sliders_;
    std::vector<std::string> preset_names_;
    IntEntry export_w_;
    IntEntry export_h_;
    TinyButton export_reset_;
    // 图像卡：宽/高（画布世界像素）+ 取画布尺寸 + 保持比例
    IntEntry layer_w_;
    IntEntry layer_h_;
    TinyButton layer_fit_;
    Toggle layer_keep_;
    Toggle keep_ratio_;
    Toggle lossless_;
    Slider quality_;
    Slider effort_;
    Slider alpha_q_;
    Button export_btn_;

    Region* selected_ = nullptr;

    float scroll_ = 0.0f;
    float max_scroll_ = 0.0f;
    float content_bottom_ = 0.0f;
    float first_top_ = 0.0f;

    float left_ = 0.0f, bottom_ = 0.0f, panel_w_ = 0.0f, view_h_ = 0.0f;
    float top_ = 0.0f;
    float card_x_ = 0.0f, avail_w_ = 0.0f, cw_ = 0.0f, x0_ = 0.0f;
    int H_ = 0;

    float progress_base_ = 0.0f;
    float status_base_ = 0.0f;
    float lbl_width_base_ = 0.0f;
    float lbl_height_base_ = 0.0f;
    // 导出卡的索引不写死：卡片顺序变了（以后插卡）也得对得上
    int export_index_ = 0;
    int image_index_ = 0;      // "图像"卡
    float lbl_layer_w_base_ = 0.0f;
    float lbl_layer_h_base_ = 0.0f;

    std::string fps_info_;
    std::string fps_label_ = "FPS: --";
    std::string export_size_info_ = "-- × --";

    Rect4 progress_rect_{0, 0, 0, 0};
    std::vector<Rect4> card_rect_;
    std::pair<float, float> status_pos_{0, 0};
    std::vector<std::pair<float, float>> info_pos_;

    bool exporting_ = false;
    std::string export_status_;
    float progress_ratio_ = 0.0f;
    Popup fps_popup_;
};

}  // namespace ui
}  // namespace bl
