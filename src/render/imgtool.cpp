#include "render/imgtool.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

#include "vips/vips.h"

#include "io/blob.h"

namespace bl {
namespace {

struct VipsGuard {
    VipsImage* image = nullptr;
    ~VipsGuard() {
        if (image != nullptr) g_object_unref(image);
    }
    VipsImage* release() {
        VipsImage* out = image;
        image = nullptr;
        return out;
    }
};

void fail(const char* what) {
    throw std::runtime_error(std::string("图像解码失败: ") + what + ": " +
                             vips_error_buffer());
}

// 任意格式 → srgb + uchar + 至少 4 带（与 Python 的 _rgba() 同一串操作）。
// 文件内容由调用方先读进内存：libvips 在 Windows 上按 ANSI 解文件名，
// 中文路径会打不开，走 buffer 入口就不经过文件名了。
VipsImage* rgba_pipeline(const std::vector<unsigned char>& bytes) {
    VipsImage* raw = vips_image_new_from_buffer(
        bytes.data(), bytes.size(), "", "access", VIPS_ACCESS_RANDOM, nullptr);
    if (raw == nullptr) fail("打不开文件");
    VipsGuard guard{raw};

    VipsImage* srgb = nullptr;
    if (vips_colourspace(guard.image, &srgb, VIPS_INTERPRETATION_sRGB, nullptr)) {
        fail("colourspace srgb");
    }
    VipsGuard srgb_guard{srgb};

    if (srgb->BandFmt != VIPS_FORMAT_UCHAR) {
        VipsImage* cast = nullptr;
        if (vips_cast(srgb_guard.image, &cast, VIPS_FORMAT_UCHAR, nullptr)) {
            fail("cast uchar");
        }
        g_object_unref(srgb);
        srgb_guard.image = cast;
    }

    if (!vips_image_hasalpha(srgb_guard.image)) {
        VipsImage* joined = nullptr;
        if (vips_bandjoin_const1(srgb_guard.image, &joined, 255.0, nullptr)) {
            fail("bandjoin alpha");
        }
        g_object_unref(srgb_guard.image);
        srgb_guard.image = joined;
    }
    return srgb_guard.release();
}

std::vector<unsigned char> to_bytes(VipsImage* image, size_t* size) {
    void* memory = vips_image_write_to_memory(image, size);
    if (memory == nullptr) fail("write_to_memory");
    const size_t count = *size;
    std::vector<unsigned char> bytes(count);
    const unsigned char* source = static_cast<const unsigned char*>(memory);
    for (size_t i = 0; i < count; ++i) bytes[i] = source[i];
    g_free(memory);
    return bytes;
}

}  // namespace

RgbaImage load_image_rgba(const std::string& path) {
    std::vector<unsigned char> bytes;
    if (!io::ReadFileBytes(path, &bytes)) {
        throw std::runtime_error("图像解码失败: 文件不存在或读不了: " + path);
    }
    VipsImage* image = rgba_pipeline(bytes);
    VipsGuard guard{image};
    size_t size = 0;
    RgbaImage out;
    out.width = image->Xsize;
    out.height = image->Ysize;
    out.pixels = to_bytes(image, &size);
    return out;
}

RgbaImage load_image_rgba_memory(const unsigned char* data, size_t size) {
    if (data == nullptr || size == 0) {
        throw std::runtime_error("图像解码失败: 内存为空");
    }
    std::vector<unsigned char> bytes(data, data + size);
    VipsImage* image = rgba_pipeline(bytes);
    VipsGuard guard{image};
    size_t written = 0;
    RgbaImage out;
    out.width = image->Xsize;
    out.height = image->Ysize;
    out.pixels = to_bytes(image, &written);
    return out;
}

void alpha_bounds(const unsigned char* data, int width, int height, int* x0,
                  int* y0, int* x1, int* y1) {
    int left = width;
    int top = height;
    int right = -1;
    int bottom = -1;
    for (int y = 0; y < height; ++y) {
        const unsigned char* row = data + static_cast<size_t>(y) * width * 4;
        int first = -1;
        int last = -1;
        for (int x = 0; x < width; ++x) {
            if (row[x * 4 + 3] != 0) {
                if (first < 0) first = x;
                last = x;
            }
        }
        if (first < 0) continue;
        if (first < left) left = first;
        if (last > right) right = last;
        if (y < top) top = y;
        bottom = y;
    }
    if (right < 0) {
        *x0 = *y0 = *x1 = *y1 = 0;
        return;
    }
    *x0 = left;
    *y0 = top;
    *x1 = right + 1;
    *y1 = bottom + 1;
}

RgbaImage scaled_rgba(const std::vector<unsigned char>& data, int width,
                      int height, int max_side) {
    const double scale =
        std::min(1.0, static_cast<double>(max_side) / std::max(width, height));
    const int tw = std::max(1, static_cast<int>(std::lround(width * scale)));
    const int th = std::max(1, static_cast<int>(std::lround(height * scale)));

    VipsImage* source = vips_image_new_from_memory_copy(
        data.data(), data.size(), width, height, 4, VIPS_FORMAT_UCHAR);
    if (source == nullptr) fail("new_from_memory_copy");
    VipsGuard guard{source};

    RgbaImage out;
    out.width = tw;
    out.height = th;
    if (tw == width && th == height) {
        size_t size = 0;
        out.pixels = to_bytes(source, &size);
        return out;
    }
    VipsImage* scaled = nullptr;
    if (vips_thumbnail_image(guard.image, &scaled, tw, "height", th, "size",
                             VIPS_SIZE_DOWN, nullptr)) {
        fail("thumbnail_image");
    }
    VipsGuard scaled_guard{scaled};
    size_t size = 0;
    out.pixels = to_bytes(scaled, &size);
    return out;
}

}  // namespace bl
