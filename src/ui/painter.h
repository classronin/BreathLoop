// 2D 画笔：界面唯一的绘制出口。
//
// 只有一种顶点格式（像素坐标 + UV + RGBA），一次 flush 一个纹理/一个裁剪矩形。
// 矩形、线、圆、椭圆、文字、纹理块都走这里；上层（控件、面板、覆盖层）
// 只管给出"画在哪、什么颜色"，不碰 GL。
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "config.h"
#include "render/gl.h"

namespace bl::ui {

enum class Align { kLeft, kCenter, kRight };

class Painter {
public:
    bool init(std::string* error);
    void destroy();

    // 窗口像素尺寸（左下角原点、y 向上，与界面布局同一个坐标系）
    void begin(int width, int height);
    void end();

    void push_clip(float x, float y, float w, float h);
    void pop_clip();

    void rect(float x, float y, float w, float h, cfg::Rgba color,
              float alpha = 1.0f);
    void line(float x0, float y0, float x1, float y1, float thickness,
              cfg::Rgba color, float alpha = 1.0f);
    void circle(float cx, float cy, float radius, cfg::Rgba color,
                float alpha = 1.0f, int segments = 32);
    void ellipse(float cx, float cy, float rx, float ry, float rotation_deg,
                 cfg::Rgba color, float alpha = 1.0f, int segments = 48);
    // 任意凸多边形填充（按重心做扇形），给"不能在屏幕空间用椭圆表达"的图形用
    void polygon(const std::vector<std::pair<float, float>>& points,
                 cfg::Rgba color, float alpha = 1.0f);

    // y 是这一行的垂直中心；字号只传角色，点号由 config.h 的字号表决定
    float text(const std::string& content, float x, float y_center,
               cfg::FontRole role, bool bold, cfg::Rgba color,
               Align align = Align::kLeft, float alpha = 1.0f);
    // 顶端对齐（多行状态文字用）
    float text_top(const std::string& content, float x, float y_top,
                   cfg::FontRole role, bool bold, cfg::Rgba color,
                   Align align = Align::kLeft, float alpha = 1.0f);
    float measure(const std::string& content, cfg::FontRole role, bool bold);
    // 整行行高（含上下伸），用来给文字垫背景条/算行距
    float line_height(cfg::FontRole role, bool bold);

    // 纹理块：v 轴按"图像顶在上"给（与 render::Texture 的顶向下数据一致）
    void sprite(GLuint texture, float x, float y, float w, float h,
                float alpha = 1.0f);
    void sprite_uv(GLuint texture, float x, float y, float w, float h,
                   float u0, float v0, float u1, float v1, float alpha = 1.0f);

    int width() const { return width_; }
    int height() const { return height_; }

private:
    struct Vertex {
        float x, y, u, v;
        float r, g, b, a;
    };
    struct Clip {
        float x, y, w, h;
    };

    static constexpr int kModeSolid = 0;
    static constexpr int kModeMask = 1;
    static constexpr int kModeSprite = 2;

    void flush();
    void set_texture(GLuint texture, int mode);
    void add_quad(float x0, float y0, float x1, float y1, float u0, float v0,
                  float u1, float v1, cfg::Rgba color, float alpha);
    void add_triangle(float x0, float y0, float x1, float y1, float x2,
                      float y2, cfg::Rgba color, float alpha);

    GLuint program_ = 0;
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint white_ = 0;
    int uniform_screen_ = -1;
    int uniform_mode_ = -1;
    int uniform_tex_ = -1;

    std::vector<Vertex> vertices_;
    std::vector<Clip> clips_;
    GLuint texture_ = 0;
    int mode_ = kModeSolid;
    int width_ = 1;
    int height_ = 1;
    bool ready_ = false;
};

}  // namespace bl::ui
