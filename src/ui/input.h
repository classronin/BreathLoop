// 画布输入：视图平移缩放、图层选择移动、BBOX 手柄编辑。
//
// 坐标只用两套：窗口坐标（y 向上）与世界坐标（y 向下）。
// 换算统一走 Scene 的方法，本模块不自行推导视图矩阵。
#pragma once

#include <string>
#include <utility>

namespace bl {

struct Layer;
struct Region;

namespace ui {

class Scene;

namespace input {

// 画布可用范围：只被停靠面板挤压，浮动面板浮在画布之上。
std::pair<double, double> CanvasEdges(const Scene& scene);
bool InCanvas(const Scene& scene, double x);

// 命中（画布点选与覆盖层共用）
Layer* LayerAt(Scene& scene, double wx, double wy);
bool BboxAt(Scene& scene, double wx, double wy, Layer** layer, Region** region);
std::string HandleAt(Scene& scene, double wx, double wy);   // "" = 没有
// Ctrl+T 变形态下的悬停命中："tl"/"tr"/"br"/"bl" 拖角缩放、"rot" 旋转、
// "move" 图内拖动、"" 无。光标由 Scene::update_cursor 据此选形状。
// 拖角缩放的主轴在按下时定死：按住 ALT = 对角不动，否则 = 图像中心不动。
std::string TransformHandleAt(Scene& scene, double wx, double wy);

void HandleKey(Scene& scene, int key, int mods);
void HandleKeyRelease(Scene& scene, int key, int mods);
void HandlePress(Scene& scene, double x, double y, int button, int mods);
void HandleDrag(Scene& scene, double x, double y, double dx, double dy,
                int mods);
void HandleRelease(Scene& scene, double x, double y, int button, int mods);
bool HandleScroll(Scene& scene, double x, double y, double dy);

}  // namespace input
}  // namespace ui
}  // namespace bl
