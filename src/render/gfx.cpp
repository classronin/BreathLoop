#include "render/gfx.h"

#include <algorithm>

namespace bl {
namespace {

// 顶点着色器：窗口坐标（左下原点、y 向上）→ NDC；UV 的 v=0 对齐图像顶行。
const char* kVertexSource = R"GLSL(
#version 330
uniform vec2 u_viewport;
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
out vec2 v_uv;
out vec4 v_color;

void main() {
    v_uv = a_uv;
    v_color = vec4(1.0);
    vec2 ndc = (a_pos / u_viewport) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
)GLSL";

GLuint Compile(GLenum type, const char* source, std::string* error) {
    const GLuint shader = gl::CreateShader(type);
    gl::ShaderSource(shader, 1, &source, nullptr);
    gl::CompileShader(shader);
    GLint status = 0;
    gl::GetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status == GL_FALSE) {
        GLint length = 0;
        gl::GetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        std::string log(static_cast<size_t>(std::max(1, length)), '\0');
        gl::GetShaderInfoLog(shader, length, nullptr, log.data());
        if (error != nullptr) *error = log;
        gl::DeleteShader(shader);
        return 0;
    }
    return shader;
}

}  // namespace

// ---- Texture ----

Texture::~Texture() {
    destroy();
}

void Texture::create(int width, int height, int channels,
                     const unsigned char* data, bool point_mag) {
    destroy();
    width_ = width;
    height_ = height;
    glGenTextures(1, &id_);
    glBindTexture(GL_TEXTURE_2D, id_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    const GLenum format = channels == 1 ? GL_RED : GL_RGBA;
    const GLint internal_format = channels == 1 ? GL_R8 : GL_RGBA8;
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, format,
                 GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    point_mag ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Texture::update_region(int x, int y, int w, int h,
                            const unsigned char* data, int channels,
                            int row_stride) {
    if (id_ == 0 || data == nullptr || w <= 0 || h <= 0) return;
    if (x < 0 || y < 0 || x + w > width_ || y + h > height_) return;
    glBindTexture(GL_TEXTURE_2D, id_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    const int stride = row_stride > 0 ? row_stride : w;
    glPixelStorei(GL_UNPACK_ROW_LENGTH, stride);
    const GLenum format = channels == 1 ? GL_RED : GL_RGBA;
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, format, GL_UNSIGNED_BYTE,
                    data);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Texture::destroy() {
    if (id_ != 0) {
        glDeleteTextures(1, &id_);
        id_ = 0;
    }
    width_ = 0;
    height_ = 0;
}

// ---- Program ----

Program::~Program() {
    destroy();
}

bool Program::build(const std::string& vertex_source,
                    const std::string& fragment_source, std::string* error) {
    destroy();
    const std::string vertex = vertex_source.empty() ? kVertexSource : vertex_source;
    const GLuint vertex_id = Compile(GL_VERTEX_SHADER, vertex.c_str(), error);
    if (vertex_id == 0) return false;
    const GLuint fragment_id =
        Compile(GL_FRAGMENT_SHADER, fragment_source.c_str(), error);
    if (fragment_id == 0) {
        gl::DeleteShader(vertex_id);
        return false;
    }
    id_ = gl::CreateProgram();
    gl::AttachShader(id_, vertex_id);
    gl::AttachShader(id_, fragment_id);
    gl::LinkProgram(id_);
    gl::DeleteShader(vertex_id);
    gl::DeleteShader(fragment_id);
    GLint status = 0;
    gl::GetProgramiv(id_, GL_LINK_STATUS, &status);
    if (status == GL_FALSE) {
        GLint length = 0;
        gl::GetProgramiv(id_, GL_INFO_LOG_LENGTH, &length);
        std::string log(static_cast<size_t>(std::max(1, length)), '\0');
        gl::GetProgramInfoLog(id_, length, nullptr, log.data());
        if (error != nullptr) *error = log;
        destroy();
        return false;
    }
    return true;
}

void Program::destroy() {
    if (id_ != 0) {
        gl::DeleteProgram(id_);
        id_ = 0;
    }
}

int Program::uniform(const char* name) const {
    return gl::GetUniformLocation(id_, name);
}

void Program::use() const {
    gl::UseProgram(id_);
}

void Program::set_int(const char* name, int value) const {
    gl::Uniform1i(uniform(name), value);
}

void Program::set_float(const char* name, float value) const {
    gl::Uniform1f(uniform(name), value);
}

void Program::set_vec2(const char* name, float x, float y) const {
    gl::Uniform2f(uniform(name), x, y);
}

void Program::set_float_array(const char* name, const float* values, int count,
                              bool vector2) const {
    if (vector2) {
        gl::Uniform2fv(uniform(name), count, values);
    } else {
        gl::Uniform1fv(uniform(name), count, values);
    }
}

// ---- Renderer ----

void Renderer::begin(int framebuffer_width, int framebuffer_height) {
    viewport_width_ = static_cast<float>(std::max(1, framebuffer_width));
    viewport_height_ = static_cast<float>(std::max(1, framebuffer_height));
    glViewport(0, 0, framebuffer_width, framebuffer_height);

    if (vao_ == 0) {
        gl::GenVertexArrays(1, &vao_);
        gl::GenBuffers(1, &vbo_);
        gl::GenBuffers(1, &ibo_);
        gl::BindVertexArray(vao_);
        gl::BindBuffer(GL_ARRAY_BUFFER, vbo_);
        gl::BindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
        const unsigned int indices[6] = {0, 1, 2, 0, 2, 3};
        gl::BufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices,
                       GL_STATIC_DRAW);
        gl::EnableVertexAttribArray(0);
        gl::VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                                4 * static_cast<GLsizei>(sizeof(float)),
                                reinterpret_cast<const void*>(0));
        gl::EnableVertexAttribArray(1);
        gl::VertexAttribPointer(
            1, 2, GL_FLOAT, GL_FALSE, 4 * static_cast<GLsizei>(sizeof(float)),
            reinterpret_cast<const void*>(2 * sizeof(float)));
        gl::BindVertexArray(0);
    }
}

void Renderer::draw_textured(const Program& program, const Texture& texture,
                            int unit, double left, double bottom, double right,
                            double top, double u0, double v0, double u1,
                            double v1) {
    if (vao_ == 0 || !program.id() || !texture.valid()) return;

    program.use();
    program.set_vec2("u_viewport", viewport_width_, viewport_height_);
    gl::ActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, texture.id());

    const float verts[16] = {
        static_cast<float>(left),  static_cast<float>(top),    static_cast<float>(u0), static_cast<float>(v0),
        static_cast<float>(right), static_cast<float>(top),    static_cast<float>(u1), static_cast<float>(v0),
        static_cast<float>(right), static_cast<float>(bottom), static_cast<float>(u1), static_cast<float>(v1),
        static_cast<float>(left),  static_cast<float>(bottom), static_cast<float>(u0), static_cast<float>(v1),
    };

    gl::BindVertexArray(vao_);
    gl::BindBuffer(GL_ARRAY_BUFFER, vbo_);
    gl::BufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
    gl::BindVertexArray(0);
}

void Renderer::draw_quad(const Program& program, const Texture& texture,
                         int unit, const double p[8]) {
    if (vao_ == 0 || !program.id() || !texture.valid()) return;
    program.use();
    program.set_vec2("u_viewport", viewport_width_, viewport_height_);
    gl::ActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, texture.id());

    const float verts[16] = {
        static_cast<float>(p[0]), static_cast<float>(p[1]), 0.0f, 0.0f,
        static_cast<float>(p[2]), static_cast<float>(p[3]), 1.0f, 0.0f,
        static_cast<float>(p[4]), static_cast<float>(p[5]), 1.0f, 1.0f,
        static_cast<float>(p[6]), static_cast<float>(p[7]), 0.0f, 1.0f,
    };
    gl::BindVertexArray(vao_);
    gl::BindBuffer(GL_ARRAY_BUFFER, vbo_);
    gl::BufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
    gl::BindVertexArray(0);
}

void Renderer::destroy() {
    if (vbo_ != 0) {
        gl::DeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }
    if (ibo_ != 0) {
        gl::DeleteBuffers(1, &ibo_);
        ibo_ = 0;
    }
    if (vao_ != 0) {
        gl::DeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
}

// ---- Framebuffer ----

Framebuffer::~Framebuffer() {
    destroy();
}

bool Framebuffer::create(int width, int height) {
    destroy();
    width_ = width;
    height_ = height;
    glGenTextures(1, &texture_id_);
    glBindTexture(GL_TEXTURE_2D, texture_id_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);

    gl::GenFramebuffers(1, &fbo_);
    gl::BindFramebuffer(GL_FRAMEBUFFER, fbo_);
    gl::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                             texture_id_, 0);
    const bool complete =
        gl::CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (!complete) destroy();
    return complete;
}

void Framebuffer::destroy() {
    if (fbo_ != 0) {
        gl::DeleteFramebuffers(1, &fbo_);
        fbo_ = 0;
    }
    if (texture_id_ != 0) {
        glDeleteTextures(1, &texture_id_);
        texture_id_ = 0;
    }
    width_ = 0;
    height_ = 0;
}

void Framebuffer::bind() {
    gl::BindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, width_, height_);
}

void Framebuffer::unbind() {
    gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
}

std::vector<unsigned char> Framebuffer::read_rgba() {
    std::vector<unsigned char> pixels(
        static_cast<size_t>(width_) * static_cast<size_t>(height_) * 4, 0);
    gl::BindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    gl::BindFramebuffer(GL_FRAMEBUFFER, 0);

    // GL 的行序自下而上，导出链要自上而下：这里翻一次，别处不必再翻。
    std::vector<unsigned char> flipped(pixels.size(), 0);
    const size_t stride = static_cast<size_t>(width_) * 4;
    for (int y = 0; y < height_; ++y) {
        const unsigned char* src =
            pixels.data() + stride * static_cast<size_t>(height_ - 1 - y);
        unsigned char* dst = flipped.data() + stride * static_cast<size_t>(y);
        for (size_t i = 0; i < stride; ++i) dst[i] = src[i];
    }
    return flipped;
}

}  // namespace bl
