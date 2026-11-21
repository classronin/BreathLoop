// 画布底：画布外暗色，画布内 alpha 棋盘格。
//
// 棋盘格按屏幕像素算、锚在屏幕原点，不随缩放或平移变化（对齐
// E:\GitHub\C\ImageEditor\src\nodes\CanvasNode.cpp:243-280）；画布边界由 UI 层再补 1px 白线。
#pragma once

#include <string>

#include "render/gfx.h"

namespace bl {

class Backdrop {
public:
    static bool init(std::string* error);

    // canvas 四边用窗口坐标（左下原点、y 向上）。
    // 界外只是底色，不遮内容：越界像素仍然可见（同 ImageEditor 不做二次覆盖）。
    // frame_only = true 时只画 1px 边界（同源同一对坐标，画在内容之上）。
    static void draw(Renderer& renderer, int fb_w, int fb_h, double left,
                     double bottom, double right, double top,
                     bool frame_only = false);
};

}  // namespace bl
