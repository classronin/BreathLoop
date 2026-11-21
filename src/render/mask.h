// 遮罩：套索几何的唯一光栅化器。
//
// 内外判定只在这里发生（奇偶扫描线填充），预览、导出、抠图都从这两个函数
// 派生：
//   sdf()      → 有符号距离场（R8，刻度 = 距离 + 127，单位纹理像素）；
//   coverage() → 原生分辨率的 0..255 覆盖率，抠图拿它乘 alpha。
//
// 分辨率由 detail（每图像像素取几个样）决定，受 kMaskMaxSide 上限约束：
// 预览取 config.MASK_PREVIEW_DETAIL，导出按渲染尺度取；两者调同一个函数、
// 同一份几何，只有采样密度不同。
//
// Python 版借了 OpenCV；这里自己写奇偶填充、超采样折回与精确欧氏距离变换，
// 不再为一张遮罩引入整套 cv2。
#pragma once

#include <memory>
#include <vector>

#include "domain/lasso.h"
#include "render/gfx.h"

namespace bl {

constexpr int kMaskMaxSide = 1024;  // 遮罩最长边上限：羽化与边缘精度到此足够
constexpr int kFieldBias = 127;     // R8 里"正好在边界"的刻度
constexpr double kEdgePadPx = 8.0;  // 光栅化四周多垫的图像像素
constexpr int kSupersample = 2;     // 折回前的超采样倍数
constexpr long kSupersampleBudget = 1200000;  // 超过这个纹素数就不再超采样

struct Mask {
    int width = 0;
    int height = 0;
    double scale = 1.0;              // 纹理像素 / 图像像素
    std::vector<unsigned char> data; // R8，顶向下

    // 图像像素表示的羽化半宽 → 纹理像素，着色器直接用。
    double feather(double image_px) const { return 0.5 * image_px * scale; }
};

// 遮罩尺寸与缩放：不超过上限，也不低于按 detail 放大的需求。
void mask_size(int img_w, int img_h, double detail, int* tw, int* th,
               double* scale);

// 原生分辨率的 0..255 覆盖率；越界顶点由填充器自己裁。
std::vector<unsigned char> coverage(const Lasso& lasso, int img_w, int img_h);

// 几何 → 有符号距离场：边界为 0，圈内为正，单位纹理像素。
Mask sdf(const Lasso& lasso, int img_w, int img_h, double detail = 1.0);

// 距离场 → R8 纹理。夹边不重复，线性过滤：羽化靠插值不靠重画。
std::unique_ptr<Texture> texture_for(const Mask& mask);

}  // namespace bl
