#include "ui/param_panel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <GLFW/glfw3.h>

#include "domain/region.h"
#include "ui/painter.h"
#include "ui/scene.h"

namespace bl::ui {
namespace {

const char* kCaretOn = "▲";
const char* kCaretOff = "▼";

// 单行文字按宽度裁：超宽就截断并补一个省略号。按 UTF-8 码点整段取，
// 不会把中文的三字节切开变成乱码。状态栏里那条导出路径最容易顶出面板。
std::string FitLine(Painter& painter, const std::string& text,
                    cfg::FontRole role, bool bold, float max_w) {
    if (max_w <= 0.0f || text.empty()) return text;
    if (painter.measure(text, role, bold) <= max_w) return text;
    const std::string dots = "…";
    const float dots_w = painter.measure(dots, role, bold);
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        const size_t start = i;
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            i += 1;
        } else if ((c >> 5) == 0x06) {
            i += 2;
        } else if ((c >> 4) == 0x0E) {
            i += 3;
        } else {
            i += 4;
        }
        if (i > text.size()) i = text.size();
        const std::string next = out + text.substr(start, i - start);
        if (painter.measure(next, role, bold) + dots_w > max_w) break;
        out = next;
    }
    return out + dots;
}

}  // namespace

void ParamPanel::add(Widget& widget, Kind kind, float base, float lx, int card,
                     int cols, int col) {
    // base 传的是控件顶边距面板顶的偏移；布局按底边放置，所以登记时补上自身高度。
    slots_.push_back(Slot{&widget, kind, base, lx, card, false, cols, col});
    slots_.back().base += widget.h;
}

void ParamPanel::init(Scene* scene) {
    scene_ = scene;
    H_ = scene->height();
    first_top_ = static_cast<float>(cfg::PANEL_HEADER_H + 10);
    left_ = 0.0f;
    bottom_ = 0.0f;
    panel_w_ = scene_->panel_float("param").w;
    view_h_ = static_cast<float>(H_);
    top_ = static_cast<float>(H_);
    layout_widths();
    build();
    card_rect_.assign(cards_.size(), Rect4{0, 0, 0, 0});
    info_pos_.assign(cards_.size(), {0.0f, 0.0f});
    relayout(H_);
}

// ---- 建卡 ----
void ParamPanel::build() {
    preset_names_.clear();
    for (const auto& preset : Presets::get().all())
        preset_names_.push_back(preset.first);

    float top = first_top_;

    // ---- 无缝循环 ----
    {
        Card card;
        card.key = "seamless";
        card.title = "无缝循环";
        card.top = top;
        card.caret.label = kCaretOn;
        cards_.push_back(card);
        const int index = static_cast<int>(cards_.size()) - 1;
        float y = top + cfg::CARD_HEAD_H;

        // 九档帧率收成一个下拉：面板里只占一行，选项列表走弹层
        fps_combo_.label = "帧率 ▾";
        fps_combo_.color = cfg::BTN_SUBTLE;
        fps_combo_.color_hover = cfg::BTN_SUBTLE_HOVER;
        fps_combo_.on_press = [this]() {
            fps_popup_.open = !fps_popup_.open;
            if (fps_popup_.open) fps_popup_.hover = nearest_preset();
        };
        fps_combo_.w = cw_;
        fps_combo_.h = Button::kH;
        add(fps_combo_, Kind::kButton, y, x0_, index);
        buttons_.push_back(&fps_combo_);
        y += Button::kH + 8.0f;

        estimate_btn_.label = "自动对齐并估算完整循环";
        estimate_btn_.color = cfg::BTN_GRAY;
        estimate_btn_.color_hover = cfg::BTN_GRAY_HOVER;
        estimate_btn_.on_press = [this]() { scene_->estimate_and_align(); };
        estimate_btn_.w = cw_;
        estimate_btn_.h = Button::kH;
        add(estimate_btn_, Kind::kButton, y, x0_, index);
        buttons_.push_back(&estimate_btn_);
        y += Button::kH + 12.0f;

        cards_[static_cast<size_t>(index)].height_full = y - top;
        top = y + cfg::CARD_GAP;
    }

    // ---- BBOX 参数 ----
    {
        Card card;
        card.key = "param";
        card.title = "BBOX 参数";
        card.top = top;
        card.caret.label = kCaretOn;
        cards_.push_back(card);
        const int index = static_cast<int>(cards_.size()) - 1;
        float y = top + cfg::CARD_HEAD_H;

        sync_toggle_.label = "所有 BBOX 同步幅度/速度";
        sync_toggle_.value = scene_->state.sync_on;
        sync_toggle_.on_change = [this](bool value) {
            scene_->on_sync_toggle(value);
        };
        sync_toggle_.w = cw_;
        sync_toggle_.h = Toggle::kH;
        sync_toggle_.enabled = false;
        add(sync_toggle_, Kind::kToggle, y, x0_, index);
        y += Toggle::kH + 6.0f;

        const float btn_w = (cw_ - 10.0f) / 3.0f;
        preset_buttons_.resize(preset_names_.size());
        for (size_t k = 0; k < preset_names_.size(); ++k) {
            Button& button = preset_buttons_[k];
            button.label = preset_names_[k];
            button.color = cfg::BTN_SUBTLE;
            button.color_hover = cfg::BTN_SUBTLE_HOVER;
            const std::string name = preset_names_[k];
            button.on_press = [this, name]() { scene_->on_preset(name); };
            button.w = btn_w;
            button.h = Button::kH;
            button.enabled = false;
            add(button, Kind::kButton, y,
                x0_ + (btn_w + 5.0f) * static_cast<float>(k), index, 3, k);
            buttons_.push_back(&button);
        }
        y += Button::kH + 8.0f;

        param_sliders_.resize(kRegionParamDescs.size());
        for (size_t k = 0; k < kRegionParamDescs.size(); ++k) {
            const ParamDesc& desc = kRegionParamDescs[k];
            Slider& slider = param_sliders_[k];
            slider.label = desc.label;
            slider.vmin = static_cast<float>(desc.min);
            slider.vmax = static_cast<float>(desc.max);
            slider.step = static_cast<float>(desc.step);
            slider.format = desc.fmt;
            slider.value = static_cast<float>(desc.min);
            slider.enabled = false;
            const std::string key = desc.key;
            slider.on_change = [this, key](float value) {
                scene_->on_param(key, static_cast<double>(value));
            };
            slider.on_release = [this]() {
                scene_->state.mark_physics_dirty();
            };
            slider.on_start = [this]() { scene_->push_undo(); };
            slider.w = cw_;
            slider.h = Slider::kH;
            add(slider, Kind::kSlider, y, x0_, index);
            sliders_.push_back(&slider);
            y += Slider::kH;
        }
        y += 12.0f;
        cards_[static_cast<size_t>(index)].height_full = y - top;
        top = y + cfg::CARD_GAP;
    }

    // ---- 图像（选中图层的尺寸；与导出卡分开，少一行宽高混淆）----
    {
        Card card;
        card.key = "image";
        card.title = "图像";
        card.top = top;
        card.caret.label = kCaretOn;
        cards_.push_back(card);
        const int index = static_cast<int>(cards_.size()) - 1;
        image_index_ = index;
        float y = top + cfg::CARD_HEAD_H;
        const float row_h = 24.0f;

        lbl_layer_w_base_ = y + row_h * 0.5f;
        layer_w_.w = 50.0f;
        layer_w_.h = TextEntry::kH;
        layer_w_.max_len = 6;
        layer_w_.min_value = 1;
        layer_w_.max_value = 100000;
        layer_w_.on_change = [this](int value) { on_layer_w(value); };
        add(layer_w_, Kind::kEntry, y, x0_ + 18.0f, index, 0);

        lbl_layer_h_base_ = y + row_h * 0.5f;
        layer_h_.w = 50.0f;
        layer_h_.h = TextEntry::kH;
        layer_h_.max_len = 6;
        layer_h_.min_value = 1;
        layer_h_.max_value = 100000;
        layer_h_.on_change = [this](int value) { on_layer_h(value); };
        add(layer_h_, Kind::kEntry, y, x0_ + 94.0f, index, 0);

        // ⤓：把当前画布尺寸读进这两个框（只当尺子，不改画布/导出尺寸）
        layer_fit_.label = "⤓";
        layer_fit_.color = cfg::BTN_SUBTLE;
        layer_fit_.on_press = [this]() { on_layer_fit(); };
        layer_fit_.w = TinyButton::kS;
        layer_fit_.h = TinyButton::kS;
        add(layer_fit_, Kind::kTiny, y, x0_ + 148.0f, index, 0);

        layer_keep_.label = "";
        layer_keep_.value = scene_->layer_keep_ratio();
        layer_keep_.on_change = [this](bool value) { on_layer_keep(value); };
        layer_keep_.w = 24.0f;
        layer_keep_.h = Toggle::kH;
        add(layer_keep_, Kind::kToggle, y, x0_ + cw_ - 24.0f, index, -24);
        y += row_h + 4.0f;
        cards_[static_cast<size_t>(index)].height_full = y - top;
        top = y + cfg::CARD_GAP;
    }

    // ---- 导出 ----
    {
        Card card;
        card.key = "export";
        card.title = "导出";
        card.top = top;
        card.caret.label = kCaretOn;
        cards_.push_back(card);
        const int index = static_cast<int>(cards_.size()) - 1;
        export_index_ = index;
        float y = top + cfg::CARD_HEAD_H;
        const float row_h = 24.0f;

        lbl_width_base_ = y + row_h * 0.5f;
        export_w_.w = 50.0f;
        export_w_.h = TextEntry::kH;
        export_w_.max_len = 6;
        export_w_.min_value = 1;
        export_w_.max_value = 100000;
        export_w_.on_change = [this](int value) { on_export_w(value); };
        add(export_w_, Kind::kEntry, y, x0_ + 18.0f, index, 0);

        lbl_height_base_ = y + row_h * 0.5f;
        export_h_.w = 50.0f;
        export_h_.h = TextEntry::kH;
        export_h_.max_len = 6;
        export_h_.min_value = 1;
        export_h_.max_value = 100000;
        export_h_.on_change = [this](int value) { on_export_h(value); };
        add(export_h_, Kind::kEntry, y, x0_ + 94.0f, index, 0);

        export_reset_.label = "↪";
        export_reset_.color = cfg::BTN_SUBTLE;
        export_reset_.on_press = [this]() {
            scene_->adopt_canvas_from_layer(scene_->selected_layer());
        };
        export_reset_.w = TinyButton::kS;
        export_reset_.h = TinyButton::kS;
        add(export_reset_, Kind::kTiny, y, x0_ + 148.0f, index, 0);

        keep_ratio_.label = "";
        keep_ratio_.value = scene_->state.export_keep_ratio;
        keep_ratio_.on_change = [this](bool value) { on_keep_ratio(value); };
        keep_ratio_.w = 24.0f;
        keep_ratio_.h = Toggle::kH;
        add(keep_ratio_, Kind::kToggle, y, x0_ + cw_ - 24.0f, index, -24);
        y += row_h + 8.0f;

        lossless_.label = scene_->state.export_lossless ? "压缩模式：无损"
                                                        : "压缩模式：有损";
        lossless_.value = scene_->state.export_lossless;
        lossless_.on_change = [this](bool value) { on_lossless(value); };
        lossless_.w = cw_;
        lossless_.h = Toggle::kH;
        add(lossless_, Kind::kToggle, y, x0_, index);
        y += Toggle::kH;

        quality_.label = "质量因子";
        quality_.vmin = 0.0f;
        quality_.vmax = static_cast<float>(cfg::EXPORT_QUALITY_MAX);
        quality_.step = 1.0f;
        quality_.format = "{:.0f}";
        quality_.value = static_cast<float>(scene_->state.export_quality);
        quality_.on_change = [this](float value) {
            scene_->state.export_quality = static_cast<int>(value);
        };
        quality_.on_start = [this]() { scene_->push_undo(); };
        quality_.w = cw_;
        quality_.h = Slider::kH;
        add(quality_, Kind::kSlider, y, x0_, index);
        sliders_.push_back(&quality_);
        y += Slider::kH;

        effort_.label = "压缩级别";
        effort_.vmin = 0.0f;
        effort_.vmax = static_cast<float>(cfg::EXPORT_EFFORT_MAX);
        effort_.step = 1.0f;
        effort_.format = "{:.0f}";
        effort_.value = static_cast<float>(scene_->state.export_effort);
        effort_.on_change = [this](float value) {
            scene_->state.export_effort = static_cast<int>(value);
        };
        effort_.on_start = [this]() { scene_->push_undo(); };
        effort_.w = cw_;
        effort_.h = Slider::kH;
        add(effort_, Kind::kSlider, y, x0_, index);
        sliders_.push_back(&effort_);
        y += Slider::kH;

        alpha_q_.label = "Alpha 通道质量";
        alpha_q_.vmin = 0.0f;
        alpha_q_.vmax = static_cast<float>(cfg::EXPORT_ALPHA_Q_MAX);
        alpha_q_.step = 1.0f;
        alpha_q_.format = "{:.0f}";
        alpha_q_.value = static_cast<float>(scene_->state.export_alpha_q);
        alpha_q_.on_change = [this](float value) {
            scene_->state.export_alpha_q = static_cast<int>(value);
        };
        alpha_q_.on_start = [this]() { scene_->push_undo(); };
        alpha_q_.w = cw_;
        alpha_q_.h = Slider::kH;
        add(alpha_q_, Kind::kSlider, y, x0_, index);
        sliders_.push_back(&alpha_q_);
        y += Slider::kH + 6.0f;

        export_btn_.label = "导出 WebP";
        export_btn_.color = cfg::BTN_ORANGE;
        export_btn_.color_hover = cfg::BTN_ORANGE_HOVER;
        export_btn_.on_press = [this]() { scene_->start_export(); };
        export_btn_.enabled = false;
        export_btn_.w = cw_;
        export_btn_.h = Button::kH;
        add(export_btn_, Kind::kButton, y, x0_, index);
        buttons_.push_back(&export_btn_);
        y += Button::kH + 8.0f;

        progress_base_ = y;
        status_base_ = y + 8.0f;
        y += 28.0f;

        cards_[static_cast<size_t>(index)].height_full = y - top;
    }

    // 卡片标题的折叠热区：整行宽，右侧画小箭头
    for (size_t i = 0; i < cards_.size(); ++i) {
        Card& card = cards_[i];
        HeaderToggle& caret = card.caret;
        caret.label = kCaretOn;
        caret.color = cfg::BTN_SUBTLE;
        const int index = static_cast<int>(i);
        caret.on_press = [this, index]() { toggle_card(index); };
        Slot slot;
        slot.obj = &caret;
        slot.kind = Kind::kCaret;
        slot.base = 0.0f;
        slot.lx = 0.0f;
        slot.card = index;
        slot.head = true;
        slots_.push_back(slot);
    }
}

bool ParamPanel::panel_floating() const {
    return scene_->panel_float("param").floating;
}

// ---- 宽度与重排 ----
void ParamPanel::layout_widths() {
    const PanelFloat& f = scene_->panel_float("param");
    const bool floating = f.floating;
    const float shift =
        (!floating && f.side == "right") ? cfg::COLLAPSED_W : 0.0f;
    const float track =
        (!floating && f.side == "left") ? cfg::COLLAPSED_W : 0.0f;
    card_x_ = 10.0f + shift;
    avail_w_ = std::max(40.0f, f.w - 20.0f - shift - track);
    const float pad = 14.0f;
    cw_ = std::max(20.0f, avail_w_ - pad * 2.0f);
    x0_ = card_x_ + pad;
}

void ParamPanel::repack() {
    float top = first_top_;
    float bottom = first_top_;
    for (Card& card : cards_) {
        card.delta = top - card.top;
        bottom = top + (card.expanded ? card.height_full
                                      : static_cast<float>(cfg::CARD_HEAD_H));
        top = bottom + cfg::CARD_GAP;
    }
    content_bottom_ = bottom;
}

void ParamPanel::place_slots() {
    for (Slot& slot : slots_) {
        Widget* widget = slot.widget();
        const Card& card = cards_[static_cast<size_t>(slot.card)];
        if (slot.head) {
            // 标题行内缩到内容列（与 x0_/cw_ 同口径），caret 右边缘才落在
            // info_pos_ 预留的那一格上，而不是贴着卡片/面板边缘。
            widget->x = left_ + x0_;
            widget->y = top_ - (card.top + slot.base + card.delta)
                         - static_cast<float>(cfg::CARD_HEAD_H) + scroll_;
            widget->w = cw_;
            widget->h = static_cast<float>(cfg::CARD_HEAD_H);
            widget->clip_on = true;
            widget->clip[0] = left_;
            widget->clip[1] = bottom_;
            widget->clip[2] = left_ + panel_w_;
            widget->clip[3] = bottom_ + view_h_;
            continue;
        }
        if (!card.expanded) {
            widget->x = -1000.0f;
            widget->y = -1000.0f;
            continue;
        }
        widget->x = left_ + slot.lx;
        widget->y = top_ - (slot.base + card.delta) + scroll_;
        // 宽度跟着内容宽走：换行宽（浮动/拖宽）时不用重建卡片
        if (slot.cols == 1) {
            widget->w = cw_;
        } else if (slot.cols >= 2) {
            const float item_w =
                (cw_ - 5.0f * static_cast<float>(slot.cols - 1))
                / static_cast<float>(slot.cols);
            widget->w = std::max(10.0f, item_w);
            widget->x = left_ + x0_ + (widget->w + 5.0f)
                                        * static_cast<float>(slot.col);
        } else if (slot.cols < 0) {
            widget->w = static_cast<float>(-slot.cols);
            widget->x = left_ + x0_ + cw_ - widget->w;
        }
        widget->clip_on = panel_floating();
        widget->clip[0] = left_;
        widget->clip[1] = bottom_;
        widget->clip[2] = left_ + panel_w_;
        widget->clip[3] = bottom_ + view_h_;
    }
}

void ParamPanel::relayout(int height) {
    H_ = height;
    layout_widths();
    const PanelFloat& f = scene_->panel_float("param");
    const bool floating = f.floating;
    const bool collapsed = !floating && scene_->is_collapsed(f.side);
    const auto rect = f.rect(static_cast<float>(scene_->width()),
                             static_cast<float>(height), collapsed);
    left_ = rect[0];
    bottom_ = rect[1];
    panel_w_ = rect[2];
    view_h_ = rect[3];
    top_ = bottom_ + view_h_;

    repack();
    max_scroll_ = std::max(0.0f, content_bottom_ - view_h_ + 12.0f);
    scroll_ = std::min(std::max(scroll_, 0.0f), max_scroll_);

    card_rect_.resize(cards_.size());
    info_pos_.resize(cards_.size());
    for (size_t i = 0; i < cards_.size(); ++i) {
        const Card& card = cards_[i];
        const float h = card.expanded ? card.height_full
                                      : static_cast<float>(cfg::CARD_HEAD_H);
        const float card_top = top_ - (card.top + card.delta);
        card_rect_[i] = {left_ + card_x_, card_top - h + scroll_, avail_w_, h};
        info_pos_[i] = {left_ + x0_ + cw_ - HeaderToggle::kS - 6.0f,
                        card_top - cfg::CARD_HEAD_H * 0.5f + scroll_};
    }
    place_slots();
    if (collapsed) {
        for (Slot& slot : slots_) {
            slot.widget()->x = -1000.0f;
            slot.widget()->y = -1000.0f;
        }
    }
    progress_rect_ = {left_ + x0_,
                      top_ - (progress_base_ + cards_[export_index_].delta + 4.0f) + scroll_,
                      cw_, 4.0f};
    status_pos_ = {left_ + x0_,
                   top_ - (status_base_ + cards_[export_index_].delta) + scroll_};
}

// ---- 绘制 ----
void ParamPanel::draw(Painter& painter) {
    refresh_layer_size();   // 每帧跟随（Ctrl+T 拖角后数字要立刻变）
    const PanelFloat& f = scene_->panel_float("param");
    const bool floating = f.floating;
    const bool collapsed = !floating && scene_->is_collapsed(f.side);

    if (floating) {
        painter.rect(left_, bottom_, panel_w_, view_h_, cfg::PANEL_BG);
        painter.rect(left_, bottom_, panel_w_, 1.0f, cfg::PANEL_BORDER);
        painter.rect(left_, bottom_ + view_h_ - 1.0f, panel_w_, 1.0f,
                     cfg::PANEL_BORDER);
        painter.rect(left_, bottom_, 1.0f, view_h_, cfg::PANEL_BORDER);
        painter.rect(left_ + panel_w_ - 1.0f, bottom_, 1.0f, view_h_,
                     cfg::PANEL_BORDER);
    } else {
        painter.rect(left_, 0.0f, panel_w_, static_cast<float>(H_),
                     cfg::PANEL_BG);
        const float inner =
            f.side == "left" ? left_ + panel_w_ - 1.0f : left_ - 1.0f;
        painter.rect(inner, 0.0f, 1.0f, static_cast<float>(H_), cfg::DIVIDER);
        const float bar_x = f.side == "left" ? left_ + panel_w_ - cfg::COLLAPSED_W
                                             : left_;
        painter.rect(bar_x, 0.0f, static_cast<float>(cfg::COLLAPSED_W),
                     static_cast<float>(H_), cfg::BAR_BG);
        const char* glyph = collapsed ? (f.side == "left" ? "▸" : "◂")
                                      : (f.side == "left" ? "◂" : "▸");
        painter.text(glyph, bar_x + cfg::COLLAPSED_W * 0.5f,
                     static_cast<float>(H_) * 0.5f, cfg::FontRole::Title, false,
                     cfg::TEXT_DIM, Align::kCenter);
    }

    if (!collapsed) {
        for (size_t i = 0; i < cards_.size(); ++i) {
            const Card& card = cards_[i];
            const auto& rect = card_rect_[i];
            painter.rect(rect.x, rect.y, rect.w, rect.h, cfg::CARD_BG);
            painter.text(card.title, rect.x + 14.0f,
                         rect.y + rect.h - cfg::CARD_HEAD_H * 0.5f, cfg::FontRole::Title, true,
                         cfg::TEXT);
        }
        painter.text(fps_info_, info_pos_[0].first, info_pos_[0].second, cfg::FontRole::Body,
                     false, cfg::TEXT, Align::kRight);
        painter.text(export_size_info_, info_pos_[export_index_].first,
                     info_pos_[export_index_].second, cfg::FontRole::Body, false, cfg::TEXT,
                     Align::kRight);

        if (cards_[export_index_].expanded) {
            // base 是建卡时的绝对偏移（含 card.top），所以这里只能补 card.delta，
            // 与 slot 的 place_slots 口径一致；再加 card.top 就会把标签压到卡片外。
            const float card3_shift = cards_[export_index_].delta;
            painter.text("宽", left_ + x0_,
                         top_ - (lbl_width_base_ + card3_shift) + scroll_, cfg::FontRole::Body,
                         false, cfg::TEXT);
            painter.text("高", left_ + x0_ + 76.0f,
                         top_ - (lbl_height_base_ + card3_shift) + scroll_, cfg::FontRole::Body,
                         false, cfg::TEXT);
        }

        if (cards_[image_index_].expanded) {
            const float shift = cards_[image_index_].delta;
            painter.text("宽", left_ + x0_,
                         top_ - (lbl_layer_w_base_ + shift) + scroll_, cfg::FontRole::Body,
                         false, cfg::TEXT);
            painter.text("高", left_ + x0_ + 76.0f,
                         top_ - (lbl_layer_h_base_ + shift) + scroll_, cfg::FontRole::Body,
                         false, cfg::TEXT);
        }

        for (Slot& slot : slots_) {
            if (slot.head) continue;
            if (slot.widget()->x < -900.0f) continue;
            switch (slot.kind) {
                case Kind::kSlider:
                    static_cast<Slider*>(slot.obj)->draw(painter);
                    break;
                case Kind::kToggle:
                    static_cast<Toggle*>(slot.obj)->draw(painter);
                    break;
                case Kind::kButton:
                    static_cast<Button*>(slot.obj)->draw(painter);
                    break;
                case Kind::kTiny:
                    static_cast<TinyButton*>(slot.obj)->draw(painter);
                    break;
                case Kind::kEntry:
                    static_cast<TextEntry*>(slot.obj)->draw(painter);
                    break;
                case Kind::kCaret:
                    break;
            }
        }

        for (size_t i = 0; i < cards_.size(); ++i) {
            cards_[i].caret.draw(painter);
        }

        if (cards_[export_index_].expanded && exporting_) {
            painter.rect(progress_rect_.x, progress_rect_.y, progress_rect_.w,
                         progress_rect_.h, cfg::TRACK);
            painter.rect(progress_rect_.x, progress_rect_.y,
                         std::max(1.0f, progress_rect_.w * progress_ratio_),
                         progress_rect_.h, cfg::FILL);
        }
        if (cards_[export_index_].expanded && !export_status_.empty()) {
            // 裁宽与绘制必须是同一个角色（以前这里按 9pt 量、按别的字号画，
            // 量出来的宽度和实际字宽两张皮）。
            const cfg::FontRole role = cfg::FontRole::Body;
            const std::string line =
                FitLine(painter, export_status_, role, false, cw_);
            painter.text_top(line, status_pos_.first, status_pos_.second, role,
                             false, cfg::TEXT);
        }
    }

    if (floating || !collapsed) {
        float header_x = left_;
        float header_w = panel_w_;
        if (!floating) {
            header_x = left_ + (f.side == "right" ? cfg::COLLAPSED_W : 0.0f);
            header_w = panel_w_ - cfg::COLLAPSED_W;
        }
        painter.rect(header_x, top_ - cfg::PANEL_HEADER_H,
                     header_w, static_cast<float>(cfg::PANEL_HEADER_H),
                     cfg::BAR_BG);
        painter.text("参数", header_x + 12.0f,
                     top_ - cfg::PANEL_HEADER_H * 0.5f, cfg::FontRole::Title,
                     true, cfg::TEXT);
        painter.text(fps_label_, header_x + header_w - 12.0f,
                     top_ - cfg::PANEL_HEADER_H * 0.5f, cfg::FontRole::Title, true, cfg::TEXT,
                     Align::kRight);
    }
}

// ---- 状态同步 ----
void ParamPanel::set_fps(int fps) {
    if (fps <= 0) return;
    const double ms = 1000.0 / static_cast<double>(fps);
    if (ms != scene_->state.ms_per_frame) scene_->push_undo();
    scene_->state.ms_per_frame = ms;
    refresh_fps_info();
}

// ---- 帧率下拉 ----
int ParamPanel::nearest_preset() const {
    const double ms = scene_->state.ms_per_frame;
    const double fps = ms > 0.0 ? 1000.0 / ms : 0.0;
    int best = 0;
    double best_gap = 1e9;
    for (size_t i = 0; i < cfg::FPS_PRESETS.size(); ++i) {
        const double gap = std::fabs(fps - static_cast<double>(
                                    cfg::FPS_PRESETS[i]));
        if (gap < best_gap) {
            best_gap = gap;
            best = static_cast<int>(i);
        }
    }
    return best;
}

void ParamPanel::layout_popup() {
    const float item_h = Button::kH;
    fps_popup_.w = cw_;
    fps_popup_.item_h = item_h;
    fps_popup_.h =
        item_h * static_cast<float>(cfg::FPS_PRESETS.size()) + 4.0f;
    fps_popup_.x = fps_combo_.x;
    // 卡片内容自上而下：默认在按钮下面展开；窗口底放不下就翻到按钮上面
    float bottom = fps_combo_.y - fps_popup_.h - 2.0f;
    if (bottom < 2.0f) bottom = fps_combo_.y + fps_combo_.h + 2.0f;
    fps_popup_.bottom = bottom;
}

int ParamPanel::popup_index_at(float x, float y) const {
    if (!fps_popup_.open) return -1;
    if (x < fps_popup_.x || x > fps_popup_.x + fps_popup_.w) return -1;
    if (y < fps_popup_.bottom || y > fps_popup_.bottom + fps_popup_.h) {
        return -1;
    }
    const float top = fps_popup_.bottom + fps_popup_.h - 2.0f;
    if (y > top) return -1;
    const int count = static_cast<int>(cfg::FPS_PRESETS.size());
    const int index =
        static_cast<int>((top - y) / fps_popup_.item_h);
    return index >= 0 && index < count ? index : -1;
}

void ParamPanel::close_popup() {
    fps_popup_.open = false;
    fps_popup_.hover = -1;
}

bool ParamPanel::popup_press(float x, float y, int button) {
    (void)button;
    if (!fps_popup_.open) return false;
    // 面板被收起时下拉按钮已经被挪到屏幕外，弹层跟着失效，不能吞掉这次点击
    if (fps_combo_.x < -900.0f) {
        close_popup();
        return false;
    }
    layout_popup();
    // 再点下拉按钮自己 = 收起（不进 on_press，避免又弹开）
    if (fps_combo_.hit(x, y)) {
        close_popup();
        return true;
    }
    const int index = popup_index_at(x, y);
    close_popup();
    if (index >= 0) set_fps(cfg::FPS_PRESETS[static_cast<size_t>(index)]);
    // 点在弹层外也吞掉这次按下，不能穿透到画布/图层面板
    return true;
}

bool ParamPanel::popup_key(int key, int mods) {
    (void)mods;
    if (!fps_popup_.open) return false;
    if (key == GLFW_KEY_ESCAPE) {
        close_popup();
        return true;
    }
    return false;
}

void ParamPanel::draw_popup(Painter& painter) {
    if (!fps_popup_.open) return;
    if (fps_combo_.x < -900.0f) {
        close_popup();
        return;
    }
    layout_popup();
    const float x = fps_popup_.x;
    const float w = fps_popup_.w;
    const float bottom = fps_popup_.bottom;
    const float h = fps_popup_.h;
    painter.rect(x, bottom, w, h, cfg::PANEL_BG);
    painter.rect(x, bottom, w, 1.0f, cfg::PANEL_BORDER);
    painter.rect(x, bottom + h - 1.0f, w, 1.0f, cfg::PANEL_BORDER);
    painter.rect(x, bottom, 1.0f, h, cfg::PANEL_BORDER);
    painter.rect(x + w - 1.0f, bottom, 1.0f, h, cfg::PANEL_BORDER);

    const double ms = scene_->state.ms_per_frame;
    const double current = ms > 0.0 ? 1000.0 / ms : 0.0;
    const float top = bottom + h - 2.0f;
    for (size_t i = 0; i < cfg::FPS_PRESETS.size(); ++i) {
        const int fps = cfg::FPS_PRESETS[i];
        const float item_bottom =
            top - fps_popup_.item_h * static_cast<float>(i + 1);
        const bool hovered = static_cast<int>(i) == fps_popup_.hover;
        const bool matched =
            std::fabs(current - static_cast<double>(fps)) < 0.5;
        const cfg::Rgba bg = hovered ? cfg::BTN_SUBTLE_HOVER
                              : matched ? cfg::ROW_SEL
                                        : cfg::BTN_SUBTLE;
        painter.rect(x + 2.0f, item_bottom, w - 4.0f, fps_popup_.item_h, bg);
        painter.text(std::to_string(fps) + " fps", x + 10.0f,
                     item_bottom + fps_popup_.item_h * 0.5f, cfg::FontRole::Body, false,
                     cfg::TEXT);
        if (matched) {
            painter.text("✓", x + w - 12.0f,
                         item_bottom + fps_popup_.item_h * 0.5f, cfg::FontRole::Body, false,
                         cfg::TEXT, Align::kCenter);
        }
    }
}

void ParamPanel::refresh_fps_info() {
    // 卡内摘要＝循环速度；面板标题右侧＝实测渲染帧率（由 Scene 每半秒刷新）。
    const double ms = scene_->state.ms_per_frame;
    const double fps = ms > 0.0 ? 1000.0 / ms : 0.0;
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.0f fps %.1f ms", fps, ms);
    fps_info_ = buffer;
    // 下拉按钮显示的就是当前循环帧率，不四舍五入到预设（周期摊分后可能不是整值）
    std::snprintf(buffer, sizeof(buffer), "%.0f fps ▾", fps);
    fps_combo_.label = buffer;
    sync_export();
}

void ParamPanel::set_render_fps(double fps) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "FPS: %.0f", fps);
    fps_label_ = buffer;
}

void ParamPanel::set_has_layers(bool has) {
    const bool busy = scene_->exporting();
    export_btn_.enabled = has && !busy;
    export_w_.enabled = has && !busy;
    export_h_.enabled = has && !busy;
    export_reset_.enabled = has && !busy;
    keep_ratio_.enabled = has && !busy;
    lossless_.enabled = !busy;
    effort_.enabled = !busy;
    const bool lossless = scene_->state.export_lossless;
    quality_.enabled = !busy && !lossless;
    alpha_q_.enabled = !busy && !lossless;
}

void ParamPanel::show_region(Region* region) {
    selected_ = region;
    const bool enabled = region != nullptr && !scene_->selection.multi();
    // 这块 BBOX 是不是"正在生效"：自己的眼睛开着，且所属图层链可见。
    // 不生效时参数一律灰色（Slider/Button 的 enabled 负责变灰 + 手感拒拖）。
    bool active = enabled;
    if (active) {
        const Layer* owner = scene_->state.doc->layer_of(region);
        active = region->visible && owner != nullptr && owner->is_visible_chain();
    }
    if (region != nullptr) {
        for (size_t k = 0; k < param_sliders_.size(); ++k) {
            const ParamDesc& desc = kRegionParamDescs[k];
            param_sliders_[k].set_value(
                static_cast<float>(region->*(desc.member)));
        }
    }
    for (Slider& slider : param_sliders_) slider.enabled = active;
    for (Button& button : preset_buttons_) button.enabled = active;
    sync_toggle_.enabled = active;
    sync_toggle_.value = scene_->state.sync_on;
}

void ParamPanel::refresh_values() {
    sync_toggle_.value = scene_->state.sync_on;
    if (selected_ == nullptr) return;
    for (size_t k = 0; k < param_sliders_.size(); ++k) {
        const ParamDesc& desc = kRegionParamDescs[k];
        param_sliders_[k].set_value(
            static_cast<float>(selected_->*(desc.member)));
    }
}

// ---- 图像卡（选中图层的世界尺寸）----
void ParamPanel::refresh_layer_size() {
    Layer* layer = scene_->selected_layer();
    const bool active = layer != nullptr && !scene_->selection.multi();
    layer_w_.enabled = active;
    layer_h_.enabled = active;
    layer_fit_.enabled = active;
    layer_keep_.enabled = active;

    if (!active) {
        // 未选中 / 多选：整卡灰，框里给"--"
        if (!layer_w_.focused) layer_w_.set_text("--");
        if (!layer_h_.focused) layer_h_.set_text("--");
        layer_keep_.value = false;
        return;
    }
    // 拖手柄把图层弄成非等比后，勾要跟着掉（不是用户点的，是数据决定的）
    if (!scene_->layer_ratio_locked(layer)) scene_->set_layer_keep(false);
    const auto size = scene_->layer_size(layer);
    if (!layer_w_.focused)
        layer_w_.set_value(static_cast<int>(std::lround(size.first)));
    if (!layer_h_.focused)
        layer_h_.set_value(static_cast<int>(std::lround(size.second)));
    layer_keep_.value = scene_->layer_keep_ratio();
}

void ParamPanel::on_layer_w(int value) {
    Layer* layer = scene_->selected_layer();
    if (layer == nullptr || layer->w <= 0 || layer->h <= 0) return;
    const auto size = scene_->layer_size(layer);
    if (std::lround(size.first) == value) return;   // 没真改就不动（防来回漂）
    const double w = static_cast<double>(value);
    const double h = scene_->layer_keep_ratio()
                         ? w * static_cast<double>(layer->h) / layer->w
                         : size.second;
    scene_->set_layer_size(layer, w, h);
    refresh_layer_size();
}

void ParamPanel::on_layer_h(int value) {
    Layer* layer = scene_->selected_layer();
    if (layer == nullptr || layer->w <= 0 || layer->h <= 0) return;
    const auto size = scene_->layer_size(layer);
    if (std::lround(size.second) == value) return;
    const double h = static_cast<double>(value);
    const double w = scene_->layer_keep_ratio()
                         ? h * static_cast<double>(layer->w) / layer->h
                         : size.first;
    scene_->set_layer_size(layer, w, h);
    refresh_layer_size();
}

// ⤓：把画布尺寸填进这两个框。只当尺子——画布与导出尺寸都不动。
void ParamPanel::on_layer_fit() {
    Layer* layer = scene_->selected_layer();
    if (layer == nullptr || layer->w <= 0 || layer->h <= 0) return;
    const auto canvas = scene_->canvas_size();
    const double w = static_cast<double>(canvas.first);
    const double h = scene_->layer_keep_ratio()
                         ? w * static_cast<double>(layer->h) / layer->w
                         : static_cast<double>(canvas.second);
    scene_->set_layer_size(layer, w, h);
    refresh_layer_size();
}

// 点勾：按文件实际宽高结算保比例——以当前宽度为准，把纵倍率对齐过来。
void ParamPanel::on_layer_keep(bool value) {
    scene_->set_layer_keep(value);
    Layer* layer = scene_->selected_layer();
    if (value && layer != nullptr && layer->w > 0 && layer->h > 0) {
        const auto size = scene_->layer_size(layer);
        const double h = size.first * static_cast<double>(layer->h) / layer->w;
        scene_->set_layer_size(layer, size.first, h);
    }
    refresh_layer_size();
}

void ParamPanel::refresh_export_size() {
    scene_->ensure_export_size();
    // ↪ 的语义是"按选中图层的位图尺寸重取画布"，没有选中图层就没有可取的尺寸。
    export_reset_.enabled =
        scene_->selected_layer() != nullptr && !scene_->exporting();
    const int w = scene_->state.export_w;
    const int h = scene_->state.export_h;
    export_size_info_ =
        (w > 0 && h > 0) ? (std::to_string(w) + " × " + std::to_string(h))
                         : std::string("-- × --");
    if (!export_w_.focused) export_w_.set_value(w);
    if (!export_h_.focused) export_h_.set_value(h);
}

void ParamPanel::on_export_w(int value) {
    const int old_w = scene_->state.export_w;
    const int old_h = scene_->state.export_h;
    int h = value;
    if (scene_->state.export_keep_ratio && old_w > 0 && old_h > 0) {
        h = std::max(1, static_cast<int>(std::lround(
                            static_cast<double>(value) * old_h / old_w)));
        export_h_.set_value(h);
    } else {
        h = scene_->canvas_size().second;
    }
    // 尺寸框就是画布边界：改它等于改画布，导出永远跟着画布走
    scene_->set_canvas_size(value, h);
    refresh_export_size();
}

void ParamPanel::on_export_h(int value) {
    const int old_w = scene_->state.export_w;
    const int old_h = scene_->state.export_h;
    int w = value;
    if (scene_->state.export_keep_ratio && old_w > 0 && old_h > 0) {
        w = std::max(1, static_cast<int>(std::lround(
                            static_cast<double>(value) * old_w / old_h)));
        export_w_.set_value(w);
    } else {
        w = scene_->canvas_size().first;
    }
    scene_->set_canvas_size(w, value);
    refresh_export_size();
}

void ParamPanel::on_keep_ratio(bool value) {
    scene_->state.export_keep_ratio = value;
    if (value && export_w_.value() > 0) on_export_w(export_w_.value());
}

void ParamPanel::on_lossless(bool value) {
    if (value != scene_->state.export_lossless) scene_->push_undo();
    scene_->state.export_lossless = value;
    lossless_.label = value ? "压缩模式：无损" : "压缩模式：有损";
    quality_.enabled = !value;
    alpha_q_.enabled = !value;
}

void ParamPanel::sync_export() {
    const AppState& state = scene_->state;
    lossless_.value = state.export_lossless;
    lossless_.label = state.export_lossless ? "压缩模式：无损"
                                            : "压缩模式：有损";
    quality_.set_value(static_cast<float>(state.export_quality));
    effort_.set_value(static_cast<float>(state.export_effort));
    alpha_q_.set_value(static_cast<float>(state.export_alpha_q));
    keep_ratio_.value = state.export_keep_ratio;
    refresh_export_size();
}

void ParamPanel::set_exporting(bool on) {
    exporting_ = on;
    if (on) progress_ratio_ = 0.0f;
    set_has_layers(scene_->has_layers());
}

void ParamPanel::set_progress(int done, int total) {
    progress_ratio_ =
        total > 0 ? static_cast<float>(done) / static_cast<float>(total) : 0.0f;
}

void ParamPanel::set_status(const std::string& text) { export_status_ = text; }

// ---- 输入 ----
IntEntry* ParamPanel::focused_entry() {
    if (export_w_.focused) return &export_w_;
    if (export_h_.focused) return &export_h_;
    return nullptr;
}

bool ParamPanel::entry_press(float x, float y, int button) {
    (void)button;
    if (cards_[export_index_].expanded) {
        if (export_w_.hit(x, y)) {
            if (export_h_.focused) export_h_.unfocus();
            // press() 内部已经 focus() 并把光标落到点击处；再补一次 focus()
            // 会把光标甩到末尾、清掉 sel_anchor，拖拽框选就永远不会出现。
            export_w_.press(x, y);
            return true;
        }
        if (export_h_.hit(x, y)) {
            if (export_w_.focused) export_w_.unfocus();
            export_h_.press(x, y);
            return true;
        }
    }
    IntEntry* focused = focused_entry();
    // 点别处要让刚输入的数字真正落到 state：unfocus 只清焦点，commit 才结算
    if (focused != nullptr) focused->commit();
    return false;
}

bool ParamPanel::entry_drag(float x, float y, double dx, double dy) {
    (void)dx;
    (void)dy;
    IntEntry* entry = focused_entry();
    if (entry == nullptr) return false;
    entry->drag_to(x, y);
    return true;
}

bool ParamPanel::entry_release(float x, float y, int button) {
    (void)button;
    IntEntry* entry = focused_entry();
    if (entry == nullptr) return false;
    entry->release(x, y);
    return true;
}

bool ParamPanel::slider_hit(float x, float y) const {
    for (const Slider* slider : sliders_) {
        if (slider->hit(x, y)) return true;
    }
    return false;
}

bool ParamPanel::press(float x, float y, int button, int mods) {
    (void)button;
    (void)mods;
    for (Slot& slot : slots_) {
        if (slot.head || slot.widget()->x < -900.0f) continue;
        Widget* widget = slot.widget();
        switch (slot.kind) {
            case Kind::kSlider:
                if (static_cast<Slider*>(slot.obj)->press(x, y)) return true;
                break;
            case Kind::kToggle:
                if (static_cast<Toggle*>(slot.obj)->press(x, y)) return true;
                break;
            case Kind::kButton:
                if (static_cast<Button*>(slot.obj)->press(x, y)) return true;
                break;
            case Kind::kTiny:
                if (static_cast<TinyButton*>(slot.obj)->press(x, y)) return true;
                break;
            case Kind::kEntry:
            case Kind::kCaret:
                break;
        }
        (void)widget;
    }
    for (Slot& slot : slots_) {
        if (!slot.head) continue;
        if (slot.widget()->x < -900.0f) continue;
        if (static_cast<HeaderToggle*>(slot.obj)->press(x, y)) return true;
    }
    return false;
}

void ParamPanel::drag(float x, float y, double dx, double dy, int mods) {
    (void)dx;
    (void)dy;
    (void)mods;
    for (Slider* slider : sliders_) {
        if (slider->drag) slider->drag_to(x, y);
    }
}

void ParamPanel::release(float x, float y, int button) {
    (void)button;
    for (Slider* slider : sliders_) {
        if (slider->drag) slider->release(x, y);
    }
}

void ParamPanel::motion(float x, float y) {
    for (Button* button : buttons_) button->motion(x, y);
    if (!fps_popup_.open) return;
    // 弹层可以伸出面板，所以悬停按弹层自己的矩形算，不看面板
    layout_popup();
    fps_popup_.hover = popup_index_at(x, y);
}

void ParamPanel::scroll_by(double dy) {
    close_popup();
    scroll_ = std::min(std::max(scroll_ + static_cast<float>(dy), 0.0f),
                       max_scroll_);
    relayout(H_);
}

void ParamPanel::toggle_card(int index) {
    if (index < 0 || index >= static_cast<int>(cards_.size())) return;
    Card& card = cards_[static_cast<size_t>(index)];
    card.expanded = !card.expanded;
    card.caret.label = card.expanded ? kCaretOn : kCaretOff;
    close_popup();
    relayout(H_);
}

std::map<std::string, bool> ParamPanel::expansion_state() const {
    std::map<std::string, bool> state;
    for (const Card& card : cards_) state[card.key] = card.expanded;
    return state;
}

void ParamPanel::set_expansion(const std::map<std::string, bool>& state) {
    bool changed = false;
    for (Card& card : cards_) {
        auto it = state.find(card.key);
        if (it == state.end()) continue;
        if (it->second != card.expanded) {
            card.expanded = it->second;
            card.caret.label = card.expanded ? kCaretOn : kCaretOff;
            changed = true;
        }
    }
    if (changed) relayout(H_);
}

bool ParamPanel::header_hit(float x, float y) {
    const PanelFloat& f = scene_->panel_float("param");
    const bool collapsed = !f.floating && scene_->is_collapsed(f.side);
    return f.contains_header(x, y, static_cast<float>(scene_->width()),
                             static_cast<float>(H_), collapsed);
}

float ParamPanel::natural_height() const { return content_bottom_ + 12.0f; }

}  // namespace bl::ui




