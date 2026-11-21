// 控件实现：只画、只判命中；布局由面板每帧给坐标。
#include "ui/widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "ui/text.h"

namespace bl::ui {
namespace {

// 控件正文一律按 Body 角色度量：字号只在 config.h 的字号表里定，
// 度量与绘制必须同一个角色，否则光标/选中区会和字对不上。
const cfg::FontRole kBodyRole = cfg::FontRole::Body;

float MeasureBody(const std::string& text) {
    return Text::Instance().measure(text, PointToPx(cfg::FontPointOf(kBodyRole)),
                                    false);
}

std::string FormatFloat(const std::string& pattern, float value) {
    // 只支持界面用到的两种："{:.0f}" 与 "{:.1f}"
    int digits = 0;
    const size_t pos = pattern.find(":.", 0);
    if (pos != std::string::npos && pos + 2 < pattern.size()) {
        digits = pattern[pos + 2] - '0';
        if (digits < 0 || digits > 6) digits = 0;
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", digits,
                  static_cast<double>(value));
    return std::string(buffer);
}

cfg::Rgba WithAlpha(cfg::Rgba color, int alpha) {
    color.a = static_cast<unsigned char>(alpha);
    return color;
}

}  // namespace

bool Widget::clipped(float px, float py) const {
    if (!clip_on) return false;
    return !(clip[0] <= px && px <= clip[2] && clip[1] <= py && py <= clip[3]);
}

bool Widget::hit(float px, float py) const {
    return enabled && !clipped(px, py) && x <= px && px <= x + w && y <= py
           && py <= y + h;
}

// ---- Slider ----
void Slider::set_value(float v, bool notify) {
    v = std::min(std::max(v, vmin), vmax);
    if (step > 0.0f) {
        v = vmin + std::round((v - vmin) / step) * step;
        v = std::min(std::max(v, vmin), vmax);
    }
    if (v == value && !text.empty()) return;
    value = v;
    refresh();
    if (notify && on_change) on_change(value);
}

std::string Slider::format_value() const { return FormatFloat(format, value); }

void Slider::refresh() { text = format_value(); }

void Slider::draw(Painter& painter) const {
    const float alpha = enabled ? 1.0f : 80.0f / 255.0f;
    const cfg::Rgba label_color =
        enabled ? cfg::TEXT : WithAlpha(cfg::TEXT, 160);
    painter.text(label, x, y + kH * 0.5f, kBodyRole, false, label_color);
    painter.text(text, x + w, y + kH * 0.5f, kBodyRole, false, label_color,
                 Align::kRight);
    painter.rect(x, y + 4.0f, w, 3.0f, cfg::TRACK, alpha);
    const float frac = (vmax > vmin) ? (value - vmin) / (vmax - vmin) : 0.0f;
    const float knob_x = x + frac * w;
    painter.rect(x, y + 4.0f, std::max(0.0f, knob_x - x), 3.0f, cfg::FILL,
                 alpha);
    painter.rect(knob_x - 1.0f, y + 2.0f, 2.0f, 7.0f, cfg::KNOB, alpha);
}

void Slider::from_x(float px) {
    const float frac =
        std::min(std::max((px - x) / (w > 0.0f ? w : 1.0f), 0.0f), 1.0f);
    set_value(vmin + frac * (vmax - vmin), true);
}

bool Slider::press(float px, float py) {
    if (!enabled) return false;   // 灰色 = 不可拖（BBOX 眼睛关掉后的参数就这样）
    if (!hit(px, py)) return false;
    if (on_start) on_start();
    drag = true;
    from_x(px);
    return true;
}

void Slider::drag_to(float px, float py) {
    (void)py;
    if (enabled && drag) from_x(px);
}

void Slider::release(float px, float py) {
    (void)px;
    (void)py;
    if (drag && on_release) on_release();
    drag = false;
}

bool Slider::scroll(float px, float py, float dy) {
    if (!hit(px, py) || dy == 0.0f) return false;
    if (on_start) on_start();
    set_value(value + step * (dy > 0.0f ? 1.0f : -1.0f), true);
    return true;
}

// ---- Toggle ----
void Toggle::draw(Painter& painter) const {
    const float alpha = enabled ? 1.0f : 80.0f / 255.0f;
    painter.text(label, x, y + kH * 0.5f, kBodyRole, false,
                 enabled ? cfg::TEXT : WithAlpha(cfg::TEXT, 160));
    const float box_x = x + w - kS;
    const float box_y = y + (kH - kS) * 0.5f;
    painter.rect(box_x, box_y, kS, kS, value ? cfg::FILL : cfg::ENTRY_BG,
                 alpha);
    if (value) {
        painter.text("✓", x + w - kS * 0.5f, y + kH * 0.5f, kBodyRole, false,
                     WithAlpha(cfg::FromRgb(255, 255, 255), enabled ? 255 : 80),
                     Align::kCenter);
    }
}

bool Toggle::press(float px, float py) {
    if (!hit(px, py)) return false;
    value = !value;
    if (on_change) on_change(value);
    return true;
}

// ---- Button ----
void Button::draw(Painter& painter) const {
    const float alpha = enabled ? 1.0f : 80.0f / 255.0f;
    painter.rect(x, y, w, kH, hover && enabled ? color_hover : color, alpha);
    painter.text(label, x + w * 0.5f, y + kH * 0.5f, kBodyRole, false,
                 WithAlpha(cfg::FromRgb(255, 255, 255), enabled ? 255 : 80),
                 Align::kCenter);
}

bool Button::press(float px, float py) {
    if (!hit(px, py)) return false;
    if (on_press) on_press();
    return true;
}

void Button::motion(float px, float py) {
    if (!enabled) {
        hover = false;
        return;
    }
    hover = hit(px, py);
}

// ---- TinyButton ----
void TinyButton::draw(Painter& painter) const {
    const float alpha = enabled ? 1.0f : 60.0f / 255.0f;
    painter.rect(x, y, kS, kS, color, alpha);
    painter.text(label, x + kS * 0.5f, y + kS * 0.5f, kBodyRole, false,
                 WithAlpha(cfg::FromRgb(255, 255, 255), enabled ? 255 : 60),
                 Align::kCenter);
}

bool TinyButton::press(float px, float py) {
    if (!hit(px, py)) return false;
    if (on_press) on_press();
    return true;
}

// ---- HeaderToggle ----
void HeaderToggle::draw(Painter& painter) const {
    const float alpha = enabled ? 1.0f : 60.0f / 255.0f;
    const float bx = x + w - kS;
    const float by = y + (h - kS) * 0.5f;
    painter.rect(bx, by, kS, kS, color, alpha);
    painter.text(label, bx + kS * 0.5f, by + kS * 0.5f, kBodyRole, false,
                 WithAlpha(cfg::FromRgb(255, 255, 255), enabled ? 255 : 60),
                 Align::kCenter);
}

bool HeaderToggle::press(float px, float py) {
    if (!hit(px, py)) return false;
    if (on_press) on_press();
    return true;
}

// ---- TextEntry ----
void TextEntry::set_text(const std::string& value) {
    text = value;
    committed = value;
    cursor = static_cast<int>(text.size());
    sel_anchor = -1;
    update_cursor();
}

int TextEntry::index_at(float px) const {
    const float local = px - (x + 4.0f);
    if (local <= 0.0f) return 0;
    float acc = 0.0f;
    int index = 0;
    size_t byte = 0;
    while (byte < text.size()) {
        size_t next = byte;
        DecodeUtf8(text, &next);
        const float advance =
            Text::Instance().measure(text.substr(byte, next - byte),
                                     PointToPx(cfg::FontPointOf(kBodyRole)),
                                     false);
        if (acc + advance * 0.5f > local) return index;
        acc += advance;
        byte = next;
        ++index;
    }
    return index;
}

bool TextEntry::delete_selection() {
    if (sel_anchor < 0 || sel_anchor == cursor) return false;
    const int a = std::min(sel_anchor, cursor);
    const int b = std::max(sel_anchor, cursor);
    text = text.substr(0, static_cast<size_t>(a))
           + text.substr(static_cast<size_t>(b));
    cursor = a;
    sel_anchor = -1;
    return true;
}

void TextEntry::update_cursor() {
    if (cursor < 0) cursor = 0;
    if (cursor > static_cast<int>(text.size()))
        cursor = static_cast<int>(text.size());
}

bool TextEntry::accept_char(uint32_t cp) const {
    return cp >= 32 && cp != '\t' && cp != '\r' && cp != '\n';
}

void TextEntry::insert_text(const std::string& chunk) {
    if (!focused || !enabled) return;
    for (char ch : chunk) {
        if (static_cast<unsigned char>(ch) < 32) continue;
        delete_selection();
        if (static_cast<int>(text.size()) >= max_len) return;
        text.insert(static_cast<size_t>(cursor), 1, ch);
        ++cursor;
        text_changed();
    }
}

void TextEntry::focus() {
    focused = true;
    cursor = static_cast<int>(text.size());
    sel_anchor = -1;
    update_cursor();
}

void TextEntry::unfocus() {
    focused = false;
    sel_anchor = -1;
    drag = false;
}

void TextEntry::commit() {
    if (!focused && text == committed) return;
    unfocus();
    const std::string value = text;
    if (value != committed) {
        committed = value;
        if (on_commit) on_commit(value);
    }
}

void TextEntry::cancel() {
    text = committed;
    cursor = static_cast<int>(text.size());
    text_changed();
    unfocus();
}

float TextEntry::draw(Painter& painter) const {
    const float alpha = enabled ? 1.0f : 80.0f / 255.0f;
    painter.rect(x, y, w, kH, cfg::ENTRY_BG, alpha);
    painter.text(text, x + 4.0f, y + kH * 0.5f, kBodyRole, false,
                 WithAlpha(cfg::TEXT, enabled ? 255 : 160));
    if (focused && enabled) {
        const int a = sel_anchor >= 0 ? std::min(sel_anchor, cursor) : cursor;
        const int b = sel_anchor >= 0 ? std::max(sel_anchor, cursor) : cursor;
        if (b > a) {
            const float x0 = x + 4.0f + MeasureBody(text.substr(0, a));
            const float x1 = x + 4.0f + MeasureBody(text.substr(0, b));
            painter.rect(x0, y + 4.0f, std::max(1.0f, x1 - x0), kH - 8.0f,
                         WithAlpha(cfg::FILL, 120));
        }
        const float cx = x + 4.0f + MeasureBody(text.substr(0, cursor));
        painter.rect(cx, y + 6.0f, 1.0f, kH - 12.0f, cfg::TEXT);
    }
    return w;
}

bool TextEntry::press(float px, float py) {
    if (!hit(px, py)) return false;
    focus();
    cursor = index_at(px);
    sel_anchor = cursor;
    drag = true;
    update_cursor();
    return true;
}

void TextEntry::drag_to(float px, float py) {
    (void)py;
    if (!drag || !focused) return;
    cursor = index_at(px);
    update_cursor();
}

void TextEntry::release(float px, float py) {
    (void)px;
    (void)py;
    drag = false;
}

bool TextEntry::key(EditKey action) {
    if (!focused || !enabled) return false;
    switch (action) {
        case EditKey::kEnter:
            commit();
            return true;
        case EditKey::kEscape:
            cancel();
            return true;
        case EditKey::kBackspace:
            if (delete_selection()) {
                text_changed();
            } else if (cursor > 0) {
                text.erase(static_cast<size_t>(cursor - 1), 1);
                --cursor;
                text_changed();
            }
            update_cursor();
            return true;
        case EditKey::kDelete:
            if (delete_selection()) {
                text_changed();
            } else if (cursor < static_cast<int>(text.size())) {
                text.erase(static_cast<size_t>(cursor), 1);
                text_changed();
            }
            update_cursor();
            return true;
        case EditKey::kLeft:
            sel_anchor = -1;
            if (cursor > 0) --cursor;
            update_cursor();
            return true;
        case EditKey::kRight:
            sel_anchor = -1;
            if (cursor < static_cast<int>(text.size())) ++cursor;
            update_cursor();
            return true;
        case EditKey::kHome:
            sel_anchor = -1;
            cursor = 0;
            update_cursor();
            return true;
        case EditKey::kEnd:
            sel_anchor = -1;
            cursor = static_cast<int>(text.size());
            update_cursor();
            return true;
        case EditKey::kSelectAll:
            sel_anchor = 0;
            cursor = static_cast<int>(text.size());
            update_cursor();
            return true;
    }
    return false;
}

// ---- IntEntry ----
bool IntEntry::accept_char(uint32_t cp) const {
    return cp >= '0' && cp <= '9';
}

void IntEntry::text_changed() {
    if (text.empty() || !on_change) return;
    const int value_now = std::atoi(text.c_str());
    if (value_now >= min_value && value_now <= max_value)
        on_change(value_now);
}

void IntEntry::set_value(int value_now) {
    text = value_now != 0 ? std::to_string(value_now) : std::string();
    committed = text;
    cursor = static_cast<int>(text.size());
    sel_anchor = -1;
    update_cursor();
}

int IntEntry::value() const {
    if (text.empty()) return 0;
    return std::atoi(text.c_str());
}

}  // namespace bl::ui

