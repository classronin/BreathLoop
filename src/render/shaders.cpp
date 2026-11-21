#include "render/shaders.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "domain/region.h"

namespace bl {
namespace {

// 与 Python 版逐字相同的片段着色器；占位符在编译前替换。
const char* kFragmentTemplate = R"GLSL(
#version 330
uniform sampler2D u_texture;
uniform sampler2D u_mask;
uniform float u_pma;
uniform vec2 u_size;
uniform vec2 u_center[__MAX_REGIONS__];
uniform vec2 u_radius[__MAX_REGIONS__];
uniform vec2 u_rot[__MAX_REGIONS__];
uniform float u_amp_x[__MAX_REGIONS__];
uniform float u_amp_y[__MAX_REGIONS__];
uniform int u_count;
uniform float u_feather;

in vec2 v_uv;
in vec4 v_color;
out vec4 frag_color;

void main() {
    vec2 v_px = v_uv * u_size;
    float edge = texture(u_mask, v_uv).r * 255.0 - __FIELD_BIAS__;
    float soft = max(u_feather, 0.001);
    float gate = smoothstep(-soft, soft, edge);

    vec2 disp = vec2(0.0);

    for (int i = 0; i < __MAX_REGIONS__; i++) {
        if (i >= u_count) break;

        vec2 dw = v_px - u_center[i];
        vec2 cs = u_rot[i];
        vec2 d = vec2(dw.x * cs.x + dw.y * cs.y,
                      -dw.x * cs.y + dw.y * cs.x);

        vec2 nd = d / max(u_radius[i], vec2(1.0));
        float r = length(nd);
        float w = clamp(1.0 - r, 0.0, 1.0);
        w = w * w * (3.0 - 2.0 * w);
        vec2 s = vec2(u_amp_x[i], u_amp_y[i]);
        vec2 dl = vec2(d.x * s.x, d.y * s.y) * w;

        vec2 dir = normalize(d + vec2(1e-6));
        float radial = dot(dl, dir);
        vec2 tangent = vec2(-dir.y, dir.x);
        dl += tangent * (radial * __POISSON__
                         * sin(atan(d.y, d.x) * 2.0)) * w;

        disp += vec2(dl.x * cs.x - dl.y * cs.y,
                     dl.x * cs.y + dl.y * cs.x);
    }

    disp *= gate;
    vec2 uv = (v_px - disp) / u_size;
    vec4 c;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        c = vec4(0.0);
    } else {
        c = texture(u_texture, uv);
    }
    frag_color = mix(vec4(c.rgb * c.a, c.a), c, u_pma) * v_color;
}
)GLSL";

std::string Substitute(const char* text) {
    std::string out(text);
    struct Pair {
        const char* token;
        std::string value;
    };
    const Pair pairs[3] = {
        {"__MAX_REGIONS__", std::to_string(kMaxRegions)},
        {"__FIELD_BIAS__", "127"},
        {"__POISSON__", "0.35"},
    };
    for (const Pair& pair : pairs) {
        size_t at = 0;
        while ((at = out.find(pair.token, at)) != std::string::npos) {
            out.replace(at, std::strlen(pair.token), pair.value);
            at += pair.value.size();
        }
    }
    return out;
}

Texture g_inside_all;
bool g_inside_all_ready = false;

}  // namespace

Program& BreathStyle::program() {
    static Program instance;
    return instance;
}

bool BreathStyle::init_program(std::string* error) {
    return program().build(std::string(), Substitute(kFragmentTemplate), error);
}

const Texture& inside_all_mask() {
    if (!g_inside_all_ready) {
        const unsigned char full = 255;
        g_inside_all.create(1, 1, 1, &full);
        g_inside_all_ready = true;
    }
    return g_inside_all;
}

void BreathStyle::bind(Program& program, const Texture& inside_all) const {
    program.use();
    program.set_vec2("u_size", static_cast<float>(img_w), static_cast<float>(img_h));

    const std::vector<BBoxView> empty;
    const std::vector<BBoxView>& active = deform ? views : empty;
    const int n = std::min(static_cast<int>(active.size()), kMaxRegions);
    program.set_int("u_count", n);

    // 打包顺序与 Python 完全一致：中心/半径/旋转 2*MAX 个 float，幅度各 MAX 个。
    float centers[2 * kMaxRegions] = {0};
    float radii[2 * kMaxRegions] = {0};
    float rots[2 * kMaxRegions] = {0};
    float amp_x[kMaxRegions] = {0};
    float amp_y[kMaxRegions] = {0};

    for (int i = 0; i < n; ++i) {
        const BBoxView& view = active[static_cast<size_t>(i)];
        // BBOX 参数在图层局部坐标里（世界 y 向上），着色器的 v_px 是纹理像素
        // 坐标（y 向下）。这里做一次上下镜像：中心翻到 H - cy、旋转角取反，
        // 幅度不变——镜像后 d.y 自带负号，dl.y 的符号正好对上。
        centers[i * 2] = static_cast<float>(view.cx);
        centers[i * 2 + 1] = static_cast<float>(img_h - view.cy);
        radii[i * 2] = static_cast<float>(std::max(view.rx, 1.0));
        radii[i * 2 + 1] = static_cast<float>(std::max(view.ry, 1.0));
        const double angle = -view.rot * (3.14159265358979323846 / 180.0);
        rots[i * 2] = static_cast<float>(std::cos(angle));
        rots[i * 2 + 1] = static_cast<float>(std::sin(angle));
        amp_x[i] = static_cast<float>(view.disp_x());
        amp_y[i] = static_cast<float>(view.disp_y());
    }
    // 补齐到满额：未用的槽位不参与循环，但 uniform 数组必须整段写完。
    for (int i = n; i < kMaxRegions; ++i) {
        radii[i * 2] = 1.0f;
        radii[i * 2 + 1] = 1.0f;
        rots[i * 2] = 1.0f;
        rots[i * 2 + 1] = 0.0f;
    }

    program.set_float_array("u_center", centers, kMaxRegions, true);
    program.set_float_array("u_radius", radii, kMaxRegions, true);
    program.set_float_array("u_rot", rots, kMaxRegions, true);
    program.set_float_array("u_amp_x", amp_x, kMaxRegions, false);
    program.set_float_array("u_amp_y", amp_y, kMaxRegions, false);

    const Texture& mask = mask_texture ? *mask_texture : inside_all;
    gl::ActiveTexture(GL_TEXTURE0 + kMaskUnit);
    glBindTexture(GL_TEXTURE_2D, mask.id());
    gl::ActiveTexture(GL_TEXTURE0 + kLayerUnit);
    program.set_int("u_mask", kMaskUnit);
    program.set_int("u_texture", kLayerUnit);
    program.set_float("u_feather", feather);
    // u_pma 是"按预乘输出"的开关：0 时片元写 c.rgb*c.a，1 时写直通的 c。
    // 纹理来自 PNG/解码后的直通 RGBA，而混合函数是 GL_SRC_ALPHA/GL_ONE_MINUS_SRC_ALPHA
    // （再乘一次 alpha）。如果这里传 0，alpha 会被乘两遍：边缘像素变成 c.rgb*a²，
    // FBO 再被 unpremultiply 还原成 c.rgb*a —— 浅底上一圈黑边、矩形抠图上一圈黑框。
    // 序列到"直通输出 + 直通混合"，FBO 的合成结果才是正确的预乘 RGB。
    program.set_float("u_pma", 1.0f);
}

}  // namespace bl
