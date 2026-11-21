// 自绘控件：Blender 风格扁平化。
//
// 控件只知道自己"在哪、多大、要不要响应"，绘制与命中都在这里；
// 面板只负责每帧摆位置、把回调接到 Scene。没有隐式布局，没有全局单例。
#pragma once

#include <functional>
#include <string>

#include "config.h"
#include "ui/painter.h"

namespace bl::ui {

// 控件命中判定的唯一口径：启用 + 没被浮动面板裁掉 + 落在自己的矩形内。
struct Widget {
    // 虚析构让 Widget 自身带 vfptr：派生类若另有虚函数，布局仍从 0 开始，
    // 面板用 void* 存指针再按派生类型取回才不会错位。
    virtual ~Widget() = default;

    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    bool enabled = true;
    bool clip_on = false;
    float clip[4] = {0.0f, 0.0f, 0.0f, 0.0f};   // x, y, x+w, y+h

    bool clipped(float px, float py) const;
    bool hit(float px, float py) const;
};

struct Slider : Widget {
    static constexpr float kH = 24.0f;

    std::string label;
    std::string text;         // 当前显示值
    float vmin = 0.0f;
    float vmax = 1.0f;
    float step = 0.0f;
    float value = 0.0f;
    std::string format = "{:.0f}";   // Python 风格占位：只支持 {:.Nf}
    bool drag = false;

    std::function<void(float)> on_change;
    std::function<void()> on_release;
    std::function<void()> on_start;

    void set_value(float v, bool notify = false);
    void draw(Painter& painter) const;
    bool press(float px, float py);
    void drag_to(float px, float py);
    void release(float px, float py);
    bool scroll(float px, float py, float dy);

private:
    void refresh();
    void from_x(float px);
    std::string format_value() const;
};

struct Toggle : Widget {
    static constexpr float kH = 24.0f;
    static constexpr float kS = 12.0f;

    std::string label;
    bool value = false;
    std::function<void(bool)> on_change;

    void set_label(const std::string& text) { label = text; }
    void draw(Painter& painter) const;
    bool press(float px, float py);
};

struct Button : Widget {
    static constexpr float kH = 28.0f;

    std::string label;
    cfg::Rgba color = cfg::BTN_GRAY;
    cfg::Rgba color_hover = cfg::BTN_GRAY_HOVER;
    bool hover = false;
    std::function<void()> on_press;

    void draw(Painter& painter) const;
    bool press(float px, float py);
    void motion(float px, float py);
};

struct TinyButton : Widget {
    static constexpr float kS = 20.0f;

    std::string label;
    cfg::Rgba color = cfg::BTN_GRAY;
    std::function<void()> on_press;

    void set_label(const std::string& text) { label = text; }
    void draw(Painter& painter) const;
    bool press(float px, float py);
};

struct HeaderToggle : Widget {
    static constexpr float kS = 20.0f;

    std::string label;
    cfg::Rgba color = cfg::BTN_SUBTLE;
    std::function<void()> on_press;

    void set_label(const std::string& text) { label = text; }
    void draw(Painter& painter) const;
    bool press(float px, float py);
};

enum class EditKey {
    kEnter, kEscape, kBackspace, kDelete, kLeft, kRight, kHome, kEnd,
    kSelectAll,
};

struct TextEntry : Widget {
    static constexpr float kH = 24.0f;

    std::string text;
    std::string committed;
    int cursor = 0;
    int sel_anchor = -1;
    bool drag = false;
    bool focused = false;
    int max_len = 64;
    std::function<void(const std::string&)> on_commit;

    float draw(Painter& painter) const;
    bool press(float px, float py);
    void drag_to(float px, float py);
    void release(float px, float py);
    bool key(EditKey action);
    void insert_text(const std::string& chunk);
    void focus();
    void unfocus();
    void commit();
    void cancel();
    void set_text(const std::string& value);

protected:
    virtual bool accept_char(uint32_t cp) const;
    virtual void text_changed() {}

    void measure_prefixes();
    void update_cursor();
    int index_at(float px) const;
    bool delete_selection();
};

struct IntEntry : TextEntry {
    int min_value = 1;
    int max_value = 100000;
    std::function<void(int)> on_change;

    void set_value(int value);
    int value() const;

protected:
    bool accept_char(uint32_t cp) const override;
    void text_changed() override;
};

}  // namespace bl::ui
