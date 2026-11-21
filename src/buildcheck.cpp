#include "buildcheck.h"

#include <GLFW/glfw3.h>
#include <vips/vips.h>

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "config.h"
#include "domain/layers.h"
#include "domain/region.h"
#include "domain/seamless.h"
#include "io/paths.h"
#include "render/assets.h"
#include "render/exporter.h"
#include "render/framing.h"
#include "render/gl.h"
#include "render/mask.h"

namespace {

FILE* g_report = nullptr;

void Log(const char* fmt, ...) {
    if (g_report == nullptr) return;
    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_report, fmt, args);
    va_end(args);
    std::fputc('\n', g_report);
    std::fflush(g_report);
}

// ---- 离屏渲染 ----

bool RenderOnce(int width, int height, std::vector<unsigned char>* rgba) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);

    GLuint fbo = 0;
    gl::GenFramebuffers(1, &fbo);
    gl::BindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                             tex, 0);
    if (gl::CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        Log("FAIL framebuffer incomplete");
        return false;
    }

    glViewport(0, 0, width, height);
    glClearColor(0.25f, 0.5f, 0.75f, 0.5f);
    glClear(GL_COLOR_BUFFER_BIT);

    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    // GL 的行序是自下而上，导出链要自上而下：这里翻一次，后续代码不必再翻。
    rgba->assign(pixels.size(), 0);
    const size_t stride = static_cast<size_t>(width) * 4;
    for (int y = 0; y < height; ++y) {
        const unsigned char* src = pixels.data() + stride * (height - 1 - y);
        unsigned char* dst = rgba->data() + stride * y;
        for (size_t i = 0; i < stride; ++i) dst[i] = src[i];
    }

    gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl::DeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);

    // 清屏色 0.25/0.5/0.75/0.5 → 64/128/191/128（±1 取整误差）
    const unsigned char* p = rgba->data();
    bool ok = std::abs(static_cast<int>(p[0]) - 64) <= 1 &&
              std::abs(static_cast<int>(p[1]) - 128) <= 1 &&
              std::abs(static_cast<int>(p[2]) - 191) <= 1 &&
              std::abs(static_cast<int>(p[3]) - 128) <= 1;
    Log("clear readback: %d %d %d %d -> %s", p[0], p[1], p[2], p[3],
        ok ? "ok" : "FAIL");
    return ok;
}

// ---- 编码 ----

bool EncodeWebpAnimation(const std::vector<unsigned char>& frames, int w, int h,
                         int frame_count, const std::string& out_path) {
    VipsImage* img = vips_image_new_from_memory_copy(
        frames.data(), frames.size(), w, h * frame_count, 4, VIPS_FORMAT_UCHAR);
    if (img == nullptr) {
        Log("FAIL vips_image_new_from_memory_copy");
        return false;
    }
    vips_image_set_int(img, VIPS_META_PAGE_HEIGHT, h);
    vips_image_set_int(img, VIPS_META_N_PAGES, frame_count);
    vips_image_set_int(img, "loop", 0);
    int delays[8] = {};
    for (int i = 0; i < frame_count && i < 8; ++i) delays[i] = 33;
    vips_image_set_array_int(img, "delay", delays, frame_count);

    int rc = vips_webpsave(img, out_path.c_str(),
                           "page-height", h,
                           "lossless", TRUE,
                           "Q", 100,
                           "alpha-q", 100,
                           "effort", 0,
                           "smart-subsample", FALSE,
                           "kmax", 10,
                           "kmin", 5,
                           "keep", 0,
                           NULL);
    g_object_unref(img);
    if (rc != 0) {
        Log("FAIL vips_webpsave");
        return false;
    }
    return true;
}

bool EncodePng(const std::vector<unsigned char>& rgba, int w, int h,
               const std::string& out_path) {
    VipsImage* img = vips_image_new_from_memory_copy(
        rgba.data(), rgba.size(), w, h, 4, VIPS_FORMAT_UCHAR);
    if (img == nullptr) {
        Log("FAIL vips_image_new_from_memory_copy (png)");
        return false;
    }
    int rc = vips_pngsave(img, out_path.c_str(),
                          "compression", 6,
                          "bitdepth", 8,
                          "filter", VIPS_FOREIGN_PNG_FILTER_NONE,
                          NULL);
    g_object_unref(img);
    if (rc != 0) {
        Log("FAIL vips_pngsave");
        return false;
    }
    return true;
}

bool CheckWritten(const std::string& path) {
    VipsImage* img = vips_image_new_from_file(path.c_str(), "access", VIPS_ACCESS_SEQUENTIAL, NULL);
    if (img == nullptr) {
        Log("FAIL reopen %s", path.c_str());
        return false;
    }
    int pages = vips_image_get_n_pages(img);
    int page_h = vips_image_get_page_height(img);
    Log("reopen %s: %dx%d pages=%d page_height=%d", path.c_str(), img->Xsize,
        img->Ysize, pages, page_h);
    g_object_unref(img);
    return true;
}

// ---- domain 数值对照 ----
//
// 与 tools/gen_golden.py 用同一组输入；两边输出同名的 key=value，
// 逐位比对就是"物理与坐标契约等价"的证据。
void DomainParity() {
    using bl::Document;
    using bl::DispTable;
    using bl::FrameDisp;
    using bl::LoopPlan;
    using bl::Region;

    struct Spec {
        double amp, speed, ax, ay, zeta, freq;
    };
    const Spec specs[3] = {{10.0, 20.0, 0.3, 1.0, 0.15, 2.5},
                           {20.0, 12.0, 0.8, 1.0, 0.25, 1.8},
                           {30.0, 8.0, 0.5, 0.5, 0.4, 4.0}};

    std::vector<std::unique_ptr<Region>> owners;
    std::vector<Region*> regions;
    std::vector<double> speeds;
    for (const Spec& spec : specs) {
        std::unique_ptr<Region> region =
            std::make_unique<Region>(Region::make_default());
        region->amp = spec.amp;
        region->speed = spec.speed;
        region->aniso_x = spec.ax;
        region->aniso_y = spec.ay;
        region->zeta = spec.zeta;
        region->freq = spec.freq;
        speeds.push_back(spec.speed);
        regions.push_back(region.get());
        owners.push_back(std::move(region));
    }

    Log("common_period=%.17g", bl::common_period(speeds));
    const LoopPlan plan = bl::plan_loop(60, 33.3, speeds);
    Log("plan.n=%d", plan.n);
    Log("plan.per_cycle=%d", plan.per_cycle);
    Log("plan.period=%.17g", plan.period);
    Log("plan.duration=%.17g", plan.duration);

    const DispTable table = bl::build_table(regions, plan.period);
    Log("table.frames=%d", static_cast<int>(table.size()));

    const double probes[3] = {0.5, 3.2, 4.9};
    for (double t : probes) {
        const FrameDisp disp = bl::sample_table(table, plan.period, t);
        for (size_t i = 0; i < disp.size(); ++i) {
            Log("sample.%.1f.%d.dx=%.17g", t, static_cast<int>(i), disp[i].first);
            Log("sample.%.1f.%d.dy=%.17g", t, static_cast<int>(i), disp[i].second);
        }
    }

    std::unique_ptr<Document> doc = std::make_unique<Document>();
    std::unique_ptr<bl::Layer> layer =
        doc->new_layer("a.png", "a.png", 1, 100, 200, 50.0, 60.0);
    layer->scale = 1.5;
    bl::Layer* root = doc->add_layer_top(std::move(layer));
    const std::pair<double, double> world = root->to_world(10.0, 20.0);
    Log("layer.world.x=%.17g", world.first);
    Log("layer.world.y=%.17g", world.second);
    const std::pair<double, double> local = root->from_world(world.first, world.second);
    Log("layer.local.x=%.17g", local.first);
    Log("layer.local.y=%.17g", local.second);

    std::unique_ptr<bl::Layer> child =
        doc->new_layer("b.png", "b.png", 2, 40, 80, 120.0, 90.0);
    child->parent = root;
    root->children.push_back(std::move(child));
    bl::Layer* kid = root->children.back().get();
    const std::pair<double, double> mapped = kid->map_point_from(*root, 10.0, 20.0);
    Log("map.x=%.17g", mapped.first);
    Log("map.y=%.17g", mapped.second);
    Log("len_factor=%.17g", kid->len_factor_from(*root));
}

// ---- 遮罩对照 ----
//
// 与 tools/gen_mask_golden.py 同一组输入；两边都吐出同名二进制与同名 key，
// 逐字节比就是"遮罩几何等价"的证据。
void WriteBytes(const std::string& path, const std::vector<unsigned char>& data) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        Log("FAIL open %s", path.c_str());
        return;
    }
    if (!data.empty()) std::fwrite(data.data(), 1, data.size(), file);
    std::fclose(file);
}

void MaskParity() {
    using bl::Lasso;
    using bl::Point;

    struct Case {
        const char* name;
        int w, h;
        double detail;
        std::vector<Point> points;
    };
    const std::vector<Case> cases = {
        {"a", 40, 28, 2.0,
         {{-5.0, -3.0}, {30.0, 2.0}, {38.0, 20.0}, {15.0, 34.0}, {2.0, 10.0}}},
        {"b", 96, 64, 1.0,
         {{10.0, 8.0}, {80.0, 14.0}, {70.0, 55.0}, {20.0, 48.0}}},
        {"c", 96, 64, 1.0, {{1.0, 1.0}, {2.0, 2.0}}},
    };

    for (const Case& item : cases) {
        Lasso lasso;
        lasso.points = item.points;
        const bl::Mask field = bl::sdf(lasso, item.w, item.h, item.detail);
        const std::vector<unsigned char> cov =
            bl::coverage(lasso, item.w, item.h);
        Log("mask.%s.size=%d %d", item.name, field.width, field.height);
        Log("mask.%s.scale=%.17g", item.name, field.scale);
        Log("mask.%s.feather=%.17g", item.name,
            field.feather(cfg::LASSO_FEATHER));
        WriteBytes(paths::BesideExe(std::string("mask_") + item.name + "_cpp.bin"),
                   field.data);
        WriteBytes(
            paths::BesideExe(std::string("coverage_") + item.name + "_cpp.bin"),
            cov);
    }
}

}  // namespace

// 诊断用：把一张图 1:1 画进 FBO 并存成 PNG，排除纹理/四边形/行序的问题。
int RunDumpFrame(const std::string& asset_path, const std::string& out_png) {
    if (vips_init("BreathLoop") != 0) {
        std::fprintf(stderr, "vips_init failed\n");
    }
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    GLFWwindow* window = glfwCreateWindow(64, 64, "dumpframe", nullptr, nullptr);
    if (window == nullptr) return 1;
    glfwMakeContextCurrent(window);
    if (!gl::LoadAll()) return 1;

    int status = 0;
    try {
        bl::AssetStore assets;
        bl::Asset& asset = assets.load(asset_path);

        // 解码结果原样存一份，先看纹理源数据对不对
        std::vector<unsigned char> straight = asset.rgba();
        VipsImage* source = vips_image_new_from_memory_copy(
            straight.data(), straight.size(), asset.width, asset.height, 4,
            VIPS_FORMAT_UCHAR);
        vips_pngsave(source, (out_png + ".src.png").c_str(), nullptr);
        g_object_unref(source);

        bl::Framebuffer target;
        if (!target.create(asset.width, asset.height)) {
            throw std::runtime_error("FBO 创建失败");
        }
        std::string error;
        if (!bl::BreathStyle::init_program(&error)) {
            throw std::runtime_error("着色器编译失败: " + error);
        }
        bl::BreathStyle style;
        style.img_w = static_cast<double>(asset.width);
        style.img_h = static_cast<double>(asset.height);
        style.deform = false;

        bl::Renderer renderer;
        target.bind();
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        renderer.begin(asset.width, asset.height);
        glEnable(GL_BLEND);
        gl::BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                              GL_ONE_MINUS_SRC_ALPHA);
        bl::Program& program = bl::BreathStyle::program();
        program.use();
        style.bind(program, bl::inside_all_mask());
        renderer.draw_textured(program, *asset.texture, bl::kLayerUnit, 0.0, 0.0,
                               asset.width, asset.height, 0.0, 0.0, 1.0, 1.0);
        glDisable(GL_BLEND);
        const std::vector<unsigned char> raw = target.read_rgba();
        target.unbind();

        VipsImage* frame = vips_image_new_from_memory_copy(
            raw.data(), raw.size(), asset.width, asset.height, 4,
            VIPS_FORMAT_UCHAR);
        if (vips_pngsave(frame, out_png.c_str(), nullptr)) {
            throw std::runtime_error(vips_error_buffer());
        }
        g_object_unref(frame);
        std::printf("dumped %s\n", out_png.c_str());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "dump failed: %s\n", error.what());
        status = 1;
    }
    glfwDestroyWindow(window);
    glfwTerminate();
    return status;
}

int RunExportCheck(const std::string& asset_path, const std::string& out_base) {    if (vips_init("BreathLoop") != 0) {
        std::fprintf(stderr, "vips_init failed\n");
    }
    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    GLFWwindow* window = glfwCreateWindow(64, 64, "exportcheck", nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    if (!gl::LoadAll()) {
        std::fprintf(stderr, "gl::LoadAll failed: %s\n", gl::LastError());
        glfwTerminate();
        return 1;
    }

    int status = 0;
    try {
        bl::AssetStore assets;
        bl::Document doc;
        bl::Asset& asset = assets.load(asset_path);
        std::unique_ptr<bl::Layer> layer =
            doc.new_layer(asset.name, asset.path, asset.key, asset.width,
                          asset.height, 200.0, 150.0);
        layer->scale = 1.2;
        bl::Layer* root = doc.add_layer_top(std::move(layer));

        struct Spec {
            double cx, cy, rx, ry, rot;
            double amp, speed, ax, ay, zeta, freq;
        };
        const Spec specs[2] = {
            {45.0, 70.0, 30.0, 40.0, 15.0, 14.0, 100.0, 0.35, 1.0, 0.2, 2.5},
            {85.0, 95.0, 22.0, 18.0, -30.0, 8.0, 50.0, 0.8, 0.6, 0.35, 3.5},
        };
        for (const Spec& spec : specs) {
            // Python 侧是 y 向下的图像坐标，本工程的图层局部坐标是 y 向上：
            // 纵向位置取 h-cy、旋转取反，同一份数值才能复现同一张画面。
            const double cy = asset.height - spec.cy;
            bl::Region* region = doc.add_bbox(root, spec.cx, cy);
            region->rx = spec.rx;
            region->ry = spec.ry;
            region->rot = -spec.rot;
            region->amp = spec.amp;
            region->speed = spec.speed;
            region->aniso_x = spec.ax;
            region->aniso_y = spec.ay;
            region->zeta = spec.zeta;
            region->freq = spec.freq;
        }
        // 套索不参与导出（它是 UI 上的选区），这里不需要挂任何套索

        bl::ExportOptions options;
        options.out_w = 128;
        options.out_h = 0;
        options.lossless = true;
        options.quality = 100;
        options.alpha_q = 100;
        options.effort = 0;
        options.ms_per_frame = 33.3;
        options.out_base = out_base;
        options.debug_dump_frame = out_base + ".raw.png";

        const bl::ContentFrame content = bl::content_frame(doc, assets);
        std::printf("asset content: %d %d %d %d\n", asset.content_x,
                    asset.content_y, asset.content_w, asset.content_h);
        std::printf("content x=%f\n", content.x);
        std::printf("content y=%f\n", content.y);
        std::printf("content w=%f\n", content.w);
        std::printf("content h=%f\n", content.h);

        // 导出范围已经改成画布边界：对拍用内容框当画布，保持既有基准不变。
        doc.canvas_w = std::max(1, static_cast<int>(std::ceil(content.w)));
        doc.canvas_h = std::max(1, static_cast<int>(std::ceil(content.h)));

        bl::Exporter exporter(doc, assets, 12, options);
        const bl::ExportResult result = exporter.run();
        std::printf("cpp export: %s\n", result.path.c_str());
        std::printf("out=%dx%d ss=%d n=%d\n", result.width, result.height,
                    exporter.render_supersample(), result.frames);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "export failed: %s\n", error.what());
        status = 1;
    }

    glfwDestroyWindow(window);
    glfwTerminate();
    return status;
}

int RunBuildCheck() {
    g_report = std::fopen(paths::BesideExe("breathloop_buildcheck.log").c_str(), "w");
    Log("== BreathLoop build check ==");
    Log("exe: %s", paths::ExePath().c_str());
    Log("vips: %s", vips_version_string());
    DomainParity();
    MaskParity();

    glfwSetErrorCallback(nullptr);
    if (!glfwInit()) {
        Log("FAIL glfwInit");
        return 1;
    }
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    GLFWwindow* window = glfwCreateWindow(320, 240, "buildcheck", nullptr, nullptr);
    if (window == nullptr) {
        Log("FAIL glfwCreateWindow (3.3 core)");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    if (!gl::LoadAll()) {
        Log("FAIL gl::LoadAll, missing %s", gl::LastError());
        return 1;
    }
    Log("GL_RENDERER: %s", reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    Log("GL_VERSION : %s", reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    Log("GLSL       : %s",
        reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION)));

    int failures = 0;
    const int w = 64;
    const int h = 64;
    std::vector<unsigned char> rgba;
    if (!RenderOnce(w, h, &rgba)) ++failures;

    if (!vips_init(paths::ExePath().c_str())) {
        // 载入格式清单：和 Python 那头 pyvips.get_suffixes() 对表用。
        char** suffixes = vips_foreign_get_suffixes();
        int suffix_count = 0;
        std::string suffix_list;
        for (char** p = suffixes; p != nullptr && *p != nullptr; ++p) {
            suffix_list += *p;
            suffix_list += ' ';
            ++suffix_count;
        }
        g_strfreev(suffixes);
        Log("loaders (%d): %s", suffix_count, suffix_list.c_str());
        std::vector<unsigned char> frames;
        frames.reserve(rgba.size() * 2);
        frames.insert(frames.end(), rgba.begin(), rgba.end());
        // 第二帧换个颜色：相邻且完全相同的帧会被 libwebp 合并成一个 ANMF，
        // 那样就验不出分页有没有写对。
        std::vector<unsigned char> second = rgba;
        for (size_t i = 0; i < second.size(); i += 4) second[i] = 200;
        frames.insert(frames.end(), second.begin(), second.end());
        std::string webp = paths::BesideExe("buildcheck_anim.webp");
        std::string png = paths::BesideExe("buildcheck_frame.png");
        if (!EncodeWebpAnimation(frames, w, h, 2, webp)) ++failures;
        if (!CheckWritten(webp)) ++failures;
        if (!EncodePng(rgba, w, h, png)) ++failures;
        if (!CheckWritten(png)) ++failures;
    } else {
        Log("FAIL vips_init");
        ++failures;
    }

    glfwDestroyWindow(window);
    glfwTerminate();

    if (failures == 0) {
        Log("RESULT: ok");
        if (g_report) std::fclose(g_report);
        g_report = nullptr;
        return 0;
    }
    Log("RESULT: %d failure(s)", failures);
    if (g_report) std::fclose(g_report);
    g_report = nullptr;
    return 2;
}
