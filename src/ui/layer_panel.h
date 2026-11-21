// 图层面板：图层树 + 每个图层名下的 BBOX 行。
//
// 行结构由文档派生的两份扁平表组成（图层表 / BBOX 表），重建时只比对内容；
// 选中态、拖放、缩略图都按行处理，不另设控件对象。
//
// 层级：图层缩进一级（LAYER_INDENT），BBOX 再缩一级；行高分别 30 / 22。
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "config.h"
#include "ui/widgets.h"

namespace bl {
struct Layer;
struct Region;
class Texture;
namespace ui {
class Scene;
class Painter;

class LayerPanel {
public:
    void init(Scene* scene);
    void rebuild();          // 文档结构变化后调用（含 BBOX 增删）
    void relayout(int height);
    void draw(Painter& painter);
    // 拖动浮层：不裁剪、最后画，指针可以带出面板（与 Python 版 drag_batch 同职责）
    void draw_ghost(Painter& painter);

    bool header_hit(float x, float y);
    bool press(float x, float y, int button, int mods);
    void drag(float x, float y, double dx, double dy, int mods);
    void release(float x, float y, int button, int mods);
    void motion(float x, float y);
    void scroll_by(double dy);
    float natural_height() const;
    bool press_active() const { return pressing_; }

    // BBOX 改名：双击行内文字进入，回车提交、Esc 取消、点别处提交。
    bool rename_focused() const;
    bool rename_key(EditKey action);
    bool rename_text(const std::string& chunk);
    void commit_rename_outside(float x, float y);

    // 行表（Scene 的拖放判定与测试用）
    struct Row {
        int kind = 0;   // 0 = 图层，1 = BBOX
        Layer* layer = nullptr;
        Region* region = nullptr;
        int depth = 0;
        float y = 0.0f;  // 窗口坐标里的行底边
        float h = 0.0f;
    };
    const std::vector<Row>& rows() const { return rows_; }

private:
    struct Drop {
        bool valid = false;
        Layer* layer = nullptr;   // 插入目标的父（图层拖放）
        int index = 0;
        int bbox_layer_uid = 0;   // BBOX 拖放的目标图层
        int bbox_index = 0;
    };

    // 按住行时跟着鼠标走的半透明浮层（行数据的快照，不引用 rows_）
    struct Ghost {
        bool active = false;
        bool bbox = false;
        int depth = 0;
        int asset_key = 0;
        cfg::Rgba dot{0, 0, 0, 255};
        std::string name;
        float w = 0.0f, h = 0.0f;
        float off_x = 0.0f, off_y = 0.0f;   // 按下点相对行左下角的偏移
        float x = 0.0f, y = 0.0f;           // 当前光标（窗口坐标）
    };

    void layout_widths();
    void compute_rows();
    void place_rows();
    void draw_row(Painter& painter, size_t index);
    void draw_layer_row(Painter& painter, const Row& row, int asset_key);
    void draw_bbox_row(Painter& painter, const Row& row, int index);
    int row_at(float x, float y, size_t* index) const;
    void begin_row_drag(size_t index, float x, float y);
    void update_drop(float x, float y);
    void apply_drop();
    void action_areas(const Row& row, float* add_x, float* eye_x,
                      float* reset_x) const;
    unsigned int thumb_texture(int asset_key);

    // ---- 拖动浮层 ----
    void begin_ghost(const Row& row, float x, float y);
    void move_ghost(float x, float y);
    void hide_ghost();

    // ---- BBOX 改名 ----
    const Row* find_rename_row() const;
    void begin_rename(const Row& row);
    void place_rename(const Row& row);
    void commit_rename();
    void cancel_rename();
    void apply_rename(const std::string& text);
    void destroy_rename();

    Scene* scene_ = nullptr;

    std::vector<Row> rows_;
    std::map<int, std::unique_ptr<Texture>> thumb_textures_;   // 资产键 → 缩略图
    int hover_index_ = -1;

    std::unique_ptr<TextEntry> rename_entry_;
    bool renaming_ = false;
    Layer* rename_layer_ = nullptr;
    Region* rename_region_ = nullptr;
    Region* last_click_region_ = nullptr;   // 双击判定
    double last_click_time_ = 0.0;

    float left_ = 0.0f, bottom_ = 0.0f, w_ = 0.0f, view_h_ = 0.0f, top_ = 0.0f;
    int H_ = 0;
    float row_x_ = 0.0f, row_w_ = 0.0f;
    float content_top_ = 0.0f;
    float content_height_ = 0.0f;

    float scroll_ = 0.0f;
    float max_scroll_ = 0.0f;

    // 拖放
    bool pressing_ = false;
    size_t press_row_ = 0;
    float press_y_ = 0.0f;
    bool dragging_ = false;
    size_t drag_row_ = 0;
    float ghost_y_ = 0.0f;
    Drop drop_;
    Ghost ghost_;
    // 正在被"收为子层"的那一行（-1 = 没有）。只在这个状态画高亮，
    // 判定条件跟 update_drop 一套，不会出现"亮着但不是子层"。
    int drop_child_row_ = -1;
};

}  // namespace ui
}  // namespace bl
