// 图层视图：把 Document 的每张图变成可绘制的节点（世界坐标，扁平列表）。
//
// 不套父节点变换：根图层的"牵动子树"已在 domain/layers 里烘焙到各图层的
// x/y/scale，这里只负责逐层同步。绘制顺序 = 文档 flatten 顺序，后加者盖在上面。
//
// 套索不再参与呼吸变形（它只用来裁剪/复制），所以这里不生成遮罩纹理。
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "domain/layers.h"
#include "render/assets.h"
#include "render/shaders.h"

namespace bl {

class LayerView {
public:
    LayerView(Layer* layer, Asset* asset);

    Layer* layer = nullptr;
    Asset* asset = nullptr;

    // 节点状态（与 passer 的 Image 属性同义）
    double x = 0.0;
    double y = 0.0;
    double scale = 1.0;    // 等比部分
    double scale_x = 1.0;  // 实际横向倍率（scale * sx）
    double scale_y = 1.0;
    double rot = 0.0;      // 绕中心的旋转（弧度）
    bool visible = true;

    BreathStyle style;

    void sync(bool deform = true);
    void release();

    // 世界坐标下的四角（左下/右上），供绘制与点选共用。
    void world_quad(double* left, double* bottom, double* right, double* top) const;
    // 旋转后的真实四角：p[0..7] = 左上 / 右上 / 右下 / 左下
    void world_corners(double* p) const;
};

// 一个画布下的全部图层视图，按 uid 查询。
class LayerViews {
public:
    void rebuild(const Document& doc, AssetStore& assets);
    LayerView* get(int uid);
    void sync(bool deform = true);
    void release_all();

    // 底 → 顶的绘制顺序（= 文档 flatten 顺序）
    const std::vector<std::unique_ptr<LayerView>>& views() const { return views_; }
    // 从上到下的点击判定顺序
    std::vector<LayerView*> top_down() const;
    bool empty() const { return views_.empty(); }

private:
    std::vector<std::unique_ptr<LayerView>> views_;
    std::map<int, LayerView*> by_uid_;
};

}  // namespace bl
