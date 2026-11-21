// 绘制底座：passer 的最小替代层。
//
// 只做四件事：纹理（顶向下、不预乘）、着色器程序、按窗口坐标画一个带纹理的
// 四边形、离屏帧缓冲。它不知道图层、不知道 BBOX、不知道鼠标——那都不是它的活。
//
// 坐标：窗口坐标，左下角原点、y 向上（与 pyglet 事件一致）。
// 纹理：数据顶向下（第一行是图像顶），UV 的 v=0 也对齐图像顶，因此不翻行。
#pragma once

#include <string>
#include <vector>

#include "render/gl.h"

namespace bl {

class Texture {
public:
    Texture() = default;
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    ~Texture();

    // channels: 4 = RGBA8，1 = R8（数据都按顶向下给）
    // point_mag: 放大时用点采样（GL_NEAREST）。图像浏览器（
    // E:\GitHub\C\ImageBrowser\src\BasicProcessing.cpp:868-891 的 point sampling）
    // 放大一律点采样：源像素整块放大，不插值，套索抠出来的透明边就不会被
    // 双线性混成灰黑描边。缩小仍走 GL_LINEAR，保持照片缩小不锯齿。
    void create(int width, int height, int channels, const unsigned char* data,
                bool point_mag = false);
    // 只重传一块矩形（像素级编辑的实时预览用；数据按顶向下）。
    // row_stride：源数据一整行的像素数（源图的完整宽度）。quick 里 w 只是
    // 子矩形宽，若源数据每行跨 stride 像素，必须显式告诉 GL，否则它按 w 读行
    // → 整张图被压进子矩形（表现为"方块里的镜像图"）。0 表示等于 w。
    void update_region(int x, int y, int w, int h, const unsigned char* data,
                       int channels, int row_stride = 0);
    void destroy();

    GLuint id() const { return id_; }
    int width() const { return width_; }
    int height() const { return height_; }
    bool valid() const { return id_ != 0; }

private:
    GLuint id_ = 0;
    int width_ = 0;
    int height_ = 0;
};

class Program {
public:
    Program() = default;
    Program(const Program&) = delete;
    Program& operator=(const Program&) = delete;
    ~Program();

    bool build(const std::string& vertex_source,
               const std::string& fragment_source, std::string* error);
    void destroy();

    GLuint id() const { return id_; }
    int uniform(const char* name) const;
    void use() const;

    void set_int(const char* name, int value) const;
    void set_float(const char* name, float value) const;
    void set_vec2(const char* name, float x, float y) const;
    void set_float_array(const char* name, const float* values, int count,
                         bool vector2) const;

private:
    GLuint id_ = 0;
};

// 四边形绘制器：共用一套 VAO/VBO，每次上传 4 顶点。
class Renderer {
public:
    void begin(int framebuffer_width, int framebuffer_height);
    // uv 顺序：左上 (u0,v0) → 右下 (u1,v1)
    void draw_textured(const Program& program, const Texture& texture, int unit,
                       double left, double bottom, double right, double top,
                       double u0 = 0.0, double v0 = 0.0, double u1 = 1.0,
                       double v1 = 1.0);
    // 任意四边形（已旋转的图层）：p[8] = 左上/右上/右下/左下 的 (x,y)，
    // uv 固定为 (0,0)(1,0)(1,1)(0,1)，与 draw_textured 的 v=0 在上一致。
    void draw_quad(const Program& program, const Texture& texture, int unit,
                   const double p[8]);
    void destroy();

private:
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint ibo_ = 0;
    float viewport_width_ = 1.0f;
    float viewport_height_ = 1.0f;
};

// 离屏帧缓冲：一张 RGBA8 颜色附件，读回时翻成顶向下。
class Framebuffer {
public:
    Framebuffer() = default;
    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;
    ~Framebuffer();

    bool create(int width, int height);
    void destroy();
    void bind();
    void unbind();
    // RGBA8，自上而下（导出与比对都按这个行序）
    std::vector<unsigned char> read_rgba();

    int width() const { return width_; }
    int height() const { return height_; }
    GLuint texture_id() const { return texture_id_; }

private:
    GLuint fbo_ = 0;
    GLuint texture_id_ = 0;
    int width_ = 0;
    int height_ = 0;
};

}  // namespace bl
