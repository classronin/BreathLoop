// 画布覆盖层：可见 BBOX 的椭圆 + 选中 BBOX 的手柄 + 套索。
//
// 手柄位置由 layer/region 的局部坐标推世界坐标，再统一走 Scene 的视图变换；
// 绘制时 y 轴翻转只在这里发生一次（世界 y 向下 → 窗口 y 向上）。
#pragma once

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace bl {

struct Region;
struct Layer;

namespace ui {

class Scene;
class Painter;

// 手柄在 region 局部坐标里的位置；旋转手柄固定离屏幕 20px（按缩放折算）
struct Handle {
    const char* name;
    double x;
    double y;
};
const std::array<Handle, 5>& HandleList(const Region& region,
                                        double up_scale);
// 区域局部 +y 方向 1 单位在屏幕上多少像素：旋转手柄的 20px 间隔按它折算，
// 这样旋转点在任何缩放/各向异性下都稳定离环 20px。
double RegionUpScale(const Scene& scene, const Layer& layer,
                     const Region& region);

void DrawOverlay(Scene& scene, Painter& painter);

}  // namespace ui
}  // namespace bl
