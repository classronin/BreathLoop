#include "render/cutout.h"

#include <algorithm>
#include <cmath>
#include <queue>

#include "config.h"
#include "render/mask.h"

namespace bl {
namespace {

// 把颜色向 alpha=0 的像素扩散（只动 RGB，不动 alpha）。
// 抠出来的小块四周是补边的透明黑；缩放/旋转后双线性采样会把这片黑混进边缘，
// 表现为一圈灰黑描边。先把颜色铺满整块（透明处的 RGB 不再参与合成，但参与
// 采样），采样到的颜色就和内部一致，alpha 仍然只由覆盖率决定。
void BleedRgb(std::vector<unsigned char>& px, int w, int h) {
    if (w <= 0 || h <= 0) return;
    const size_t count = static_cast<size_t>(w) * h;
    std::vector<unsigned char> known(count, 0);
    std::queue<size_t> pending;
    for (size_t i = 0; i < count; ++i) {
        if (px[i * 4 + 3] != 0) {
            known[i] = 1;
            pending.push(i);
        }
    }
    const int nx[4] = {1, -1, 0, 0};
    const int ny[4] = {0, 0, 1, -1};
    while (!pending.empty()) {
        const size_t i = pending.front();
        pending.pop();
        const int x = static_cast<int>(i % static_cast<size_t>(w));
        const int y = static_cast<int>(i / static_cast<size_t>(w));
        for (int k = 0; k < 4; ++k) {
            const int px_x = x + nx[k];
            const int px_y = y + ny[k];
            if (px_x < 0 || px_y < 0 || px_x >= w || px_y >= h) continue;
            const size_t n = static_cast<size_t>(px_y) * w + px_x;
            if (known[n] != 0) continue;
            int sum[3] = {0, 0, 0};
            int hits = 0;
            for (int m = 0; m < 4; ++m) {
                const int qx = px_x + nx[m];
                const int qy = px_y + ny[m];
                if (qx < 0 || qy < 0 || qx >= w || qy >= h) continue;
                const size_t q = static_cast<size_t>(qy) * w + qx;
                if (known[q] == 0) continue;
                sum[0] += px[q * 4 + 0];
                sum[1] += px[q * 4 + 1];
                sum[2] += px[q * 4 + 2];
                ++hits;
            }
            if (hits == 0) continue;  // 邻居还没颜色，交给后面的环
            px[n * 4 + 0] = static_cast<unsigned char>(sum[0] / hits);
            px[n * 4 + 1] = static_cast<unsigned char>(sum[1] / hits);
            px[n * 4 + 2] = static_cast<unsigned char>(sum[2] / hits);
            known[n] = 1;
            pending.push(n);
        }
    }
}

}  // namespace

void bleed_rgb(std::vector<unsigned char>& rgba, int w, int h) {
    BleedRgb(rgba, w, h);
}

void crop_bounds(const Lasso& lasso, int img_w, int img_h, int* left, int* top,
                 int* right, int* bottom) {
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
    lasso.bounds(&x0, &y0, &x1, &y1);
    // 套索的 y 是图层局部坐标（向上为正），像素行是自上而下：翻一下再取整。
    const double top_row = static_cast<double>(img_h) - y1;
    const double bottom_row = static_cast<double>(img_h) - y0;
    *left = std::max(0, static_cast<int>(std::floor(x0)));
    *top = std::max(0, static_cast<int>(std::floor(top_row)));
    *right = std::min(img_w, static_cast<int>(std::ceil(x1)));
    *bottom = std::min(img_h, static_cast<int>(std::ceil(bottom_row)));
    if (*right <= *left || *bottom <= *top) {
        *left = *top = *right = *bottom = 0;
    }
}

std::optional<CropResult> cut_out(const std::vector<unsigned char>& rgba,
                                  int img_w, int img_h, const Lasso& lasso) {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    crop_bounds(lasso, img_w, img_h, &left, &top, &right, &bottom);
    if (right <= left) return std::nullopt;

    const std::vector<unsigned char> cov = coverage(lasso, img_w, img_h);
    std::vector<unsigned char> pixels = rgba;
    for (size_t i = 0; i < cov.size(); ++i) {
        // 覆盖率直接乘进 alpha（超采样后的 0..255），边缘是渐变的：
        // 合成路径已经按预乘处理（着色器出直通、混合乘一次 alpha、导出再
        // unpremultiply），渐变边不会被再乘一遍 alpha，所以既不会出暗边，
        // 也不会像硬阈值那样留下台阶状的方形锯齿。
        unsigned char* alpha = &pixels[i * 4 + 3];
        *alpha = static_cast<unsigned char>(
            (static_cast<int>(*alpha) * static_cast<int>(cov[i]) + 127) / 255);
    }

    const int pad = std::max(0, static_cast<int>(std::rint(cfg::CUTOUT_PAD)));
    CropResult result;
    result.width = (right - left) + 2 * pad;
    result.height = (bottom - top) + 2 * pad;
    result.pixels.assign(static_cast<size_t>(result.width) * result.height * 4,
                         0);
    // 两侧等量：新位图中心还是选区中心，粘贴落点不必再补偿
    for (int y = 0; y < bottom - top; ++y) {
        const unsigned char* src =
            pixels.data() +
            (static_cast<size_t>(top + y) * img_w + left) * 4;
        unsigned char* dst =
            result.pixels.data() +
            (static_cast<size_t>(y + pad) * result.width + pad) * 4;
        for (int i = 0; i < (right - left) * 4; ++i) dst[i] = src[i];
    }
    BleedRgb(result.pixels, result.width, result.height);
    return result;
}

std::vector<unsigned char> erase_inside(const std::vector<unsigned char>& rgba,
                                        int img_w, int img_h,
                                        const Lasso& lasso) {
    const std::vector<unsigned char> cov = coverage(lasso, img_w, img_h);
    std::vector<unsigned char> pixels = rgba;
    for (size_t i = 0; i < cov.size(); ++i) {
        // 与 cut_out 同一口径：覆盖率按渐变扣，边上是平滑过渡而不是一刀切。
        unsigned char* alpha = &pixels[i * 4 + 3];
        *alpha = static_cast<unsigned char>(
            (static_cast<int>(*alpha) * (255 - static_cast<int>(cov[i])) + 127) /
            255);
    }
    return pixels;
}

}  // namespace bl
