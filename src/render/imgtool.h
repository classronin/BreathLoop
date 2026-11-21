// 图像解码：任意格式 → 顶向下的 RGBA 字节（与纹理行序同口径）。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace bl {

struct RgbaImage {
    std::vector<unsigned char> pixels;
    int width = 0;
    int height = 0;
};

// 解码失败抛 std::runtime_error（带 libvips 的错误文本）。
RgbaImage load_image_rgba(const std::string& path);
// 同一套解码，只是数据已经在内存里（exe 内嵌资源就用这个入口）。
RgbaImage load_image_rgba_memory(const unsigned char* data, size_t size);

// 非透明像素的外接矩形 (x0, y0, x1, y1)，全透明返回 0×0。
void alpha_bounds(const unsigned char* data, int width, int height, int* x0,
                  int* y0, int* x1, int* y1);

// 按最长边缩到 max_side 以内，返回顶向下 RGBA（缩略图/掩码共用）。
RgbaImage scaled_rgba(const std::vector<unsigned char>& data, int width,
                      int height, int max_side);

}  // namespace bl
