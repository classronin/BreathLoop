// 呼吸变形着色器：按目标图层投影后的 BBOX 累积位移。
//
// 一个 BreathStyle 服务一个图层；views 由 Layer::bbox_views() 提供，位移单位
// 与像素坐标一致。套索不由着色器解释多边形：内外判定来自 mask 光栅化好的
// 距离场纹理（u_mask，边界为 0，圈内为正），这里只按 u_feather 做过渡。
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "domain/layers.h"
#include "render/gfx.h"

namespace bl {

// 泊松补偿系数：让像素看起来在滑动，而不是被均匀拉伸
inline constexpr double kPhysPoisson = 0.35;
// 遮罩纹理占用的采样单元；单元 0 是图层纹理的位置
inline constexpr int kMaskUnit = 1;
inline constexpr int kLayerUnit = 0;

class BreathStyle {
public:
    std::vector<BBoxView> views;
    double img_w = 1.0;
    double img_h = 1.0;
    std::unique_ptr<Texture> mask_texture;  // 空 = 没有套索，处处可呼吸
    float feather = 0.0f;                   // 过渡半宽，遮罩纹理像素
    bool deform = true;

    // 全部图层共用一个程序：uniform 逐次写入，绘制本来就一次一层。
    static Program& program();
    static bool init_program(std::string* error);

    // 写入 uniform 并绑定遮罩纹理（单元 1）。调用前先 program.use()。
    void bind(Program& program, const Texture& inside_all) const;
};

// 没有套索时的"处处都在圈内"：1×1 的 R8，距离取满格。
const Texture& inside_all_mask();

}  // namespace bl
