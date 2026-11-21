#include "render/assets.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>

#include "io/paths.h"
#include "render/cutout.h"
#include "render/imgtool.h"

namespace bl {
namespace {

// 缓存键：绝对路径（UTF-8）。同一张图从不同相对路径打开也只解码一次。
std::string PathKey(const std::string& path) {
    const std::filesystem::path full =
        std::filesystem::absolute(paths::Utf8Path(path));
    return paths::Utf8String(full.lexically_normal());
}

std::string BaseName(const std::string& path) {
    return paths::Utf8String(paths::Utf8Path(path).filename());
}

}  // namespace

int Asset::alpha_at(double px, double py) const {
    if (!(px >= 0.0 && px < static_cast<double>(width) && py >= 0.0 &&
          py < static_cast<double>(height))) {
        return 0;
    }
    int mx = static_cast<int>(px * mask_w / static_cast<double>(width));
    int my = static_cast<int>(py * mask_h / static_cast<double>(height));
    mx = std::min(mask_w - 1, std::max(0, mx));
    my = std::min(mask_h - 1, std::max(0, my));
    return mask[static_cast<size_t>(my) * mask_w + mx];
}

std::vector<unsigned char> Asset::rgba() const {
    if (!data.empty()) return data;
    return load_image_rgba(path).pixels;
}

bool AssetStore::contains(const std::string& path) const {
    return by_path_.find(PathKey(path)) != by_path_.end();
}

Asset& AssetStore::build(const std::string& path, const std::string& name,
                         std::vector<unsigned char> data, int width, int height,
                         bool keep) {
    owned_.push_back(std::make_unique<Asset>());
    Asset& asset = *owned_.back();
    asset.key = ++seq_;
    asset.path = path;
    asset.name = name;
    asset.width = width;
    asset.height = height;

    // 带透明区的位图先把 RGB 铺满（只改 RGB）：直通 alpha 采样时，透明处的
    // 黑色会被双线性混进边缘，导出后就是黑描边/黑方块。全不透明的图跳过。
    if (!data.empty()) {
        bool has_transparent = false;
        for (size_t i = 3; i < data.size(); i += 4) {
            if (data[i] != 255) {
                has_transparent = true;
                break;
            }
        }
        if (has_transparent) bleed_rgb(data, width, height);
    }

    asset.texture = std::make_unique<Texture>();
    asset.texture->create(width, height, 4, data.data(), true);

    const RgbaImage thumb = scaled_rgba(data, width, height, THUMB_SIDE);
    asset.thumb = thumb.pixels;
    asset.thumb_w = thumb.width;
    asset.thumb_h = thumb.height;

    const RgbaImage mask = scaled_rgba(data, width, height, MASK_SIDE);
    asset.mask_w = mask.width;
    asset.mask_h = mask.height;
    asset.mask.resize(static_cast<size_t>(mask.width) * mask.height);
    for (size_t i = 0; i < asset.mask.size(); ++i) {
        asset.mask[i] = mask.pixels[i * 4 + 3];
    }

    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    alpha_bounds(data.data(), width, height, &x0, &y0, &x1, &y1);
    asset.content_x = x0;
    asset.content_y = y0;
    asset.content_w = x1 - x0;
    asset.content_h = y1 - y0;

    if (keep) asset.data = std::move(data);
    by_key_[asset.key] = &asset;
    return asset;
}

Asset& AssetStore::load(const std::string& path) {
    const std::string key = PathKey(path);
    auto it = by_path_.find(key);
    if (it != by_path_.end()) return *it->second;

    const RgbaImage image = load_image_rgba(path);
    Asset& asset =
        build(key, BaseName(path), image.pixels, image.width, image.height, false);
    by_path_[key] = &asset;
    return asset;
}

Asset& AssetStore::put(const std::string& path, std::vector<unsigned char> data,
                       int width, int height) {
    Asset& asset =
        build(PathKey(path), BaseName(path), std::move(data), width, height, true);
    return asset;
}

Asset& AssetStore::get(int key) {
    auto it = by_key_.find(key);
    if (it == by_key_.end()) {
        throw std::runtime_error("资产键不存在: " + std::to_string(key));
    }
    return *it->second;
}

const Asset& AssetStore::get(int key) const {
    auto it = by_key_.find(key);
    if (it == by_key_.end()) {
        throw std::runtime_error("资产键不存在: " + std::to_string(key));
    }
    return *it->second;
}

}  // namespace bl
