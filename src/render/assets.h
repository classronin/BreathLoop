// 资产缓存：一张图片只解码/上传一次，图层与面板共享。
//
// Asset 持有画布纹理、缩略图字节（图层面板用）和降采样 alpha 掩码（画布点选）。
// 纹理顶向下，alpha 未预乘。
//
// 文件资产不留整幅像素（按需从路径重读）；抠图这类内存资产没有源文件，
// 自带像素，否则第二次抠图就没有数据来源了。
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "render/gfx.h"

namespace bl {

constexpr int THUMB_SIDE = 64;
constexpr int MASK_SIDE = 256;

struct Asset {
    int key = 0;
    std::string path;
    std::string name;
    int width = 0;
    int height = 0;
    int content_x = 0;  // 非透明内容在图片里的左上角与尺寸
    int content_y = 0;
    int content_w = 0;
    int content_h = 0;
    std::unique_ptr<Texture> texture;
    std::vector<unsigned char> thumb;  // RGBA，顶向下
    int thumb_w = 0;
    int thumb_h = 0;
    int mask_w = 0;
    int mask_h = 0;
    std::vector<unsigned char> mask;  // alpha 通道（MASK_SIDE 缩放后）
    std::vector<unsigned char> data;  // 内存资产的原始 RGBA；文件资产为空

    // 图层局部像素坐标处的 alpha（0-255），越界为 0。
    int alpha_at(double px, double py) const;

    // 整幅 RGBA（顶向下）：抠图读它，与画布纹理同源。
    std::vector<unsigned char> rgba() const;

    bool is_memory() const { return !data.empty(); }
};

class AssetStore {
public:
    AssetStore() = default;
    AssetStore(const AssetStore&) = delete;
    AssetStore& operator=(const AssetStore&) = delete;

    bool contains(const std::string& path) const;
    Asset& load(const std::string& path);
    // 内存资产：没有源文件，像素留在 Asset 上，重复抠图才有数据来源。
    Asset& put(const std::string& path, std::vector<unsigned char> data,
               int width, int height);
    Asset& get(int key);
    const Asset& get(int key) const;

private:
    Asset& build(const std::string& path, const std::string& name,
                 std::vector<unsigned char> data, int width, int height,
                 bool keep);

    std::map<std::string, Asset*> by_path_;
    std::map<int, Asset*> by_key_;
    std::vector<std::unique_ptr<Asset>> owned_;
    int seq_ = 0;
};

}  // namespace bl
