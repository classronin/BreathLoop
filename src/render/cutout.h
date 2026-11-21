// 抠图：把套索覆盖率乘到 alpha 上，再裁到外接框并补一圈透明边。
//
// 判定仍然只有 mask.cpp 一份：这里读的是 coverage()，与预览遮罩同一函数、
// 同一几何，只是取原生分辨率的逐像素覆盖率而不是距离场。
// erase_inside 是同一条覆盖率的反面用法：不裁框，只把线内的 alpha 扣掉。
//
// 补边（config.CUTOUT_PAD）是给形变留余量：着色器按图层位图采样，越界即透明。
// 紧贴选区裁出来的小块，内容一往外走就顶到自己的位图边界，看着像被边啃掉；
// 补一圈透明边后它有地方可走。顺带把 BBOX 半轴上限一起抬高。
#pragma once

#include <optional>
#include <vector>

#include "domain/lasso.h"

namespace bl {

struct CropResult {
    std::vector<unsigned char> pixels;  // RGBA，顶向下
    int width = 0;
    int height = 0;
};

// 套索外接框取整并裁进图像；与图像无交集（含未闭合）时返回空框（right<=left）。
void crop_bounds(const Lasso& lasso, int img_w, int img_h, int* left, int* top,
                 int* right, int* bottom);

// 选区内的像素（四周含补边）→ (RGBA, 宽, 高)；选区落在图像外返回 nullopt。
// alpha 按 coverage() 的 0..255 覆盖率渐变乘出来（超采样后折回），边缘平滑。
std::optional<CropResult> cut_out(const std::vector<unsigned char>& rgba,
                                  int img_w, int img_h, const Lasso& lasso);

// 剪掉线内：alpha 按同一份覆盖率扣掉，线外原样，尺寸不变。
std::vector<unsigned char> erase_inside(const std::vector<unsigned char>& rgba,
                                        int img_w, int img_h,
                                        const Lasso& lasso);

// 把 RGB 向 alpha=0 的像素扩散（只动 RGB，不动 alpha）。
// 带透明区的位图进纹理前必须过一遍：着色器按直通 alpha 采样，双线性会把
// 透明处的黑色混进边缘，再被 unpremultiply 放大，导出结果就是一圈黑描边。
// 预先铺满 RGB 后，透明处的颜色与内部一致，alpha 仍只由覆盖率决定。
void bleed_rgb(std::vector<unsigned char>& rgba, int w, int h);

}  // namespace bl
