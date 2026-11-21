// 套索：只有一份几何，遮罩由它派生，别处不再各自判定内外。
//
// 坐标住在挂载图层的图像像素系（原点左上，y 向下），与 Region 的 cx/cy 同一
// 口径：图层移动/缩放时自动跟随，跨图层换算必须经 Layer::map_point_from 中转。
// 与 ImageEditor 同构：圈内＝选区/抠图/呼吸范围，没有"反向"极性。
#pragma once

#include <vector>

namespace bl {

constexpr int kMinLassoPoints = 3;

struct Point {
    double x = 0.0;
    double y = 0.0;
};

struct Lasso {
    std::vector<Point> points;

    bool closed() const { return static_cast<int>(points.size()) >= kMinLassoPoints; }

    // 几何外接矩形 (x0, y0, x1, y1)；未闭合时为空。
    void bounds(double* x0, double* y0, double* x1, double* y1) const;

    // 遮罩重建用的比较值：几何变了才重画。
    bool key_equals(const Lasso& other) const;

    Lasso copy() const { return Lasso{points}; }
};

}  // namespace bl
