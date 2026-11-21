// 把整棵图层树合成成 WebP 动画。
//
// 导出与预览共用 Document + BreathStyle + 遮罩派生 + 无缝位移表；这里只换目标：
// 一个 FBO、导出瞬间可见内容（非透明像素）的并集等比装进输出尺寸，逐帧读像素，
// 最后交给 libwebp 编码。并集保证内容完整不裁切，也保证输出画布里不会被透明边
// 撑出大片空白。
//
// 缩小导出时先按输出尺寸的整数倍超采样渲染，再用 lanczos3 缩到目标尺寸：
// OpenGL 直接缩采样的锯齿/摩尔纹太明显。
#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "domain/layers.h"
#include "render/assets.h"
#include "render/gfx.h"
#include "render/shaders.h"

namespace bl {

constexpr int kExportSubsteps = 2;
constexpr int kExportKmax = 10;
constexpr int kExportKmin = 5;
// 超采样上限：保证缩小导出不锯齿，同时限制单帧 FBO 与内存
constexpr int kExportSupersampleMax = 4;
constexpr long long kExportRenderMaxPixels = 16000000;

struct ExportOptions {
    int out_w = 0;  // 0 = 按内容自动
    int out_h = 0;
    bool lossless = true;
    int quality = 100;
    int effort = 0;
    int alpha_q = 100;
    double ms_per_frame = 33.3;
    std::string out_base;      // 空 = "output"；扩展名会被去掉
    std::string debug_dump_frame;  // 非空：把第 0 帧的渲染缓冲原样存成 PNG（诊断用）
};

struct ExportResult {
    std::string path;
    int frames = 0;
    int width = 0;
    int height = 0;
    // 写盘后从字节里读回的真实结构（libwebp 会合并相邻相同帧，帧数可能与
    // frames 不同；duration_ms 是文件里所有 ANMF 时长之和）。
    int file_frames = 0;
    int duration_ms = 0;
    int period_ms = 0;
    int frame_ms = 0;  // 实际每帧毫秒（被自动降帧时与面板 fps 不一致）
    bool frame_capped = false;  // true = 帧数被像素预算压低过
};

// 导出分两步走：`step()` 一次渲染一帧（必须在 GL 上下文当前的主线程上调用），
// 全部帧渲染完后 `finish()` 才编码写盘（只碰内存里的帧数据和 libvips，可以在
// 后台线程跑）。`run()` 是两者的同步封装，给命令行自检用。
class Exporter {
public:
    Exporter(const Document& doc, AssetStore& assets, int n_frames,
             const ExportOptions& options);

    ExportResult run();

    // 渲染下一帧；已经没有帧可渲染时返回 false。
    bool step();
    int done_frames() const { return frame_index_; }
    int total_frames() const { return n_; }
    // 所有帧都已渲染后编码并写盘。失败抛 std::runtime_error。
    ExportResult finish();

    // 输出尺寸与装填关系（导出卡片的自动尺寸与预览共用这个口径）
    int out_width() const { return out_w_; }
    int out_height() const { return out_h_; }
    int frame_count() const { return n_; }
    // 没有可见 BBOX：画面不随时间变，导出只有 1 帧（静态图）
    bool is_static() const { return static_frame_; }
    int render_supersample() const { return render_ss_; }

    // 编码阶段的真实进度（逐帧编码，后台线程里推进；主线程读这两个数）
    int encode_done() const { return encode_done_.load(); }
    int encode_total() const { return encode_total_.load(); }

private:
    void draw_scene();
    void render_frame(std::vector<unsigned char>* rgba);
    void save_webp(std::vector<std::vector<unsigned char>>& frames,
                   const std::vector<int>& delays, const std::string& path);

    std::unique_ptr<Document> doc_;
    AssetStore* assets_ = nullptr;
    ExportOptions options_;

    double frame_x_ = 0.0;
    double frame_y_ = 0.0;
    double frame_w_ = 1.0;
    double frame_h_ = 1.0;

    int out_w_ = 1;
    int out_h_ = 1;
    double scale_ = 1.0;
    double offset_x_ = 0.0;
    double offset_y_ = 0.0;

    int render_ss_ = 1;
    int render_w_ = 1;
    int render_h_ = 1;
    double render_scale_ = 1.0;
    double render_offset_x_ = 0.0;
    double render_offset_y_ = 0.0;

    int n_ = 0;
    int frame_index_ = 0;   // step() 已经渲染完几帧
    int file_frames_ = 0;   // 写盘后文件里的 ANMF 个数
    int file_duration_ms_ = 0;  // 文件里所有 ANMF 时长之和
    bool frame_capped_ = false;
    bool static_frame_ = false;   // 没有可见 BBOX → 只渲 1 帧，导出即静态图
    // 编码进度：save_webp 里每喂一帧 WebPAnimEncoderAdd 就加一
    std::atomic<int> encode_done_{0};
    std::atomic<int> encode_total_{0};
    bool program_ready_ = false;
    std::vector<std::vector<unsigned char>> frames_;
    std::vector<Region*> regions_;
    double dt_ = 0.0;
    std::vector<std::vector<std::pair<double, double>>> table_;

    // 场景：每个可见图层一个节点，坐标已换算到渲染像素
    struct Node {
        Layer* layer = nullptr;
        Asset* asset = nullptr;
        double x = 0.0;
        double y = 0.0;
        double scale = 1.0;
        double scale_x = 1.0;
        double scale_y = 1.0;
        double rot = 0.0;
        BreathStyle style;
    };
    std::vector<Node> nodes_;

    Renderer renderer_;
    Framebuffer target_;
};

}  // namespace bl
