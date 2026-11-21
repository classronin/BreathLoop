#include "render/mask.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace bl {
namespace {

constexpr double kFarDistance = 1e12;

int SupersampleFor(int tw, int th) {
    return (static_cast<long>(tw) * th <= kSupersampleBudget) ? kSupersample : 1;
}

// 顶点 → 超采样整数网格（1/16 像素，等价于 cv2 的 shift=4）
struct FixedVertex {
    double x = 0.0;
    double y = 0.0;
};

std::vector<FixedVertex> Quantize(const std::vector<Point>& points, double scale,
                                  int pad, int supersample) {
    // 顶点先放缩到超采样空间，再量化到该空间的 1/16 纹素（等价 cv2 的 shift=4）。
    const double grid = static_cast<double>(supersample) * 16.0;
    std::vector<FixedVertex> out;
    out.reserve(points.size());
    for (const Point& p : points) {
        out.push_back(FixedVertex{
            std::rint((p.x * scale + pad) * grid) / 16.0,
            std::rint((p.y * scale + pad) * grid) / 16.0});
    }
    return out;
}

// 奇偶射线填充（扫描线取整数行、端点四舍五入到像素并含端点，与 cv2 的
// fillPoly 同一套取整：像素 (x, y) 的坐标就是它自己的索引，不是中心）。
void FillPolygon(std::vector<unsigned char>& out, int sw, int sh,
                 const std::vector<FixedVertex>& verts) {
    const size_t edges = verts.size();
    std::vector<double> crossings;
    crossings.reserve(edges);

    for (int y = 0; y < sh; ++y) {
        const double sy = static_cast<double>(y);
        crossings.clear();
        for (size_t i = 0; i < edges; ++i) {
            const FixedVertex& a = verts[i];
            const FixedVertex& b = verts[(i + 1) % edges];
            if (a.y == b.y) continue;
            const double lo = std::min(a.y, b.y);
            const double hi = std::max(a.y, b.y);
            // 半开区间 [lo, hi)：共享顶点只算一次，奇偶才对
            if (sy < lo || sy >= hi) continue;
            crossings.push_back(a.x + (sy - a.y) * (b.x - a.x) / (b.y - a.y));
        }
        if (crossings.size() < 2) continue;
        std::sort(crossings.begin(), crossings.end());

        unsigned char* row = out.data() + static_cast<size_t>(y) * sw;
        for (size_t i = 0; i + 1 < crossings.size(); i += 2) {
            // 端点各自四舍五入后含端点（floor(v + 0.5)，与 cv2 的定点 >>4 同值）
            int first = static_cast<int>(std::floor(crossings[i] + 0.5));
            int last = static_cast<int>(std::floor(crossings[i + 1] + 0.5));
            first = std::max(0, first);
            last = std::min(sw - 1, last);
            for (int x = first; x <= last; ++x) row[x] = 255;
        }
    }
}

// 超采样缓冲 → 覆盖率（盒式降采样，等价 INTER_AREA 的整数倍折回）
void Downsample(const std::vector<unsigned char>& src, int sw, int sh,
                int supersample, std::vector<unsigned char>& dst, int w, int h) {
    dst.assign(static_cast<size_t>(w) * h, 0);
    if (supersample == 1) {
        dst = src;
        return;
    }
    const int area = supersample * supersample;
    const int half = area / 2;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int sum = 0;
            for (int dy = 0; dy < supersample; ++dy) {
                const unsigned char* row =
                    src.data() + static_cast<size_t>(y * supersample + dy) * sw;
                for (int dx = 0; dx < supersample; ++dx) {
                    sum += row[x * supersample + dx];
                }
            }
            sum += half;
            dst[static_cast<size_t>(y) * w + x] =
                static_cast<unsigned char>(sum / area);
        }
    }
}

// 几何 → 覆盖率数组（th + 2*pad, tw + 2*pad），奇偶规则；超采样后折回。
void FillCoverage(std::vector<unsigned char>& out, int tw, int th, double scale,
                  int supersample, int pad, const std::vector<Point>& points) {
    const int w = tw + 2 * pad;
    const int h = th + 2 * pad;
    const int sw = w * supersample;
    const int sh = h * supersample;

    std::vector<unsigned char> big(static_cast<size_t>(sw) * sh, 0);
    FillPolygon(big, sw, sh, Quantize(points, scale, pad, supersample));
    Downsample(big, sw, sh, supersample, out, w, h);
}

// 遮罩和图像纹理一样是自上而下的行序，而本工程的几何是 y 向上：整块按行镜像一次，
// 栅格化本身保持与 Python 逐字节一致（同一份输入同一种算法），只在交付纹理时翻。
void MirrorRows(std::vector<unsigned char>& data, int w, int h) {
    if (w <= 0 || h <= 0) return;
    std::vector<unsigned char> tmp(static_cast<size_t>(w));
    for (int y = 0; y < h / 2; ++y) {
        unsigned char* a = data.data() + static_cast<size_t>(y) * w;
        unsigned char* b = data.data() + static_cast<size_t>(h - 1 - y) * w;
        std::copy(a, a + w, tmp.begin());
        std::copy(b, b + w, a);
        std::copy(tmp.begin(), tmp.end(), b);
    }
}

// ---- 精确欧氏距离变换（Felzenszwalb & Huttenlocher）----

void DistanceTransform1D(const std::vector<double>& f, std::vector<double>& d,
                         int n, std::vector<int>& v, std::vector<double>& z) {
    if (n <= 0) return;
    auto intersect = [&f](int q, int p) {
        return ((f[q] + static_cast<double>(q) * q) -
                (f[p] + static_cast<double>(p) * p)) /
               (2.0 * q - 2.0 * p);
    };
    int k = 0;
    v[0] = 0;
    z[0] = -kFarDistance;
    z[1] = kFarDistance;
    for (int q = 1; q < n; ++q) {
        double s = intersect(q, v[k]);
        while (k > 0 && s <= z[k]) {
            --k;
            s = intersect(q, v[k]);
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = kFarDistance;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < static_cast<double>(q)) ++k;
        const double diff = static_cast<double>(q - v[k]);
        d[q] = diff * diff + f[v[k]];
    }
}

// 每个非零像素到最近零像素的欧氏距离（零像素处为 0）。
std::vector<double> DistanceToZero(const std::vector<unsigned char>& source,
                                   int width, int height) {
    const size_t count = static_cast<size_t>(width) * height;
    std::vector<double> grid(count, 0.0);
    for (size_t i = 0; i < count; ++i) {
        grid[i] = source[i] == 0 ? 0.0 : kFarDistance;
    }

    const int longest = std::max(width, height);
    std::vector<double> line(static_cast<size_t>(longest), 0.0);
    std::vector<double> result(static_cast<size_t>(longest), 0.0);
    std::vector<int> v(static_cast<size_t>(longest) + 1, 0);
    std::vector<double> z(static_cast<size_t>(longest) + 2, 0.0);

    for (int x = 0; x < width; ++x) {
        for (int y = 0; y < height; ++y) {
            line[static_cast<size_t>(y)] =
                grid[static_cast<size_t>(y) * width + x];
        }
        DistanceTransform1D(line, result, height, v, z);
        for (int y = 0; y < height; ++y) {
            grid[static_cast<size_t>(y) * width + x] =
                result[static_cast<size_t>(y)];
        }
    }
    for (int y = 0; y < height; ++y) {
        double* row = grid.data() + static_cast<size_t>(y) * width;
        for (int x = 0; x < width; ++x) line[static_cast<size_t>(x)] = row[x];
        DistanceTransform1D(line, result, width, v, z);
        for (int x = 0; x < width; ++x) row[x] = result[static_cast<size_t>(x)];
    }

    for (size_t i = 0; i < count; ++i) grid[i] = std::sqrt(grid[i]);
    return grid;
}

}  // namespace

void mask_size(int img_w, int img_h, double detail, int* tw, int* th,
               double* scale) {
    const double limit = static_cast<double>(kMaskMaxSide) /
                         static_cast<double>(std::max(1, std::max(img_w, img_h)));
    const double value = std::min(detail, limit);
    *tw = std::max(1, static_cast<int>(std::rint(img_w * value)));
    *th = std::max(1, static_cast<int>(std::rint(img_h * value)));
    *scale = value;
}

std::vector<unsigned char> coverage(const Lasso& lasso, int img_w, int img_h) {
    if (!lasso.closed()) {
        return std::vector<unsigned char>(
            static_cast<size_t>(std::max(0, img_w)) * std::max(0, img_h), 0);
    }
    std::vector<unsigned char> out;
    FillCoverage(out, img_w, img_h, 1.0, SupersampleFor(img_w, img_h), 0,
                 lasso.points);
    MirrorRows(out, img_w, img_h);
    return out;
}

Mask sdf(const Lasso& lasso, int img_w, int img_h, double detail) {
    Mask mask;
    double scale = 1.0;
    mask_size(img_w, img_h, detail, &mask.width, &mask.height, &scale);
    mask.scale = scale;
    const size_t count =
        static_cast<size_t>(mask.width) * static_cast<size_t>(mask.height);

    if (!lasso.closed()) {
        mask.data.assign(count, 255);
        return mask;
    }

    const int pad = std::max(2, static_cast<int>(std::ceil(kEdgePadPx * scale)));
    const int w = mask.width + 2 * pad;
    const int h = mask.height + 2 * pad;

    std::vector<unsigned char> cov;
    FillCoverage(cov, mask.width, mask.height, scale,
                 SupersampleFor(mask.width, mask.height), pad, lasso.points);

    std::vector<unsigned char> hard(static_cast<size_t>(w) * h, 0);
    for (size_t i = 0; i < hard.size(); ++i) hard[i] = cov[i] >= 128 ? 255 : 0;

    std::vector<unsigned char> inverse(hard.size(), 0);
    for (size_t i = 0; i < hard.size(); ++i) inverse[i] = 255 - hard[i];

    const std::vector<double> inside = DistanceToZero(hard, w, h);
    const std::vector<double> outside = DistanceToZero(inverse, w, h);

    // 距离量到的是"对面那一类最近的纹素中心"，比真实边界远半个纹素：扣掉这半格。
    mask.data.assign(count, 0);
    for (int y = 0; y < mask.height; ++y) {
        for (int x = 0; x < mask.width; ++x) {
            const size_t sidx = static_cast<size_t>(y + pad) * w + (x + pad);
            const double value = hard[sidx] > 0 ? inside[sidx] - 0.5
                                                : -(outside[sidx] - 0.5);
            // 四舍五入而不是截断：截断会让整场朝"圈外"偏半个纹素。
            const double field = std::rint(value) + kFieldBias;
            const double clipped = std::min(255.0, std::max(0.0, field));
            mask.data[static_cast<size_t>(y) * mask.width + x] =
                static_cast<unsigned char>(clipped);
        }
    }
    MirrorRows(mask.data, mask.width, mask.height);
    return mask;
}

std::unique_ptr<Texture> texture_for(const Mask& mask) {
    std::unique_ptr<Texture> texture(new Texture());
    texture->create(mask.width, mask.height, 1, mask.data.data());
    return texture;
}

}  // namespace bl
