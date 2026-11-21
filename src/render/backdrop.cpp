#include "render/backdrop.h"

#include "config.h"

namespace bl {
namespace {

const char* kVertex = R"GLSL(
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

const char* kFragment = R"GLSL(
#version 330
uniform vec2 u_size;
uniform vec4 u_dark;
uniform vec4 u_check_a;
uniform vec4 u_check_b;
uniform vec2 u_min;
uniform vec2 u_max;
uniform float u_cell;
uniform vec4 u_edge;
uniform int u_frame;

in vec2 v_uv;
in vec4 v_color;
out vec4 frag_color;

void main() {
    // draw_textured 的 v=0 绑在 quad 上边（gfx.cpp:193-198），而这里的
    // u_min/u_max 是场景坐标（左下原点、y 向上）。所以要先把它翻回 y 向上，
    // 否则画布框与图层上下互为镜像：静止居中时看不出来，一平移就一个跟手、
    // 一个反向。（同一条规矩见项目导航偏差 32：几何交给像素/着色器必须翻。）
    vec2 p = vec2(v_uv.x, 1.0 - v_uv.y) * u_size;
    if (p.x < u_min.x || p.x > u_max.x || p.y < u_min.y || p.y > u_max.y) {
        frag_color = u_frame != 0 ? vec4(0.0) : u_dark;
        return;
    }
    // 边界与底色同源：都用同一对 u_min/u_max，按像素距离判 1px，天然贴合。
    float dx = min(p.x - u_min.x, u_max.x - p.x);
    float dy = min(p.y - u_min.y, u_max.y - p.y);
    if (dx < 1.0 || dy < 1.0) {
        frag_color = u_edge;
        return;
    }
    if (u_frame != 0) {
        frag_color = vec4(0.0);
        return;
    }
    // 棋盘锚在屏幕原点、格子按屏幕像素算（对齐 ImageEditor CanvasNode.cpp:269-280），
    // 平移视图时格子不跟着滑。
    int ix = int(floor(p.x / u_cell));
    int iy = int(floor(p.y / u_cell));
    frag_color = ((ix + iy) & 1) == 0 ? u_check_a : u_check_b;
}
)GLSL";

Program g_program;
Texture g_white;
bool g_ready = false;

}  // namespace

bool Backdrop::init(std::string* error) {
    if (g_ready) return true;
    if (!g_program.build(kVertex, kFragment, error)) return false;
    const unsigned char white[4] = {255, 255, 255, 255};
    g_white.create(1, 1, 4, white);
    g_ready = g_white.valid();
    return g_ready;
}

void Backdrop::draw(Renderer& renderer, int fb_w, int fb_h, double left,
                    double bottom, double right, double top, bool frame_only) {
    if (!g_ready) return;
    Program& program = g_program;
    program.use();
    program.set_int("u_frame", frame_only ? 1 : 0);
    program.set_vec2("u_size", static_cast<float>(fb_w),
                     static_cast<float>(fb_h));
    program.set_vec2("u_min", static_cast<float>(left),
                     static_cast<float>(bottom));
    program.set_vec2("u_max", static_cast<float>(right),
                     static_cast<float>(top));
    program.set_float("u_cell", static_cast<float>(cfg::CANVAS_CHECK_CELL));
    gl::Uniform4f(program.uniform("u_dark"), cfg::CANVAS_DARK.r / 255.0f,
                cfg::CANVAS_DARK.g / 255.0f, cfg::CANVAS_DARK.b / 255.0f,
                cfg::CANVAS_DARK.a / 255.0f);
    gl::Uniform4f(program.uniform("u_check_a"), cfg::CANVAS_ALPHA_A.r / 255.0f,
                cfg::CANVAS_ALPHA_A.g / 255.0f, cfg::CANVAS_ALPHA_A.b / 255.0f,
                cfg::CANVAS_ALPHA_A.a / 255.0f);
    gl::Uniform4f(program.uniform("u_check_b"), cfg::CANVAS_ALPHA_B.r / 255.0f,
                cfg::CANVAS_ALPHA_B.g / 255.0f, cfg::CANVAS_ALPHA_B.b / 255.0f,
                cfg::CANVAS_ALPHA_B.a / 255.0f);
    gl::Uniform4f(program.uniform("u_edge"), cfg::CANVAS_EDGE.r / 255.0f,
                cfg::CANVAS_EDGE.g / 255.0f, cfg::CANVAS_EDGE.b / 255.0f,
                cfg::CANVAS_EDGE.a / 255.0f);
    renderer.draw_textured(program, g_white, 0, 0.0, 0.0,
                           static_cast<double>(fb_w),
                           static_cast<double>(fb_h), 0.0, 0.0, 1.0, 1.0);
}

}  // namespace bl
