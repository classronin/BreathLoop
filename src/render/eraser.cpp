#include "render/eraser.h"

#include <algorithm>
#include <cmath>

namespace bl {
namespace {

int ClampInt(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace

bool stamp_coverage(std::vector<unsigned char>& cov, int w, int h, double cx,
                    double cy, double radius, double feather, int* x0, int* y0,
                    int* x1, int* y1) {
    if (w <= 0 || h <= 0 || cov.size() < static_cast<size_t>(w) * h) {
        return false;
    }
    if (!(radius > 0.0)) return false;
    // 硬边 = 羽化 0；羽化不允许超过半径（超过就没有实心区了）
    const double soft = std::min(std::max(feather, 0.0), radius);
    const int left = ClampInt(static_cast<int>(std::floor(cx - radius)), 0, w);
    const int right = ClampInt(static_cast<int>(std::ceil(cx + radius)), 0, w);
    const int top = ClampInt(static_cast<int>(std::floor(cy - radius)), 0, h);
    const int bottom = ClampInt(static_cast<int>(std::ceil(cy + radius)), 0, h);
    if (right <= left || bottom <= top) return false;

    *x0 = left;
    *y0 = top;
    *x1 = right;
    *y1 = bottom;

    const double r2 = radius * radius;
    bool grew = false;
    for (int y = top; y < bottom; ++y) {
        const double dy = static_cast<double>(y) + 0.5 - cy;
        const size_t row = static_cast<size_t>(y) * w;
        for (int x = left; x < right; ++x) {
            const double dx = static_cast<double>(x) + 0.5 - cx;
            const double d2 = dx * dx + dy * dy;
            if (d2 > r2) continue;
            // 实心区覆盖率 1，羽化带按距离线性降到 0
            double c = 1.0;
            if (soft > 0.0) {
                c = (radius - std::sqrt(d2)) / soft;
                c = c < 0.0 ? 0.0 : (c > 1.0 ? 1.0 : c);
            }
            const unsigned char value =
                static_cast<unsigned char>(c * 255.0 + 0.5);
            unsigned char& slot = cov[row + static_cast<size_t>(x)];
            // 取最大值：同一笔里重叠多少次都不会侵蚀已经画好的软边
            if (value > slot) {
                slot = value;
                grew = true;
            }
        }
    }
    return grew;
}

}  // namespace bl
