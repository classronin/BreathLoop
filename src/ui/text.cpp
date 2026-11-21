// 文字实现：FreeType 直接栅格化进一张 R8 图集，按需增长（货架打包）。
#include "ui/text.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "render/gl.h"

namespace bl::ui {
namespace {

constexpr int kMaxAtlas = 2048;
const char* kRegularCandidates[] = {R"(C:\Windows\Fonts\msyh.ttc)",
                                    R"(C:\Windows\Fonts\seguisym.ttf)",
                                    R"(C:\Windows\Fonts\simsun.ttc)"};
const char* kBoldCandidates[] = {R"(C:\Windows\Fonts\msyhbd.ttc)",
                                 R"(C:\Windows\Fonts\seguisym.ttf)",
                                 R"(C:\Windows\Fonts\msyh.ttc)"};

bool HasFile(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return false;
    std::fclose(f);
    return true;
}

}  // namespace

struct Text::Face {
    FT_Face handle = nullptr;
    int px = 0;
    bool bold = false;
    float ascender = 0.0f;
    float height = 0.0f;
};

int PointToPx(int point_size) {
    // pyglet 的字体大小是 pt，96 DPI 下 1pt = 4/3 px。
    return static_cast<int>(point_size * 4.0 / 3.0 + 0.5);
}

uint32_t DecodeUtf8(const std::string& text, size_t* index) {
    const size_t i = *index;
    if (i >= text.size()) return 0;
    const unsigned char c0 = static_cast<unsigned char>(text[i]);
    auto tail = [&](size_t at) -> uint32_t {
        if (at >= text.size()) return 0x80u;
        const unsigned char c = static_cast<unsigned char>(text[at]);
        return (c & 0xC0u) == 0x80u ? (c & 0x3Fu) : 0x80u;
    };
    if (c0 < 0x80u) {
        *index = i + 1;
        return c0;
    }
    if ((c0 & 0xE0u) == 0xC0u) {
        *index = i + 2;
        return ((c0 & 0x1Fu) << 6) | tail(i + 1);
    }
    if ((c0 & 0xF0u) == 0xE0u) {
        *index = i + 3;
        return ((c0 & 0x0Fu) << 12) | (tail(i + 1) << 6) | tail(i + 2);
    }
    if ((c0 & 0xF8u) == 0xF0u) {
        *index = i + 4;
        return ((c0 & 0x07u) << 18) | (tail(i + 1) << 12)
               | (tail(i + 2) << 6) | tail(i + 3);
    }
    *index = i + 1;
    return '?';
}

Text& Text::Instance() {
    static Text instance;
    return instance;
}

bool Text::init(std::string* error) {
    if (library_ != nullptr) return true;
    FT_Library library = nullptr;
    if (FT_Init_FreeType(&library) != 0) {
        if (error != nullptr) *error = "FT_Init_FreeType 失败";
        return false;
    }
    library_ = library;

    auto load_chain = [&](const char* const* paths, size_t count, bool bold) {
        for (size_t i = 0; i < count; ++i) {
            if (!HasFile(paths[i])) continue;
            FT_Face face = nullptr;
            if (FT_New_Face(library, paths[i], 0, &face) != 0) continue;
            Face* wrapper = new Face();
            wrapper->handle = face;
            wrapper->bold = bold;
            faces_.push_back(wrapper);
        }
    };
    load_chain(kRegularCandidates,
               sizeof(kRegularCandidates) / sizeof(kRegularCandidates[0]),
               false);
    load_chain(kBoldCandidates,
               sizeof(kBoldCandidates) / sizeof(kBoldCandidates[0]), true);
    if (faces_.empty()) {
        if (error != nullptr) *error = "找不到可用字体";
        return false;
    }

    glGenTextures(1, &atlas_id_);
    glBindTexture(GL_TEXTURE_2D, atlas_id_);
    std::vector<unsigned char> zeros(static_cast<size_t>(kMaxAtlas) * kMaxAtlas,
                                     0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, kMaxAtlas, kMaxAtlas, 0, GL_RED,
                 GL_UNSIGNED_BYTE, zeros.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    atlas_size_ = kMaxAtlas;
    return true;
}

void Text::shutdown() {
    if (atlas_id_ != 0) {
        glDeleteTextures(1, &atlas_id_);
        atlas_id_ = 0;
    }
    for (Face* face : faces_) {
        FT_Done_Face(face->handle);
        delete face;
    }
    faces_.clear();
    face_cache_.clear();
    glyphs_.clear();
    if (library_ != nullptr) {
        FT_Done_FreeType(static_cast<FT_Library>(library_));
        library_ = nullptr;
    }
}

// 同一个 FT_Face 会被多种字号轮流借用（Body 26px、Title 15px、Small 13px 同屏混排），
// 尺寸是 Face 上的可变状态：只在建缓存时设一次，后面命中的那次就会用别人留下的尺寸
// 栅格化，于是同一行里汉字小而数字大。所以每次取用都确认一遍。
void Text::ensure_px(Face* face, int px) {
    if (face == nullptr) return;
    if (face->px == px) return;
    if (FT_Set_Pixel_Sizes(face->handle, 0, static_cast<FT_UInt>(px)) != 0) return;
    face->px = px;
    face->ascender =
        static_cast<float>(face->handle->size->metrics.ascender) / 64.0f;
    face->height = static_cast<float>(face->handle->size->metrics.height) / 64.0f;
}

Text::Face* Text::face_for(int px, bool bold) {
    FaceKey key{px, bold};
    auto it = face_cache_.find(key);
    if (it != face_cache_.end()) {
        ensure_px(it->second, px);
        return it->second;
    }
    Face* chosen = nullptr;
    for (Face* face : faces_) {
        if (face->bold != bold) continue;
        if (FT_Set_Pixel_Sizes(face->handle, 0, static_cast<FT_UInt>(px)) != 0)
            continue;
        chosen = face;
        break;
    }
    if (chosen == nullptr) {
        for (Face* face : faces_) {
            if (FT_Set_Pixel_Sizes(face->handle, 0, static_cast<FT_UInt>(px))
                != 0)
                continue;
            chosen = face;
            break;
        }
    }
    if (chosen == nullptr) return nullptr;
    chosen->px = -1;   // 上面已经设过尺寸，让 ensure_px 补齐度量字段
    ensure_px(chosen, px);
    face_cache_[key] = chosen;
    return chosen;
}

bool Text::upload_region(int x, int y, int w, int h,
                         const unsigned char* data) {
    if (atlas_id_ == 0 || data == nullptr || w <= 0 || h <= 0) return true;
    glBindTexture(GL_TEXTURE_2D, atlas_id_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RED, GL_UNSIGNED_BYTE,
                    data);
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

const Glyph& Text::raster(uint32_t codepoint, int px, bool bold) {
    Key key{codepoint, px, bold};
    auto it = glyphs_.find(key);
    if (it != glyphs_.end()) return it->second;

    Glyph glyph;
    Face* face = face_for(px, bold);
    if (face == nullptr) {
        glyphs_[key] = glyph;
        return glyphs_[key];
    }
    // 字体回退：YaHei 缺的符号（◂ ▸ 之类）交给 Segoe UI Symbol / SimSun。
    FT_Face handle = face->handle;
    if (FT_Get_Char_Index(handle, codepoint) == 0) {
        for (Face* other : faces_) {
            if (FT_Get_Char_Index(other->handle, codepoint) != 0) {
                handle = other->handle;
                ensure_px(other, px);   // 回退面也可能是别的字号留在上面
                break;
            }
        }
    }
    if (FT_Load_Char(handle, codepoint, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL)
        != 0) {
        glyphs_[key] = glyph;
        return glyphs_[key];
    }
    const FT_GlyphSlot slot = handle->glyph;
    glyph.advance = static_cast<float>(slot->advance.x) / 64.0f;
    glyph.x0 = static_cast<float>(slot->bitmap_left);
    glyph.y1 = static_cast<float>(slot->bitmap_top);
    glyph.x1 = glyph.x0 + static_cast<float>(slot->bitmap.width);
    glyph.y0 = glyph.y1 - static_cast<float>(slot->bitmap.rows);

    if (slot->bitmap.width > 0 && slot->bitmap.rows > 0) {
        const int w = static_cast<int>(slot->bitmap.width);
        const int h = static_cast<int>(slot->bitmap.rows);
        if (pen_x_ + w + 1 > atlas_size_) {
            pen_x_ = 1;
            pen_y_ += row_h_ + 1;
            row_h_ = 0;
        }
        if (pen_y_ + h + 1 > atlas_size_) {
            atlas_full_ = true;
            glyphs_[key] = glyph;
            return glyphs_[key];
        }
        // 逐行拷进临时图（FreeType 的 pitch 可能大于宽度）
        std::vector<unsigned char> rows(static_cast<size_t>(w) * h);
        for (int y = 0; y < h; ++y) {
            std::memcpy(rows.data() + static_cast<size_t>(y) * w,
                        slot->bitmap.buffer + static_cast<size_t>(y)
                                                  * slot->bitmap.pitch,
                        static_cast<size_t>(w));
        }
        upload_region(pen_x_, pen_y_, w, h, rows.data());
        glyph.u0 = static_cast<float>(pen_x_) / atlas_size_;
        glyph.v0 = static_cast<float>(pen_y_) / atlas_size_;
        glyph.u1 = static_cast<float>(pen_x_ + w) / atlas_size_;
        glyph.v1 = static_cast<float>(pen_y_ + h) / atlas_size_;
        glyph.empty = false;
        pen_x_ += w + 1;
        row_h_ = row_h_ > h ? row_h_ : h;
    }
    glyphs_[key] = glyph;
    return glyphs_[key];
}

const Glyph& Text::glyph(uint32_t codepoint, int px, bool bold) {
    return raster(codepoint, px, bold);
}

float Text::measure(const std::string& text, int px, bool bold) {
    float x = 0.0f;
    size_t i = 0;
    while (i < text.size()) {
        uint32_t cp = DecodeUtf8(text, &i);
        const Glyph& g = raster(cp, px, bold);
        x += g.advance;
    }
    return x;
}

float Text::ascent(int px, bool bold) {
    Face* face = face_for(px, bold);
    return face != nullptr ? face->ascender : static_cast<float>(px) * 0.8f;
}

float Text::line_height(int px, bool bold) {
    Face* face = face_for(px, bold);
    return face != nullptr ? face->height : static_cast<float>(px) * 1.2f;
}

}  // namespace bl::ui
