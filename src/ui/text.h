// 文字：FreeType 栅格化 + 自建字形图集 + 度量。
//
// 全应用只有这一份字形来源。界面按"像素字号"调用，point→px 的换算（96 DPI）
// 只在这里发生一次；控件与面板都用 measure() 拿宽度，不自己估。
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "render/gl.h"

namespace bl::ui {

struct Glyph {
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;   // 图集 UV（原点左上）
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // 相对笔位、基线以上为正
    float advance = 0;
    bool empty = true;
};

// 96 DPI 下的 point→pixel 换算：4/3。界面上所有字号都写成 point。
int PointToPx(int point_size);

// UTF-8 单码点解码：传进字节下标，返回码点并把下标推到下一个字符。
uint32_t DecodeUtf8(const std::string& text, size_t* index);

class Text {
public:
    static Text& Instance();

    bool init(std::string* error);
    void shutdown();

    GLuint atlas() const { return atlas_id_; }
    int atlas_size() const { return atlas_size_; }

    const Glyph& glyph(uint32_t codepoint, int px, bool bold);
    // 整行宽度（不含行尾多余空白）
    float measure(const std::string& text, int px, bool bold);
    // 基线以上高度（用于把"文字中心"换算成基线）
    float ascent(int px, bool bold);
    float line_height(int px, bool bold);

private:
    Text() = default;
    Text(const Text&) = delete;
    Text& operator=(const Text&) = delete;

    struct FaceKey {
        int px;
        bool bold;
        bool operator==(const FaceKey& other) const {
            return px == other.px && bold == other.bold;
        }
    };
    struct FaceKeyHash {
        size_t operator()(const FaceKey& key) const {
            return static_cast<size_t>(key.px) * 131u + (key.bold ? 7u : 0u);
        }
    };
    struct Key {
        uint32_t codepoint;
        int px;
        bool bold;
        bool operator==(const Key& other) const {
            return codepoint == other.codepoint && px == other.px
                   && bold == other.bold;
        }
    };
    struct KeyHash {
        size_t operator()(const Key& key) const {
            return (static_cast<size_t>(key.codepoint) * 1000003u
                    + static_cast<size_t>(key.px)) * 3u + (key.bold ? 1u : 0u);
        }
    };

    struct Face;
    Face* face_for(int px, bool bold);
    // 同一个 FT_Face 会被多个字号轮流借用，取用前必须确认像素尺寸正好是 px。
    void ensure_px(Face* face, int px);
    const Glyph& raster(uint32_t codepoint, int px, bool bold);
    bool upload_region(int x, int y, int w, int h, const unsigned char* data);

    void* library_ = nullptr;
    std::vector<Face*> faces_;
    std::unordered_map<FaceKey, Face*, FaceKeyHash> face_cache_;
    std::unordered_map<Key, Glyph, KeyHash> glyphs_;
    GLuint atlas_id_ = 0;
    int atlas_size_ = 2048;
    int pen_x_ = 1;
    int pen_y_ = 1;
    int row_h_ = 0;
    bool atlas_full_ = false;
};

}  // namespace bl::ui
