#include "render/exporter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <stdexcept>

#include "vips/vips.h"
#include "webp/encode.h"
#include "webp/mux.h"

#include "config.h"
#include "domain/seamless.h"
#include "io/blob.h"
#include "io/paths.h"
#include "render/framing.h"
#include "render/gfx.h"
#include "render/mask.h"
#include "render/shaders.h"

namespace bl {
namespace {

// 补掉 8 位预乘往返留下的"垃圾色"：预乘数据里 alpha=1 的像素 RGB 只剩 0/1，
// 再 unpremultiply（除以 0.004）会把量化误差放大成纯红/纯黄/纯黑/纯白。
// 这些点 alpha 极小，正常合成看不见，但只要查看器忽略 alpha、或做别的合成，
// 轮廓上就是一排黑灰色噪点。做法是只修 0<alpha<min_alpha 的那一圈：
// 用"alpha>=min_alpha 的邻居"的颜色逐环向内/向外铺，离内容远的透明区仍保持黑。
void RepairLowAlphaRgb(std::vector<unsigned char>& rgba, int w, int h,
                       int min_alpha) {
    if (w <= 0 || h <= 0) return;
    const size_t count = static_cast<size_t>(w) * h;
    std::vector<unsigned char> filled(count, 0);
    for (size_t i = 0; i < count; ++i) {
        if (rgba[i * 4 + 3] >= min_alpha) filled[i] = 1;
    }
    const int nx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    const int ny[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    // 半径 3 足够盖住降采样产生的 1~2 像素晕圈。
    for (int pass = 0; pass < 3; ++pass) {
        bool changed = false;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = static_cast<size_t>(y) * w + x;
                const int alpha = rgba[i * 4 + 3];
                if (alpha == 0 || filled[i] != 0) continue;
                int sum[3] = {0, 0, 0};
                int hits = 0;
                for (int k = 0; k < 8; ++k) {
                    const int qx = x + nx[k];
                    const int qy = y + ny[k];
                    if (qx < 0 || qy < 0 || qx >= w || qy >= h) continue;
                    const size_t q = static_cast<size_t>(qy) * w + qx;
                    if (filled[q] == 0) continue;
                    sum[0] += rgba[q * 4 + 0];
                    sum[1] += rgba[q * 4 + 1];
                    sum[2] += rgba[q * 4 + 2];
                    ++hits;
                }
                if (hits == 0) continue;
                rgba[i * 4 + 0] = static_cast<unsigned char>(sum[0] / hits);
                rgba[i * 4 + 1] = static_cast<unsigned char>(sum[1] / hits);
                rgba[i * 4 + 2] = static_cast<unsigned char>(sum[2] / hits);
                filled[i] = 1;
                changed = true;
            }
        }
        if (!changed) break;
    }
}

// 每帧毫秒数按累计取整分配，保证整段循环时长不被逐帧舍入拉偏。
std::vector<int> FrameDelays(double dt, int n) {
    const double ms = dt * 1000.0;
    std::vector<int> delays;
    delays.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const int delta = static_cast<int>(std::rint((i + 1) * ms)) -
                          static_cast<int>(std::rint(i * ms));
        delays.push_back(std::max(1, delta));
    }
    return delays;
}

// 写盘后的自检：从 WebP 字节里读回 ANMF 个数与时长之和。libwebp 会把相邻且
// 画面相同的帧合并成一个 ANMF（时长相加），所以“渲染了多少帧”不等于“文件里
// 有多少帧”；只有时长之和才代表播放一遍的真实长度，用它才能证明整段循环完整。
struct AnimStats {
    int frames = 0;
    int duration_ms = 0;
    int loop = -1;
};

AnimStats ReadAnimStats(const std::vector<unsigned char>& data) {
    AnimStats stats;
    if (data.size() < 12) return stats;
    size_t pos = 12;
    while (pos + 8 <= data.size()) {
        const unsigned int size =
            static_cast<unsigned int>(data[pos + 4]) |
            (static_cast<unsigned int>(data[pos + 5]) << 8) |
            (static_cast<unsigned int>(data[pos + 6]) << 16) |
            (static_cast<unsigned int>(data[pos + 7]) << 24);
        const size_t body = pos + 8;
        const std::string fourcc(
            reinterpret_cast<const char*>(data.data() + pos), 4);
        if (fourcc == "ANIM" && body + 6 <= data.size()) {
            stats.loop = static_cast<int>(data[body + 4]) |
                         (static_cast<int>(data[body + 5]) << 8);
        } else if (fourcc == "ANMF" && body + 15 <= data.size()) {
            stats.duration_ms += static_cast<int>(data[body + 12]) |
                                 (static_cast<int>(data[body + 13]) << 8) |
                                 (static_cast<int>(data[body + 14]) << 16);
            ++stats.frames;
        }
        pos += 8 + size + (size & 1);
    }
    return stats;
}

// 输出名：<base>_breath.webp，撞名就 <base>_breath_1.webp …
// 路径按 UTF-8 处理，中文目录名也能正确判断"是否已存在"。
std::string UniquePath(const std::string& base) {
    const std::string suffix = "webp";
    const std::filesystem::path base_path =
        paths::Utf8Path(base.empty() ? std::string("output") : base);
    const std::filesystem::path dir = base_path.parent_path();
    const std::string stem = paths::Utf8String(base_path.stem());
    auto join = [&dir, &stem](const std::string& tail) -> std::string {
        const std::filesystem::path name = paths::Utf8Path(stem + tail);
        const std::filesystem::path full = dir.empty() ? name : dir / name;
        return paths::Utf8String(full);
    };
    std::string path = join("_breath." + suffix);
    int k = 1;
    while (std::filesystem::exists(paths::Utf8Path(path))) {
        path = join("_breath_" + std::to_string(k) + "." + suffix);
        ++k;
    }
    return path;
}

}  // namespace

Exporter::Exporter(const Document& doc, AssetStore& assets_store, int n_frames,
                   const ExportOptions& options)
    : doc_(doc.clone()), assets_(&assets_store), options_(options) {
    // 导出范围就是画布边界：世界原点在画布中心。
    const Rect canvas = doc_->canvas_rect();
    frame_x_ = canvas.x0;
    frame_y_ = canvas.y0;
    frame_w_ = std::max(1e-6, static_cast<double>(doc_->canvas_w));
    frame_h_ = std::max(1e-6, static_cast<double>(doc_->canvas_h));

    out_w_ = std::max(1, doc_->canvas_w);
    out_h_ = std::max(1, doc_->canvas_h);
    scale_ = std::min(out_w_ / frame_w_, out_h_ / frame_h_);
    offset_x_ = (out_w_ - frame_w_ * scale_) / 2.0;
    offset_y_ = (out_h_ - frame_h_ * scale_) / 2.0;

    // 只在缩小导出时超采样：1 倍渲染就是输出尺寸，放大没有收益。
    render_ss_ = 1;
    if (scale_ < 1.0) {
        render_ss_ = std::max(1, static_cast<int>(std::ceil(1.0 / scale_)));
        const int max_ss = static_cast<int>(std::sqrt(
            static_cast<double>(kExportRenderMaxPixels) /
            static_cast<double>(std::max(1, out_w_ * out_h_))));
        render_ss_ =
            std::max(1, std::min({render_ss_, kExportSupersampleMax, max_ss}));
    }
    render_w_ = out_w_ * render_ss_;
    render_h_ = out_h_ * render_ss_;
    render_scale_ = scale_ * render_ss_;
    render_offset_x_ = offset_x_ * render_ss_;
    render_offset_y_ = offset_y_ * render_ss_;

    // 导出只认"眼睛开着且图层可见"的 BBOX：和预览同一份过滤（visible_regions）
    regions_ = doc_->visible_regions();
    // 一个都没有 → 位移恒为 0，画面不随时间变：只渲 1 帧，导出就是静态图。
    // （以前照样按 2 秒周期铺满帧，出来的是一段"什么都没动"的动画。）
    static_frame_ = regions_.empty();
    std::vector<double> speeds;
    speeds.reserve(regions_.size());
    for (const Region* region : regions_) speeds.push_back(region->speed);
    // 帧数上限按 Python 版的口径：一个周期最多 300 帧。超了就降到 300 帧，
    // 每帧时长由 period/n 摊出来（循环仍精确等于一整个周期），不直接拒绝导出。
    LoopPlan plan = plan_loop(n_frames, options_.ms_per_frame, speeds);
    const double period = plan.ok ? plan.period : common_period(speeds);
    if (static_frame_) {
        plan.ok = true;
        plan.n = 1;
        plan.per_cycle = 1;
        plan.k = 1;
        plan.period = 0.0;
        plan.duration = 0.0;
        plan.ms = std::max(1e-6, options_.ms_per_frame);
        plan.changed_n = true;
        plan.changed_ms = false;
        plan.hint.clear();
    } else if (!plan.ok || plan.n > cfg::FRAMES_MAX) {
        frame_capped_ = true;
        const int cap = std::max(2, static_cast<int>(cfg::FRAMES_MAX));
        plan.ok = true;
        plan.n = cap;
        plan.per_cycle = cap;
        plan.period = period;
        plan.k = 1;
        plan.ms = period * 1000.0 / static_cast<double>(cap);
        plan.duration = period;
        plan.changed_ms = true;
        plan.changed_n = cap != n_frames;
        plan.hint.clear();
    }
    if (static_frame_) {
        // 没有 BBOX：不建位移表，一帧、零位移（period=0 也不能送进 sample_table）
        table_.assign(1, FrameDisp{});
        dt_ = plan.ms / 1000.0;
        n_ = 1;
    } else {
        const std::pair<DispTable, double> animation = build_animation(regions_, plan);
        table_ = animation.first;
        dt_ = animation.second;
        n_ = static_cast<int>(table_.size());
    }
    frames_.reserve(static_cast<size_t>(n_));

    for (Layer* layer : doc_->flatten()) {
        if (!layer->is_visible_chain()) continue;
        Asset& asset = assets_->get(layer->asset_key);
        Node node;
        node.layer = layer;
        node.asset = &asset;
        node.x = (layer->x - frame_x_) * render_scale_ + render_offset_x_;
        node.y = (layer->y - frame_y_) * render_scale_ + render_offset_y_;
        node.scale = layer->scale * render_scale_;
        node.scale_x = layer->scale_x() * render_scale_;
        node.scale_y = layer->scale_y() * render_scale_;
        node.rot = layer->rot;
        node.style.views = layer->bbox_views();
        node.style.img_w = static_cast<double>(layer->w);
        node.style.img_h = static_cast<double>(layer->h);
        nodes_.push_back(std::move(node));
    }
}

void Exporter::draw_scene() {
    renderer_.begin(render_w_, render_h_);
    Program& program = BreathStyle::program();
    const Texture& inside_all = inside_all_mask();

    glEnable(GL_BLEND);
    gl::BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                          GL_ONE_MINUS_SRC_ALPHA);
    for (Node& node : nodes_) {
        program.use();
        node.style.bind(program, inside_all);
        const double w = static_cast<double>(node.layer->w);
        const double h = static_cast<double>(node.layer->h);
        const double pivot_x = w / 2.0;
        const double pivot_y = h / 2.0;
        const double hw = pivot_x * node.scale_x;
        const double hh = pivot_y * node.scale_y;
        const double c = std::cos(node.rot);
        const double s = std::sin(node.rot);
        const double lx[4] = {-hw, hw, hw, -hw};
        const double ly[4] = {hh, hh, -hh, -hh};   // uv(0,0) 在图像顶行 = y 的 +hh
        double quad[8];
        for (int i = 0; i < 4; ++i) {
            quad[i * 2] = node.x + lx[i] * c - ly[i] * s;
            quad[i * 2 + 1] = node.y + lx[i] * s + ly[i] * c;
        }
        renderer_.draw_quad(program, *node.asset->texture, kLayerUnit, quad);
    }
    glDisable(GL_BLEND);
}

void Exporter::render_frame(std::vector<unsigned char>* rgba) {
    if (target_.width() != render_w_ || target_.height() != render_h_) {
        if (!target_.create(render_w_, render_h_)) {
            throw std::runtime_error("FBO 创建失败");
        }
    }
    target_.bind();
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_scene();
    *rgba = target_.read_rgba();
    target_.unbind();
}

ExportResult Exporter::run() {
    while (step()) {
    }
    return finish();
}

bool Exporter::step() {
    if (frame_index_ >= n_) return false;
    if (!program_ready_) {
        if (!BreathStyle::init_program(nullptr)) {
            throw std::runtime_error("呼吸着色器编译失败");
        }
        program_ready_ = true;
    }

    const int i = frame_index_;
    const FrameDisp& disp = table_[static_cast<size_t>(i)];
    for (size_t k = 0; k < regions_.size() && k < disp.size(); ++k) {
        regions_[k]->disp_x = disp[k].first;
        regions_[k]->disp_y = disp[k].second;
    }
    std::vector<unsigned char> raw;
    render_frame(&raw);

    if (i == 0 && !options_.debug_dump_frame.empty()) {
        VipsImage* debug = vips_image_new_from_memory_copy(
            raw.data(), raw.size(), render_w_, render_h_, 4, VIPS_FORMAT_UCHAR);
        vips_pngsave(debug, options_.debug_dump_frame.c_str(), nullptr);
        g_object_unref(debug);
    }

    VipsImage* frame = vips_image_new_from_memory_copy(
        raw.data(), raw.size(), render_w_, render_h_, 4, VIPS_FORMAT_UCHAR);
    if (frame == nullptr) throw std::runtime_error("帧缓冲建图失败");
    VipsImage* scaled = frame;
    if (render_ss_ != 1) {
        // 先按预乘 alpha 降采样，再还原直通 alpha 交给 WebP
        VipsImage* resized = nullptr;
        const double factor = 1.0 / static_cast<double>(render_ss_);
        if (vips_resize(frame, &resized, factor, "vscale", factor, "kernel",
                        VIPS_KERNEL_LANCZOS3, nullptr)) {
            g_object_unref(frame);
            throw std::runtime_error(vips_error_buffer());
        }
        g_object_unref(frame);
        scaled = resized;
    }
    VipsImage* straight = nullptr;
    if (vips_unpremultiply(scaled, &straight, nullptr)) {
        g_object_unref(scaled);
        throw std::runtime_error(vips_error_buffer());
    }
    g_object_unref(scaled);
    // unpremultiply 把 uchar 升成 float。float 只是中间格式，必须落回 uchar：
    // 4 band 的 float 内存图 interpretation 是 MULTIBAND，vips_image_hasalpha()
    // 按 interpretation 判断（libvips/iofuncs/image.c:3141-3147），会把第 4 通道
    // 当成普通 band，webpsave 于是不写 alpha —— 导出就是这样丢掉透明度的。
    VipsImage* frame_u8 = nullptr;
    if (vips_cast(straight, &frame_u8, VIPS_FORMAT_UCHAR, nullptr)) {
        g_object_unref(straight);
        throw std::runtime_error(vips_error_buffer());
    }
    g_object_unref(straight);

    const int out_w = frame_u8->Xsize;
    const int out_h = frame_u8->Ysize;
    size_t size = 0;
    void* memory = vips_image_write_to_memory(frame_u8, &size);
    g_object_unref(frame_u8);
    if (memory == nullptr) throw std::runtime_error("帧写回内存失败");
    std::vector<unsigned char> pixels(size);
    std::memcpy(pixels.data(), memory, size);
    g_free(memory);
    // 预乘→降采样→unpremultiply 会在 alpha 极小的边缘留下被放大的量化噪声，
    // 用邻居颜色铺回去，轮廓就不再有一线黑灰点。
    RepairLowAlphaRgb(pixels, out_w, out_h, 16);
    frames_.push_back(std::move(pixels));
    ++frame_index_;
    return frame_index_ < n_;
}

ExportResult Exporter::finish() {
    if (frame_index_ < n_ || frames_.empty()) {
        throw std::runtime_error("导出帧还没渲染完");
    }
    const std::string path = UniquePath(options_.out_base);
    const std::vector<int> delays = FrameDelays(dt_, n_);
    save_webp(frames_, delays, path);

    ExportResult result;
    result.width = out_w_;
    result.height = out_h_;
    result.frames = n_;
    result.path = path;
    result.file_frames = file_frames_;
    result.duration_ms = file_duration_ms_;
    result.period_ms = static_cast<int>(std::lround(dt_ * n_ * 1000.0));
    result.frame_ms = static_cast<int>(std::lround(dt_ * 1000.0));
    result.frame_capped = frame_capped_;
    return result;
}

void Exporter::save_webp(std::vector<std::vector<unsigned char>>& frames,
                         const std::vector<int>& delays,
                         const std::string& path) {
    // 逐帧编码：WebPAnimEncoder（libwebpmux）本身就是"一帧一加"的接口，
    // 每喂一帧就知道编码了多少帧 —— 进度与剩余时间都是真值，不用估。
    // 不再走 vips_webpsave：那是一整段一次调用，中间没有任何回调。
    WebPConfig config;
    if (!WebPConfigInit(&config)) {
        throw std::runtime_error("WebP 配置初始化失败");
    }
    config.lossless = options_.lossless ? 1 : 0;
    config.quality = static_cast<float>(std::max(0, std::min(100, options_.quality)));
    // effort 0..? 对齐 libwebp 的 method 0..6（6 最慢最好）
    config.method = std::max(0, std::min(6, options_.effort));
    config.alpha_quality = std::max(0, std::min(100, options_.alpha_q));
    config.thread_level = 1;
    if (!WebPValidateConfig(&config)) {
        throw std::runtime_error("WebP 参数非法（质量/effort/alpha 越界）");
    }

    WebPAnimEncoderOptions enc_options;
    if (!WebPAnimEncoderOptionsInit(&enc_options)) {
        throw std::runtime_error("WebP 动画参数初始化失败");
    }
    enc_options.anim_params.loop_count = 0;      // 无限循环
    enc_options.anim_params.bgcolor = 0x00000000;  // 透明底色（原来是 vips 写死白）
    enc_options.minimize_size = 0;               // 不做二次搜索：帧数与耗时都可预期
    enc_options.allow_mixed = 0;
    enc_options.kmax = kExportKmax;
    enc_options.kmin = kExportKmin;

    WebPAnimEncoder* enc = WebPAnimEncoderNew(out_w_, out_h_, &enc_options);
    if (enc == nullptr) throw std::runtime_error("WebP 动画编码器创建失败");
    encode_total_ = n_;
    encode_done_ = 0;
    int timestamp = 0;
    try {
        const int count = std::min(n_, static_cast<int>(frames.size()));
        for (int i = 0; i < count; ++i) {
            timestamp += std::max(1, delays[static_cast<size_t>(i)]);
            WebPPicture pic;
            if (!WebPPictureInit(&pic)) {
                throw std::runtime_error("WebP 帧缓冲初始化失败");
            }
            pic.width = out_w_;
            pic.height = out_h_;
            pic.use_argb = 1;
            if (!WebPPictureImportRGBA(&pic, frames[static_cast<size_t>(i)].data(),
                                       out_w_ * 4)) {
                WebPPictureFree(&pic);
                throw std::runtime_error("WebP 帧导入失败（尺寸/行跨度不符）");
            }
            const bool ok = WebPAnimEncoderAdd(enc, &pic, timestamp, &config);
            WebPPictureFree(&pic);
            std::vector<unsigned char>().swap(frames[static_cast<size_t>(i)]);
            if (!ok) {
                throw std::runtime_error(std::string("WebP 编码失败：")
                                         + WebPAnimEncoderGetError(enc));
            }
            encode_done_ = i + 1;
        }
    } catch (...) {
        WebPAnimEncoderDelete(enc);
        throw;
    }

    WebPData data;
    WebPDataInit(&data);
    if (!WebPAnimEncoderAssemble(enc, &data)) {
        std::string why = WebPAnimEncoderGetError(enc);
        WebPAnimEncoderDelete(enc);
        throw std::runtime_error("WebP 容器组装失败：" + why);
    }
    WebPAnimEncoderDelete(enc);
    std::vector<unsigned char> bytes(data.bytes, data.bytes + data.size);
    WebPDataClear(&data);

    if (!io::WriteFileBytes(path, bytes.data(), bytes.size())) {
        throw std::runtime_error("写文件失败: " + path);
    }
    // 自检只看文件里真实有几帧、时长合计多少（相邻相同帧会被合并）
    const AnimStats stats = ReadAnimStats(bytes);
    file_frames_ = stats.frames;
    file_duration_ms_ = stats.duration_ms;
}

}  // namespace bl
