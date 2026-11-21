// 2D 画笔实现：一个 program + 一条动态 VBO，按纹理/裁剪变化 flush。
#include "ui/painter.h"

#include <cmath>

#include "ui/text.h"

namespace bl::ui {
namespace {

const char* kVertex = R"GLSL(
#version 330 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;
uniform vec2 u_screen;
out vec2 v_uv;
out vec4 v_color;
void main() {
    vec2 ndc = vec2(a_pos.x / u_screen.x * 2.0 - 1.0,
                    a_pos.y / u_screen.y * 2.0 - 1.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    v_uv = a_uv;
    v_color = a_color;
}
)GLSL";

const char* kFragment = R"GLSL(
#version 330 core
in vec2 v_uv;
in vec4 v_color;
uniform int u_mode;
uniform sampler2D u_tex;
out vec4 out_color;
void main() {
    vec4 color = v_color;
    if (u_mode == 1) {
        color.a *= texture(u_tex, v_uv).r;      // 字形图集：红通道当覆盖率
    } else if (u_mode == 2) {
        vec4 texel = texture(u_tex, v_uv);
        color *= texel;
    }
    out_color = color;
}
)GLSL";

GLuint compile(GLenum type, const char* source) {
    GLuint shader = gl::CreateShader(type);
    gl::ShaderSource(shader, 1, &source, nullptr);
    gl::CompileShader(shader);
    GLint ok = 0;
    gl::GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        gl::DeleteShader(shader);
        return 0;
    }
    return shader;
}

}  // namespace

bool Painter::init(std::string* error) {
    if (ready_) return true;
    GLuint vs = compile(GL_VERTEX_SHADER, kVertex);
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFragment);
    if (vs == 0 || fs == 0) {
        if (vs != 0) gl::DeleteShader(vs);
        if (fs != 0) gl::DeleteShader(fs);
        if (error != nullptr) *error = "界面着色器编译失败";
        return false;
    }
    GLuint program = gl::CreateProgram();
    gl::AttachShader(program, vs);
    gl::AttachShader(program, fs);
    gl::LinkProgram(program);
    GLint ok = 0;
    gl::GetProgramiv(program, GL_LINK_STATUS, &ok);
    gl::DeleteShader(vs);
    gl::DeleteShader(fs);
    if (!ok) {
        gl::DeleteProgram(program);
        if (error != nullptr) *error = "界面着色器链接失败";
        return false;
    }
    program_ = program;
    uniform_screen_ = gl::GetUniformLocation(program_, "u_screen");
    uniform_mode_ = gl::GetUniformLocation(program_, "u_mode");
    uniform_tex_ = gl::GetUniformLocation(program_, "u_tex");

    gl::GenVertexArrays(1, &vao_);
    gl::GenBuffers(1, &vbo_);
    gl::BindVertexArray(vao_);
    gl::BindBuffer(GL_ARRAY_BUFFER, vbo_);
    gl::EnableVertexAttribArray(0);
    gl::VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(0));
    gl::EnableVertexAttribArray(1);
    gl::VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(sizeof(float) * 2));
    gl::EnableVertexAttribArray(2);
    gl::VertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(sizeof(float) * 4));
    gl::BindVertexArray(0);

    glGenTextures(1, &white_);
    glBindTexture(GL_TEXTURE_2D, white_);
    const unsigned char pixel[4] = {255, 255, 255, 255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 pixel);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    ready_ = true;
    return true;
}

void Painter::destroy() {
    if (vbo_ != 0) {
        gl::DeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }
    if (vao_ != 0) {
        gl::DeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    if (white_ != 0) {
        glDeleteTextures(1, &white_);
        white_ = 0;
    }
    if (program_ != 0) {
        gl::DeleteProgram(program_);
        program_ = 0;
    }
    ready_ = false;
}

void Painter::begin(int width, int height) {
    width_ = width > 0 ? width : 1;
    height_ = height > 0 ? height : 1;
    vertices_.clear();
    clips_.clear();
    texture_ = 0;
    mode_ = kModeSolid;
    gl::UseProgram(program_);
    gl::Uniform2f(uniform_screen_, static_cast<float>(width_),
                static_cast<float>(height_));
    gl::Uniform1i(uniform_tex_, 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
}

void Painter::end() { flush(); }

void Painter::set_texture(GLuint texture, int mode) {
    if (texture == texture_ && mode == mode_) return;
    flush();
    texture_ = texture;
    mode_ = mode;
}

void Painter::push_clip(float x, float y, float w, float h) {
    flush();
    clips_.clear();
    clips_.push_back(Clip{x, y, w, h});
}

void Painter::pop_clip() {
    flush();
    clips_.clear();
}

void Painter::flush() {
    if (vertices_.empty()) return;
    gl::UseProgram(program_);
    gl::Uniform2f(uniform_screen_, static_cast<float>(width_),
                static_cast<float>(height_));
    gl::Uniform1i(uniform_mode_, mode_);
    gl::ActiveTexture(GL_TEXTURE0);
    if (mode_ == kModeMask || mode_ == kModeSprite) {
        glBindTexture(GL_TEXTURE_2D, texture_);
    } else {
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    if (!clips_.empty()) {
        const Clip& clip = clips_.back();
        const int x0 = static_cast<int>(std::floor(clip.x));
        const int y0 = static_cast<int>(std::floor(clip.y));
        const int x1 = static_cast<int>(std::ceil(clip.x + clip.w));
        const int y1 = static_cast<int>(std::ceil(clip.y + clip.h));
        glEnable(GL_SCISSOR_TEST);
        glScissor(x0, y0, x1 - x0 > 0 ? x1 - x0 : 1,
                  y1 - y0 > 0 ? y1 - y0 : 1);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
    gl::BindVertexArray(vao_);
    gl::BindBuffer(GL_ARRAY_BUFFER, vbo_);
    gl::BufferData(GL_ARRAY_BUFFER,
                 static_cast<long long>(vertices_.size())
                     * static_cast<long long>(sizeof(Vertex)),
                 vertices_.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices_.size()));
    gl::BindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_SCISSOR_TEST);
    vertices_.clear();
}

void Painter::add_quad(float x0, float y0, float x1, float y1, float u0,
                       float v0, float u1, float v1, cfg::Rgba color,
                       float alpha) {
    const float r = color.r / 255.0f;
    const float g = color.g / 255.0f;
    const float b = color.b / 255.0f;
    const float a = (color.a / 255.0f) * alpha;
    const Vertex tl{x0, y1, u0, v0, r, g, b, a};
    const Vertex tr{x1, y1, u1, v0, r, g, b, a};
    const Vertex br{x1, y0, u1, v1, r, g, b, a};
    const Vertex bl{x0, y0, u0, v1, r, g, b, a};
    vertices_.push_back(tl);
    vertices_.push_back(bl);
    vertices_.push_back(br);
    vertices_.push_back(tl);
    vertices_.push_back(br);
    vertices_.push_back(tr);
}

void Painter::add_triangle(float x0, float y0, float x1, float y1, float x2,
                           float y2, cfg::Rgba color, float alpha) {
    const float r = color.r / 255.0f;
    const float g = color.g / 255.0f;
    const float b = color.b / 255.0f;
    const float a = (color.a / 255.0f) * alpha;
    vertices_.push_back(Vertex{x0, y0, 0.5f, 0.5f, r, g, b, a});
    vertices_.push_back(Vertex{x1, y1, 0.5f, 0.5f, r, g, b, a});
    vertices_.push_back(Vertex{x2, y2, 0.5f, 0.5f, r, g, b, a});
}

void Painter::rect(float x, float y, float w, float h, cfg::Rgba color,
                   float alpha) {
    if (w <= 0.0f || h <= 0.0f) return;
    set_texture(0, kModeSolid);
    add_quad(x, y, x + w, y + h, 0.5f, 0.5f, 0.5f, 0.5f, color, alpha);
}

void Painter::line(float x0, float y0, float x1, float y1, float thickness,
                   cfg::Rgba color, float alpha) {
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length < 1e-6f) return;
    const float nx = -dy / length * thickness * 0.5f;
    const float ny = dx / length * thickness * 0.5f;
    set_texture(0, kModeSolid);
    add_triangle(x0 + nx, y0 + ny, x0 - nx, y0 - ny, x1 + nx, y1 + ny, color,
                 alpha);
    add_triangle(x0 - nx, y0 - ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, color,
                 alpha);
}

void Painter::circle(float cx, float cy, float radius, cfg::Rgba color,
                     float alpha, int segments) {
    ellipse(cx, cy, radius, radius, 0.0f, color, alpha, segments);
}

void Painter::ellipse(float cx, float cy, float rx, float ry,
                      float rotation_deg, cfg::Rgba color, float alpha,
                      int segments) {
    if (segments < 6) segments = 6;
    const float r = rotation_deg * 3.14159265f / 180.0f;
    const float cos_r = std::cos(r);
    const float sin_r = std::sin(r);
    set_texture(0, kModeSolid);
    for (int i = 0; i < segments; ++i) {
        const float a0 = 6.2831853f * static_cast<float>(i) / segments;
        const float a1 = 6.2831853f * static_cast<float>(i + 1) / segments;
        float px0 = std::cos(a0) * rx;
        float py0 = std::sin(a0) * ry;
        float px1 = std::cos(a1) * rx;
        float py1 = std::sin(a1) * ry;
        add_triangle(cx, cy, cx + px0 * cos_r - py0 * sin_r,
                     cy + px0 * sin_r + py0 * cos_r,
                     cx + px1 * cos_r - py1 * sin_r,
                     cy + px1 * sin_r + py1 * cos_r, color, alpha);
    }
}

void Painter::polygon(const std::vector<std::pair<float, float>>& points,
                      cfg::Rgba color, float alpha) {
    if (points.size() < 3) return;
    float mx = 0.0f;
    float my = 0.0f;
    for (const auto& p : points) {
        mx += p.first;
        my += p.second;
    }
    mx /= static_cast<float>(points.size());
    my /= static_cast<float>(points.size());
    set_texture(0, kModeSolid);
    for (size_t i = 0; i < points.size(); ++i) {
        const auto& a = points[i];
        const auto& b = points[(i + 1) % points.size()];
        add_triangle(mx, my, a.first, a.second, b.first, b.second, color, alpha);
    }
}

float Painter::measure(const std::string& content, cfg::FontRole role,
                       bool bold) {
    return Text::Instance().measure(content, PointToPx(cfg::FontPointOf(role)),
                                    bold);
}

float Painter::line_height(cfg::FontRole role, bool bold) {
    return Text::Instance().line_height(PointToPx(cfg::FontPointOf(role)), bold);
}

float Painter::text(const std::string& content, float x, float y_center,
                    cfg::FontRole role, bool bold, cfg::Rgba color, Align align,
                    float alpha) {
    const int px = PointToPx(cfg::FontPointOf(role));
    Text& font = Text::Instance();
    const float width = font.measure(content, px, bold);
    float pen = x;
    if (align == Align::kCenter) pen = x - width * 0.5f;
    if (align == Align::kRight) pen = x - width;
    // 垂直居中：pyglet 的 anchor_y='center' 居中的是整行字框，
    // 基线 = 中心 - (上伸 - 下伸)/2，下伸 = 行高 - 上伸。
    const float base_y =
        y_center - font.ascent(px, bold) + font.line_height(px, bold) * 0.5f;
    set_texture(font.atlas(), kModeMask);
    size_t i = 0;
    while (i < content.size()) {
        const uint32_t cp = DecodeUtf8(content, &i);
        const Glyph& glyph = font.glyph(cp, px, bold);
        if (!glyph.empty) {
            add_quad(pen + glyph.x0, base_y + glyph.y0, pen + glyph.x1,
                     base_y + glyph.y1, glyph.u0, glyph.v0, glyph.u1,
                     glyph.v1, color, alpha);
        }
        pen += glyph.advance;
    }
    return width;
}

float Painter::text_top(const std::string& content, float x, float y_top,
                        cfg::FontRole role, bool bold, cfg::Rgba color,
                        Align align, float alpha) {
    const int px = PointToPx(cfg::FontPointOf(role));
    const float ascent = Text::Instance().ascent(px, bold);
    return text(content, x, y_top - ascent * 0.5f, role, bold, color, align,
                alpha);
}

void Painter::sprite_uv(GLuint texture, float x, float y, float w, float h,
                        float u0, float v0, float u1, float v1, float alpha) {
    if (texture == 0 || w <= 0.0f || h <= 0.0f) return;
    set_texture(texture, kModeSprite);
    add_quad(x, y, x + w, y + h, u0, v0, u1, v1, cfg::FromRgb(255, 255, 255),
             alpha);
}

void Painter::sprite(GLuint texture, float x, float y, float w, float h,
                     float alpha) {
    sprite_uv(texture, x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, alpha);
}

}  // namespace bl::ui
