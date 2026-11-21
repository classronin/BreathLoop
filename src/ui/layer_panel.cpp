#include "ui/layer_panel.h"

#include <algorithm>
#include <cmath>

#include <GLFW/glfw3.h>

#include "domain/layers.h"
#include "domain/region.h"
#include "render/assets.h"
#include "render/gfx.h"
#include "ui/painter.h"
#include "ui/param_panel.h"
#include "ui/scene.h"

namespace bl::ui {
namespace {

float RowHeight(int kind) {
    return kind == 0 ? static_cast<float>(cfg::LAYER_ROW_H)
                     : static_cast<float>(cfg::BBOX_ROW_H);
}

// 行内格局：缩略图在最左，后面连排三个按钮（眼睛 / "+" / 重置）。名字已移除。
// 三钮宽度按"这一行实际宽度"现算，绝不越出面板：面板拖窄时三个盒一起缩。
constexpr float kBtnX0 = 6.0f;
constexpr float kBtnGap = 8.0f;
constexpr float kThumbGap = 6.0f;
constexpr float kThumbX = kBtnX0;                                // 缩略图在最左
constexpr float kBtnStart = kThumbX + cfg::THUMB_SIZE + kThumbGap;
constexpr float kBtnMinW = 24.0f;   // 盒再窄也留得住一个图标

float BtnWidth(float row_w, float indent) {
    const float avail = row_w - indent - kBtnStart - kBtnGap * 2.0f;
    return std::max(kBtnMinW, avail / 3.0f);
}

// 名字放不下时截尾补省略号，二分找最长可行前缀。
std::string FitName(Painter& painter, const std::string& name, float width) {
    if (painter.measure(name, cfg::FontRole::Small, false) <= width) return name;
    std::string base = name;
    size_t lo = 0;
    size_t hi = base.size();
    std::string best = "...";
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        const std::string candidate = base.substr(0, mid) + "...";
        if (painter.measure(candidate, cfg::FontRole::Small, false) <= width) {
            best = candidate;
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return best;
}

}  // namespace

void LayerPanel::init(Scene* scene) {
    scene_ = scene;
    H_ = scene->height();
    left_ = static_cast<float>(scene->width() - cfg::LAYER_PANEL_W);
    bottom_ = 0.0f;
    w_ = static_cast<float>(cfg::LAYER_PANEL_W);
    view_h_ = static_cast<float>(H_);
    top_ = static_cast<float>(H_);
    layout_widths();
    rebuild();
}

void LayerPanel::layout_widths() {
    row_x_ = left_ + static_cast<float>(cfg::PANEL_PAD);
    row_w_ = std::max(40.0f, w_ - static_cast<float>(cfg::PANEL_PAD) * 2.0f);
}

// ---- 行结构 ----
void LayerPanel::rebuild() {
    // 行表重建会换掉行对象，正在改名的入口一并收掉（Python 同样先 destroy）
    if (renaming_) destroy_rename();
    compute_rows();
    place_rows();
}

void LayerPanel::compute_rows() {
    rows_.clear();
    if (scene_->state.doc == nullptr) return;
    // 面板自上而下 = 从前景到背景（PS / Python 版口径），所以每一层兄弟
    // （roots、children、bboxes）都按文档里的逆序排：文档内是"后加的在上面"，
    // 面板内是"最前面的排在最上面"。
    struct Walker {
        static void walk(Layer* layer, int depth, std::vector<Row>& rows) {
            Row row;
            row.kind = 0;
            row.layer = layer;
            row.depth = depth;
            row.h = RowHeight(0);
            rows.push_back(row);
            for (int i = static_cast<int>(layer->bboxes.size()) - 1; i >= 0; --i) {
                Row bbox;
                bbox.kind = 1;
                bbox.layer = layer;
                bbox.region = layer->bboxes[static_cast<size_t>(i)].get();
                bbox.depth = depth + 1;
                bbox.h = RowHeight(1);
                rows.push_back(bbox);
            }
            for (int i = static_cast<int>(layer->children.size()) - 1; i >= 0; --i)
                walk(layer->children[static_cast<size_t>(i)].get(), depth + 1,
                     rows);
        }
    };
    const auto& roots = scene_->state.doc->roots();
    for (int i = static_cast<int>(roots.size()) - 1; i >= 0; --i)
        Walker::walk(roots[static_cast<size_t>(i)].get(), 0, rows_);
}

void LayerPanel::place_rows() {
    content_top_ = top_ - static_cast<float>(cfg::PANEL_HEADER_H) - 6.0f;
    float y = content_top_ + scroll_;
    for (Row& row : rows_) {
        row.y = y - row.h;
        y -= row.h;
    }
    content_height_ = content_top_ - y;
    max_scroll_ =
        std::max(0.0f, content_height_ - (view_h_ - cfg::PANEL_HEADER_H) + 8.0f);
    scroll_ = std::min(std::max(scroll_, 0.0f), max_scroll_);
}

void LayerPanel::relayout(int height) {
    H_ = height;
    const PanelFloat& f = scene_->panel_float("layer");
    const bool floating = f.floating;
    const bool collapsed = !floating && scene_->is_collapsed(f.side);
    const auto rect = f.rect(static_cast<float>(scene_->width()),
                             static_cast<float>(height), collapsed);
    left_ = rect[0];
    bottom_ = rect[1];
    w_ = rect[2];
    view_h_ = rect[3];
    top_ = bottom_ + view_h_;
    layout_widths();
    compute_rows();
    place_rows();
}

// ---- 绘制 ----
void LayerPanel::action_areas(const Row& row, float* add_x, float* eye_x,
                              float* reset_x) const {
    // 三个按钮接在缩略图后面连排：眼睛 / "+" / 重置
    const float indent = static_cast<float>(cfg::LAYER_INDENT * row.depth);
    const float bw = BtnWidth(row_w_, indent);
    *eye_x = kBtnStart + bw * 0.5f;
    *add_x = kBtnStart + bw + kBtnGap + bw * 0.5f;
    *reset_x = kBtnStart + (bw + kBtnGap) * 2.0f + bw * 0.5f;
}

void LayerPanel::draw_layer_row(Painter& painter, const Row& row,
                                int asset_key) {
    Layer* layer = row.layer;
    const bool selected = scene_->selection.has_layer(layer);
    const bool hovered = static_cast<int>(&row - rows_.data()) == hover_index_;
    // 按住的那一行交给浮层表现：原行回到默认底色（Python 的 _row_color 同口径）
    const bool pressed =
        pressing_ && static_cast<int>(&row - rows_.data()) ==
                        static_cast<int>(drag_row_);
    const float indent = static_cast<float>(cfg::LAYER_INDENT * row.depth);
    const int row_index = static_cast<int>(&row - rows_.data());
    const bool drop_child = row_index == drop_child_row_;
    if (pressed) {
        // 不铺选中/悬停底色
    } else if (selected) {
        painter.rect(row_x_, row.y, row_w_, row.h, cfg::ROW_SEL);
    } else if (hovered) {
        painter.rect(row_x_, row.y, row_w_, row.h, cfg::ROW_HOVER);
    }
    if (drop_child) {
        // 整行描边（2px 一圈）：不加底色、不染缩略图，落点仍然一眼看清。
        const float t = 2.0f;
        painter.rect(row_x_, row.y, row_w_, t, cfg::FILL);
        painter.rect(row_x_, row.y + row.h - t, row_w_, t, cfg::FILL);
        painter.rect(row_x_, row.y, t, row.h, cfg::FILL);
        painter.rect(row_x_ + row_w_ - t, row.y, t, row.h, cfg::FILL);
    }

    const float base_x = row_x_ + indent;
    const float cy = row.y + row.h * 0.5f;
    float add_x = 0.0f, eye_x = 0.0f, reset_x = 0.0f;
    action_areas(row, &add_x, &eye_x, &reset_x);
    // 缩略图在最左，后面三个按钮：眼睛 / "+" / 重置。图标按 Icon 角色走，盒 70×82。
    painter.text(layer->visible ? "◉" : "○", base_x + eye_x, cy,
                 cfg::FontRole::Icon, false,
                 layer->visible ? cfg::TEXT : cfg::TEXT_DIM, Align::kCenter);
    painter.text("+", base_x + add_x, cy, cfg::FontRole::Icon, true, cfg::TEXT,
                 Align::kCenter);
    painter.text("↪", base_x + reset_x, cy, cfg::FontRole::Icon, true,
                 cfg::TEXT_DIM, Align::kCenter);

    // 缩略图按长边铺满 THUMB_SIZE，保持原比例
    const Asset& asset = scene_->assets.get(asset_key);
    const unsigned int texture = thumb_texture(asset_key);
    const float max_side = static_cast<float>(cfg::THUMB_SIZE);
    float thumb_w = max_side;
    float thumb_h = max_side;
    if (asset.width > 0 && asset.height > 0) {
        if (asset.width >= asset.height) {
            thumb_h = max_side * static_cast<float>(asset.height)
                      / static_cast<float>(asset.width);
        } else {
            thumb_w = max_side * static_cast<float>(asset.width)
                      / static_cast<float>(asset.height);
        }
    }
    const float thumb_x = base_x + kThumbX + (max_side - thumb_w) * 0.5f;
    const float thumb_y = row.y + (row.h - thumb_h) * 0.5f;
    if (texture != 0) {
        painter.sprite(texture, thumb_x, thumb_y, thumb_w, thumb_h);
    }
}

unsigned int LayerPanel::thumb_texture(int asset_key) {
    auto it = thumb_textures_.find(asset_key);
    if (it != thumb_textures_.end()) return it->second->id();
    const Asset& asset = scene_->assets.get(asset_key);
    if (asset.thumb.empty()) return 0;
    std::unique_ptr<Texture> tex(new Texture());
    tex->create(asset.thumb_w, asset.thumb_h, 4, asset.thumb.data());
    const unsigned int id = tex->id();
    thumb_textures_[asset_key] = std::move(tex);
    return id;
}

void LayerPanel::draw_bbox_row(Painter& painter, const Row& row, int index) {
    const bool selected = scene_->selection.has_region(row.region);
    const bool hovered = static_cast<int>(&row - rows_.data()) == hover_index_;
    const bool pressed =
        pressing_ && static_cast<int>(&row - rows_.data()) ==
                        static_cast<int>(drag_row_);
    // BBOX 行落点语义也是"成为所属图层的子层"，所以同样点亮，别让结果和视觉两套。
    const bool drop_child = index == drop_child_row_;
    if (pressed) {
        // 浮层接管按住视觉
    } else if (selected) {
        painter.rect(row_x_, row.y, row_w_, row.h, cfg::ROW_SEL);
    } else if (hovered) {
        painter.rect(row_x_, row.y, row_w_, row.h, cfg::ROW_HOVER);
    }
    if (drop_child) {
        const float t = 2.0f;
        painter.rect(row_x_, row.y, row_w_, t, cfg::FILL);
        painter.rect(row_x_, row.y + row.h - t, row_w_, t, cfg::FILL);
    }
    const float indent = static_cast<float>(cfg::LAYER_INDENT * row.depth);
    const float dot_x = row_x_ + indent + static_cast<float>(cfg::LAYER_INDENT);
    const float cy = row.y + row.h * 0.5f;
    // 左眼睛（32px 字形）、右小方形（16×16 色块），两者间隔 6px
    const float eye_x = dot_x + 18.0f;
    const bool own_eye = row.region->visible;
    const bool owner_eye = row.layer->is_visible_chain();
    painter.text(own_eye ? "◉" : "○", eye_x, cy, cfg::FontRole::IconSmall,
                 false,
                 own_eye && owner_eye ? cfg::TEXT : cfg::TEXT_DIM,
                 Align::kCenter);
    const float square_x = eye_x + 22.0f;
    painter.rect(square_x, cy - 8.0f, 16.0f, 16.0f,
                 scene_->region_color(row.region));
    const float name_x = square_x + 24.0f;
    const float name_w =
        std::max(12.0f, row_x_ + row_w_ - cfg::LAYER_INDENT - name_x - 6.0f);
    if (renaming_ && row.region == rename_region_ && rename_entry_) {
        place_rename(row);
        rename_entry_->draw(painter);
        return;
    }
    painter.text(FitName(painter, row.region->name, name_w), name_x, cy,
                 cfg::FontRole::Body, false,
                 own_eye && owner_eye ? cfg::TEXT : cfg::TEXT_DIM);
}

void LayerPanel::draw(Painter& painter) {
    const PanelFloat& f = scene_->panel_float("layer");
    const bool floating = f.floating;
    const bool collapsed = !floating && scene_->is_collapsed(f.side);

    // 停靠：铺满整列；浮动：只占自己的矩形（与 Python relayout 一致）
    if (floating) {
        painter.rect(left_, bottom_, w_, view_h_, cfg::PANEL_BG);
        painter.rect(left_, bottom_, w_, 1.0f, cfg::PANEL_BORDER);
        painter.rect(left_, bottom_ + view_h_ - 1.0f, w_, 1.0f,
                     cfg::PANEL_BORDER);
        painter.rect(left_, bottom_, 1.0f, view_h_, cfg::PANEL_BORDER);
        painter.rect(left_ + w_ - 1.0f, bottom_, 1.0f, view_h_,
                     cfg::PANEL_BORDER);
    } else {
        painter.rect(left_, 0.0f, w_, static_cast<float>(H_), cfg::PANEL_BG);
        // 内侧（朝画布那一侧）是分隔线和收起条，与 Python relayout 一致
        const float inner = f.side == "left" ? left_ + w_ - 1.0f : left_ - 1.0f;
        painter.rect(inner, 0.0f, 1.0f, static_cast<float>(H_), cfg::DIVIDER);

        const float bar_x =
            f.side == "left" ? left_ + w_ - cfg::COLLAPSED_W : left_;
        painter.rect(bar_x, 0.0f, static_cast<float>(cfg::COLLAPSED_W),
                     static_cast<float>(H_), cfg::BAR_BG);
        const char* glyph = collapsed ? (f.side == "left" ? "▸" : "◂")
                                      : (f.side == "left" ? "◂" : "▸");
        painter.text(glyph, bar_x + cfg::COLLAPSED_W * 0.5f,
                     static_cast<float>(H_) * 0.5f, cfg::FontRole::Title, false, cfg::TEXT_DIM,
                     Align::kCenter);
    }

    if (collapsed) return;

    const float header_offset =
        (floating || f.side == "left") ? 0.0f : cfg::COLLAPSED_W;
    const float header_x = left_ + header_offset;
    const float header_w =
        w_ - (floating ? 0.0f : cfg::COLLAPSED_W);
    painter.rect(header_x, top_ - cfg::PANEL_HEADER_H, header_w,
                 static_cast<float>(cfg::PANEL_HEADER_H), cfg::BAR_BG);
    painter.text("图层", header_x + cfg::PANEL_PAD,
                 top_ - cfg::PANEL_HEADER_H * 0.5f, cfg::FontRole::Title, true, cfg::TEXT);

    const float clip_top = top_ - cfg::PANEL_HEADER_H;
    for (size_t i = 0; i < rows_.size(); ++i) {
        const Row& row = rows_[i];
        if (row.y + row.h < 0.0f || row.y > clip_top) continue;
        if (row.kind == 0) {
            draw_layer_row(painter, row, row.layer->asset_key);
        } else {
            draw_bbox_row(painter, row, static_cast<int>(i));
        }
    }

    if (dragging_ && drop_.valid) {
        painter.rect(row_x_, ghost_y_ - 1.0f, row_w_, 2.0f, cfg::DROP_LINE);
    }
    if (rows_.empty()) {
        painter.text("拖入图片添加图层", header_x + cfg::PANEL_PAD,
                     top_ - cfg::PANEL_HEADER_H - 22.0f, cfg::FontRole::Small, false,
                     cfg::TEXT_DIM);
    }
}

// ---- 命中 ----
int LayerPanel::row_at(float x, float y, size_t* index) const {
    if (!scene_->panel_float("layer").contains(
            x, y, static_cast<float>(scene_->width()),
            static_cast<float>(H_), false)) {
        return 0;
    }
    for (size_t i = 0; i < rows_.size(); ++i) {
        const Row& row = rows_[i];
        if (y >= row.y && y <= row.y + row.h) {
            *index = i;
            return 1;
        }
    }
    // 空区：算作列表末尾
    if (rows_.empty()) return 0;
    *index = rows_.size();
    return 1;
}

// ---- 输入 ----
bool LayerPanel::press(float x, float y, int button, int mods) {
    (void)button;
    if (renaming_) {
        if (rename_entry_ && rename_entry_->hit(x, y)) {
            rename_entry_->press(x, y);
            return true;
        }
        commit_rename();
    }
    size_t index = 0;
    if (!row_at(x, y, &index)) return false;
    if (rows_.empty()) return true;
    if (index >= rows_.size()) return true;

    const Row& row = rows_[index];
    if (row.kind == 0) {
        const float base_x =
            row_x_ + static_cast<float>(cfg::LAYER_INDENT * row.depth);
        float add_x = 0.0f, eye_x = 0.0f, reset_x = 0.0f;
        action_areas(row, &add_x, &eye_x, &reset_x);
        const float bw = BtnWidth(row_w_, static_cast<float>(cfg::LAYER_INDENT
                                                             * row.depth));
        // 判定顺序与画面方向一致：眼睛 → "+" → 重置
        if (x >= base_x + eye_x - bw * 0.5f && x <= base_x + eye_x + bw * 0.5f) {
            scene_->toggle_layer_visible(row.layer);
            return true;
        }
        if (x >= base_x + add_x - bw * 0.5f && x <= base_x + add_x + bw * 0.5f) {
            scene_->add_bbox(row.layer);
            rebuild();
            return true;
        }
        if (x >= base_x + reset_x - bw * 0.5f
            && x <= base_x + reset_x + bw * 0.5f) {
            scene_->center_layer(row.layer);
            return true;
        }
    }

    if ((mods & GLFW_MOD_CONTROL) != 0) {
        scene_->toggle_pick(row.layer, row.kind == 0 ? nullptr : row.region);
    } else if (row.kind == 0) {
        scene_->select_layer(row.layer);
        last_click_region_ = nullptr;
    } else {
        // BBOX 行最左那个大眼睛：命中区跟字形同宽（32px）
        const float indent = static_cast<float>(cfg::LAYER_INDENT * row.depth);
        const float eye_x = row_x_ + indent + cfg::LAYER_INDENT + 18.0f;
        if (x >= eye_x - 16.0f && x <= eye_x + 16.0f) {
            scene_->toggle_region_visible(row.region);
            return true;
        }
        scene_->select_region(row.layer, row.region);
        const double click_time = glfwGetTime();
        if (last_click_region_ == row.region
            && click_time - last_click_time_ < cfg::DOUBLE_CLICK_SECONDS) {
            last_click_region_ = nullptr;
            begin_rename(row);
            return true;
        }
        last_click_region_ = row.region;
        last_click_time_ = click_time;
    }
    begin_row_drag(index, x, y);
    return true;
}

void LayerPanel::begin_row_drag(size_t index, float x, float y) {
    pressing_ = true;
    press_row_ = index;
    press_y_ = y;
    dragging_ = false;
    drop_ = Drop{};
    drag_row_ = index;
    if (index < rows_.size()) begin_ghost(rows_[index], x, y);
}

// ---- 拖动浮层 ----
void LayerPanel::begin_ghost(const Row& row, float x, float y) {
    ghost_ = Ghost{};
    ghost_.active = true;
    ghost_.bbox = row.kind == 1;
    ghost_.depth = row.depth;
    ghost_.w = row_w_;
    ghost_.h = row.h;
    if (ghost_.bbox) {
        ghost_.name = row.region->name;
        ghost_.dot = scene_->region_color(row.region);
    } else {
        ghost_.name = row.layer->name;
        ghost_.asset_key = row.layer->asset_key;
    }
    // 按住点固定在浮层上的同一位置（Python 的 off_x/off_y 同口径）
    ghost_.off_x = x - row_x_;
    ghost_.off_y = y - row.y;
    move_ghost(x, y);
}

void LayerPanel::move_ghost(float x, float y) {
    ghost_.x = x;
    ghost_.y = y;
}

void LayerPanel::hide_ghost() {
    ghost_.active = false;
    drop_child_row_ = -1;   // 高亮只在拖动期间存在
}

void LayerPanel::draw_ghost(Painter& painter) {
    if (!ghost_.active) return;
    const float alpha = static_cast<float>(cfg::DRAG_GHOST_ALPHA) / 255.0f;
    const float gx = ghost_.x - ghost_.off_x;
    const float gy = ghost_.y - ghost_.off_y;
    const float indent = static_cast<float>(cfg::LAYER_INDENT * ghost_.depth);
    const float base_x = gx + indent;
    const float cy = gy + ghost_.h * 0.5f;
    painter.rect(gx, gy, ghost_.w, ghost_.h, cfg::ROW_SEL, alpha);
    if (ghost_.bbox) {
        const float dot_x = base_x + static_cast<float>(cfg::LAYER_INDENT);
        painter.rect(dot_x, cy - 4.0f, 8.0f, 8.0f, ghost_.dot, alpha);
        const float name_x = dot_x + 14.0f;
        painter.text(FitName(painter, ghost_.name,
                             ghost_.w - indent - cfg::LAYER_INDENT - 20.0f),
                     name_x, cy, cfg::FontRole::Small, false, cfg::TEXT, Align::kLeft, alpha);
        return;
    }
    const Asset& asset = scene_->assets.get(ghost_.asset_key);
    const unsigned int texture = thumb_texture(ghost_.asset_key);
    const float max_side = static_cast<float>(cfg::THUMB_SIZE);
    float thumb_w = max_side;
    float thumb_h = max_side;
    if (asset.width > 0 && asset.height > 0) {
        if (asset.width >= asset.height) {
            thumb_h = max_side * static_cast<float>(asset.height)
                      / static_cast<float>(asset.width);
        } else {
            thumb_w = max_side * static_cast<float>(asset.width)
                      / static_cast<float>(asset.height);
        }
    }
    if (texture != 0) {
        painter.sprite(texture,
                       base_x + kThumbX + (max_side - thumb_w) * 0.5f,
                       gy + (ghost_.h - thumb_h) * 0.5f, thumb_w, thumb_h,
                       alpha);
    }
}

// 目标层是不是自己或自己的子孙：挂上去会成环，move_layer 会拒绝，
// 所以这种目标不给"收为子层"的高亮，免得亮着却落空。
bool SelfOrDescendant(const Layer* node, const Layer* maybe_ancestor) {
    while (node != nullptr) {
        if (node == maybe_ancestor) return true;
        node = node->parent;
    }
    return false;
}

void LayerPanel::update_drop(float x, float y) {
    drop_ = Drop{};
    drop_child_row_ = -1;
    if (drag_row_ >= rows_.size()) return;
    const Row& dragged = rows_[drag_row_];
    size_t index = 0;
    if (!row_at(x, y, &index)) return;
    drop_.valid = true;
    // 列表最下面 = 最底层，对应文档里索引 0（最后画的在后面）
    if (index >= rows_.size()) {
        if (dragged.kind == 1) {
            drop_.bbox_layer_uid = dragged.layer->uid;
            drop_.bbox_index = 0;
        } else {
            drop_.layer = nullptr;
            drop_.index = 0;
        }
        ghost_y_ = rows_.empty() ? content_top_ : rows_.back().y;
        return;
    }
    const Row& target = rows_[index];
    ghost_y_ = target.y + target.h;

    if (dragged.kind == 1) {
        drop_.bbox_layer_uid = target.layer->uid;
        if (target.kind == 1) {
            int local = 0;
            const auto& boxes = target.layer->bboxes;
            for (size_t i = 0; i < boxes.size(); ++i) {
                if (boxes[i].get() == target.region) {
                    local = static_cast<int>(i);
                    break;
                }
            }
            drop_.bbox_index = local + 1;
        } else {
            drop_.bbox_index = static_cast<int>(target.layer->bboxes.size());
        }
        return;
    }

    // 面板自上而下是前景→背景：上半 = 排到目标前面（文档索引 +1），
    // 下半 = 成为目标的子层且排在最前（children 末尾）。
    // 落点规则：横向压在缩略图上 = 收为该图层的子层；
    // 在缩略图之外就只按纵向分：上半 = 排到目标前面，下半 = 排到目标后面。
    // y 向上为正，row.y 是行底边，所以 y >= 行中心是视觉上半。
    const double half = target.y + target.h * 0.5;
    if (target.kind == 0) {
        const float indent =
            static_cast<float>(cfg::LAYER_INDENT * target.depth);
        const float thumb_left = row_x_ + indent + kThumbX;
        const float thumb_right = thumb_left + static_cast<float>(cfg::THUMB_SIZE);
        const bool on_thumb = x >= thumb_left && x <= thumb_right;
        if (on_thumb) {
            drop_.layer = target.layer;
            drop_.index = static_cast<int>(target.layer->children.size());
            if (!SelfOrDescendant(target.layer, dragged.layer)) {
                drop_child_row_ = static_cast<int>(index);
            }
        } else if (y >= half) {
            drop_.layer = target.layer->parent;
            drop_.index = scene_->state.doc->sibling_index(target.layer) + 1;
        } else {
            // 排到目标后面：插在目标自己原来的位置上，目标被推到前面。
            drop_.layer = target.layer->parent;
            drop_.index = scene_->state.doc->sibling_index(target.layer);
        }
    } else {
        drop_.layer = target.layer;
        drop_.index = static_cast<int>(target.layer->children.size());
        if (!SelfOrDescendant(target.layer, dragged.layer)) {
            drop_child_row_ = static_cast<int>(index);
        }
    }
}

void LayerPanel::drag(float x, float y, double dx, double dy, int mods) {
    (void)dx;
    (void)dy;
    (void)mods;
    if (renaming_) {
        if (rename_entry_ && rename_entry_->drag) rename_entry_->drag_to(x, y);
        return;
    }
    if (!pressing_) return;
    // 阈值未到也先让浮层跟手（Python 的 handle_drag 同顺序：先 layout 再判阈值）
    if (!dragging_ && std::fabs(y - press_y_) < cfg::ROW_DRAG_MIN) {
        move_ghost(x, y);
        return;
    }
    dragging_ = true;
    move_ghost(x, y);
    update_drop(x, y);
}

void LayerPanel::apply_drop() {
    if (!drop_.valid || drag_row_ >= rows_.size()) return;
    const Row& dragged = rows_[drag_row_];
    scene_->push_undo();
    if (dragged.kind == 1) {
        Layer* target = scene_->state.doc->find(drop_.bbox_layer_uid);
        if (target != nullptr)
            scene_->move_bbox(dragged.layer, dragged.region, target,
                              drop_.bbox_index);
    } else {
        scene_->reorder_layer(dragged.layer, drop_.layer, drop_.index);
    }
    rebuild();
}

void LayerPanel::release(float x, float y, int button, int mods) {
    (void)x;
    (void)y;
    (void)button;
    (void)mods;
    if (renaming_) {
        if (rename_entry_) rename_entry_->release(x, y);
    }
    if (dragging_) apply_drop();
    hide_ghost();
    pressing_ = false;
    dragging_ = false;
    drop_ = Drop{};
}

void LayerPanel::motion(float x, float y) {
    size_t index = 0;
    const int next = row_at(x, y, &index) && index < rows_.size()
                         ? static_cast<int>(index)
                         : -1;
    hover_index_ = next;
}

void LayerPanel::scroll_by(double dy) {
    scroll_ = std::min(std::max(scroll_ + static_cast<float>(dy), 0.0f),
                       max_scroll_);
    place_rows();
}

bool LayerPanel::header_hit(float x, float y) {
    const PanelFloat& f = scene_->panel_float("layer");
    const bool collapsed = !f.floating && scene_->is_collapsed(f.side);
    return f.contains_header(x, y, static_cast<float>(scene_->width()),
                             static_cast<float>(H_), collapsed);
}

float LayerPanel::natural_height() const {
    return content_height_ + cfg::PANEL_HEADER_H + 12.0f;
}

// ---- BBOX 改名 ----
const LayerPanel::Row* LayerPanel::find_rename_row() const {
    for (const Row& row : rows_) {
        if (row.kind == 1 && row.region == rename_region_) return &row;
    }
    return nullptr;
}

void LayerPanel::begin_rename(const Row& row) {
    commit_rename();
    if (row.region == nullptr || row.layer == nullptr) return;
    const Row* current = nullptr;
    for (const Row& candidate : rows_) {
        if (candidate.kind == 1 && candidate.region == row.region) {
            current = &candidate;
            break;
        }
    }
    if (current == nullptr) return;

    rename_region_ = row.region;
    rename_layer_ = row.layer;
    rename_entry_ = std::make_unique<TextEntry>();
    rename_entry_->max_len = 24;
    rename_entry_->on_commit = [this](const std::string& text) {
        apply_rename(text);
    };
    rename_entry_->set_text(row.region->name);
    renaming_ = true;
    place_rename(*current);
    rename_entry_->focus();
    rename_entry_->sel_anchor = 0;
    rename_entry_->cursor = static_cast<int>(rename_entry_->text.size());
}

void LayerPanel::place_rename(const Row& row) {
    if (!rename_entry_) return;
    const float indent = static_cast<float>(cfg::LAYER_INDENT * row.depth);
    const float name_x =
        row_x_ + indent + static_cast<float>(cfg::LAYER_INDENT) + 14.0f;
    rename_entry_->x = name_x;
    rename_entry_->y = row.y + (row.h - TextEntry::kH) * 0.5f;
    rename_entry_->w =
        std::max(40.0f, row_x_ + row_w_ - name_x - 20.0f);
    rename_entry_->h = TextEntry::kH;
}

void LayerPanel::apply_rename(const std::string& text) {
    if (rename_region_ == nullptr) return;
    std::string trimmed = text;
    const size_t first = trimmed.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        trimmed.clear();
    } else {
        const size_t last = trimmed.find_last_not_of(" \t\r\n");
        trimmed = trimmed.substr(first, last - first + 1);
    }
    if (!trimmed.empty() && trimmed != rename_region_->name) {
        scene_->push_undo();
        rename_region_->name = trimmed;
        if (scene_->selection.has_region(rename_region_)) {
            scene_->param_panel->show_region(rename_region_);
        }
    }
}

void LayerPanel::destroy_rename() {
    rename_entry_.reset();
    renaming_ = false;
    rename_layer_ = nullptr;
    rename_region_ = nullptr;
}

void LayerPanel::commit_rename() {
    if (!renaming_) return;
    if (rename_entry_) rename_entry_->commit();
    destroy_rename();
    rebuild();
}

void LayerPanel::cancel_rename() {
    if (!renaming_) return;
    if (rename_entry_) rename_entry_->cancel();
    destroy_rename();
    rebuild();
}

bool LayerPanel::rename_focused() const {
    return renaming_ && rename_entry_ && rename_entry_->focused;
}

bool LayerPanel::rename_key(EditKey action) {
    if (!rename_focused()) return false;
    rename_entry_->key(action);
    if (!rename_entry_->focused) {
        if (action == EditKey::kEnter) {
            commit_rename();
        } else {
            cancel_rename();
        }
    }
    return true;
}

bool LayerPanel::rename_text(const std::string& chunk) {
    if (!rename_focused()) return false;
    rename_entry_->insert_text(chunk);
    return true;
}

void LayerPanel::commit_rename_outside(float x, float y) {
    if (!renaming_) return;
    if (rename_entry_ && rename_entry_->hit(x, y)) return;
    commit_rename();
}

}  // namespace bl::ui


