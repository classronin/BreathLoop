// 工具小板：画布底部居中的一条浮动工具条（一行两块：半径 / 羽化，各 300px）。
//
// 它是画布之上一层独立的东西，不归参数面板管：显示条件就是 eraser_on，
// 鼠标归属在按下那一刻决定（Scene 的 board_capture_）——在板上按下就是板子的，
// 拖进画布也不擦；在画布上落笔就是画布的，划进板子也不中断。
#pragma once

#include "ui/widgets.h"

namespace bl::ui {
class Scene;
class Painter;

class ToolBoard {
public:
    void init(Scene* scene);

    bool visible() const;
    // 命中并开始拖动；返回 true 表示这一次按下归小板（调用方要记住 owner）
    bool press(float x, float y);
    void drag(float x, float y);
    void release(float x, float y);
    void draw(Painter& painter);

private:
    void layout();
    bool contains(float x, float y) const;

    Scene* scene_ = nullptr;
    Slider radius_;    // 界面上是半径（1–250），写回的是直径（×2）
    Slider feather_;
    float x_ = 0.0f;
    float y_ = 0.0f;
    float w_ = 0.0f;
    float h_ = 0.0f;
};

}  // namespace bl::ui
