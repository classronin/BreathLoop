#include "domain/seamless.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <string>

#include "config.h"

namespace bl {
namespace {

constexpr double kPi = 3.14159265358979323846;

}  // namespace

double common_period(const std::vector<double>& speeds) {
    std::vector<int> vals;
    for (double speed : speeds) {
        if (speed > 0.0) {
            vals.push_back(std::max(1, static_cast<int>(std::lround(speed))));
        }
    }
    if (vals.empty()) return 2.0;
    int g = vals[0];
    for (size_t i = 1; i < vals.size(); ++i) g = std::gcd(g, vals[i]);
    return kCycleSeconds / static_cast<double>(g);
}

LoopPlan plan_loop(int n_frames, double ms_per_frame,
                   const std::vector<double>& speeds) {
    LoopPlan plan;
    const double period = common_period(speeds);
    const double ms = std::max(1e-6, ms_per_frame);
    const int frames = std::max(1, static_cast<int>(std::lround(period * 1000.0 / ms)));
    plan.period = period;
    if (frames > cfg::FRAMES_MAX) {
        plan.ok = false;
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "周期过长：1 个周期需 %d 帧（上限 %d），请提高速度或降低 fps。",
                      frames, cfg::FRAMES_MAX);
        plan.hint = buf;
        return plan;
    }
    plan.ok = true;
    plan.n = frames;
    // 循环时长必须**精确**等于 period。原来把 dt 直接取 fps 的 1000/fps，
    // 而 frames = round(period/ms)，于是 n×dt 与 period 最多差半帧：整段循环被
    // 拉长/压短（预览按真实时间走，导出按 n×ms 走，两边速度就对不上），接缝处
    // 还会相位跳一次。这里把余数摊进每一帧：dt = period/n，总时长 = period。
    plan.ms = period * 1000.0 / static_cast<double>(frames);
    plan.k = 1;
    plan.per_cycle = frames;
    plan.duration = period;
    plan.changed_n = frames != n_frames;
    plan.changed_ms = std::fabs(plan.ms - ms) > 1e-9;
    return plan;
}

std::string format_duration(double seconds) {
    char buf[64];
    if (seconds < 60.0) {
        std::snprintf(buf, sizeof(buf), "%.1f 秒", seconds);
        return buf;
    }
    if (seconds < 3600.0) {
        std::snprintf(buf, sizeof(buf), "%d 分 %d 秒",
                      static_cast<int>(seconds / 60.0),
                      static_cast<int>(std::fmod(seconds, 60.0)));
        return buf;
    }
    if (seconds < 86400.0) {
        std::snprintf(buf, sizeof(buf), "%d 小时 %d 分",
                      static_cast<int>(seconds / 3600.0),
                      static_cast<int>(std::fmod(seconds, 3600.0) / 60.0));
        return buf;
    }
    std::snprintf(buf, sizeof(buf), "%d 天 %d 小时",
                  static_cast<int>(seconds / 86400.0),
                  static_cast<int>(std::fmod(seconds, 86400.0) / 3600.0));
    return buf;
}

DispTable simulate_cycle(const std::vector<Region*>& regions,
                         double period, int frames, double warmup,
                         double substep) {
    const int m = static_cast<int>(regions.size());
    std::vector<double> x(m, 0.0);
    std::vector<double> y(m, 0.0);
    std::vector<double> vx(m, 0.0);
    std::vector<double> vy(m, 0.0);
    std::vector<double> w0(m, 0.0);
    std::vector<double> z(m, 0.0);
    std::vector<double> amp(m, 0.0);
    std::vector<double> ax(m, 0.0);
    std::vector<double> ay(m, 0.0);
    std::vector<double> spd(m, 0.0);
    for (int i = 0; i < m; ++i) {
        w0[i] = 2.0 * kPi * regions[i]->freq;
        z[i] = regions[i]->zeta;
        amp[i] = regions[i]->amp / 100.0;
        ax[i] = regions[i]->aniso_x;
        ay[i] = regions[i]->aniso_y;
        // 驱动相位必须与 common_period 用的是同一个整数 speed：周期按
        // round(speed) 求 gcd，若这里用原始小数，一个公共周期内该 BBOX 转的
        // 不是整数圈，首尾相位不闭合，导出的循环就会在接缝处跳一下。
        spd[i] = static_cast<double>(std::max(1, static_cast<int>(std::lround(regions[i]->speed))));
    }

    double t = 0.0;
    auto advance_to = [&](double target) {
        while (t < target - 1e-9) {
            const double h = std::min(substep, target - t);
            for (int i = 0; i < m; ++i) {
                const double phase = 2.0 * kPi * spd[i] * t / kCycleSeconds;
                const double drive = 0.5 * (1.0 - std::cos(phase)) * amp[i];
                const double tx = drive * ax[i];
                const double ty = drive * ay[i];
                vx[i] += (w0[i] * w0[i] * (tx - x[i]) - 2.0 * z[i] * w0[i] * vx[i]) * h;
                vy[i] += (w0[i] * w0[i] * (ty - y[i]) - 2.0 * z[i] * w0[i] * vy[i]) * h;
                x[i] += vx[i] * h;
                y[i] += vy[i] * h;
            }
            t += h;
        }
    };

    advance_to(period * warmup);
    DispTable out;
    out.reserve(static_cast<size_t>(std::max(0, frames)));
    for (int j = 0; j < frames; ++j) {
        advance_to(period * warmup + period * j / static_cast<double>(frames));
        FrameDisp row;
        row.reserve(static_cast<size_t>(m));
        for (int i = 0; i < m; ++i) row.emplace_back(x[i], y[i]);
        out.push_back(std::move(row));
    }
    return out;
}

DispTable build_table(const std::vector<Region*>& regions, double period,
                      double warmup, double substep) {
    const int steps = std::max(2, static_cast<int>(std::lround(period / substep)));
    return simulate_cycle(regions, period, steps, warmup, substep);
}

FrameDisp sample_table(const DispTable& table, double period, double t) {
    const int n = static_cast<int>(table.size());
    if (n == 0) return {};
    double idx = (t / period * n);
    idx = std::fmod(idx, static_cast<double>(n));
    if (idx < 0.0) idx += n;
    const int i0 = static_cast<int>(idx) % n;
    const int i1 = (i0 + 1) % n;
    const double frac = idx - static_cast<double>(static_cast<int>(idx));
    FrameDisp out;
    out.reserve(static_cast<size_t>(n > 0 ? table[i0].size() : 0));
    for (size_t k = 0; k < table[i0].size(); ++k) {
        const std::pair<double, double>& a = table[i0][k];
        const std::pair<double, double>& b = table[i1][k];
        out.emplace_back((1.0 - frac) * a.first + frac * b.first,
                         (1.0 - frac) * a.second + frac * b.second);
    }
    return out;
}

std::pair<DispTable, double> build_animation(
    const std::vector<Region*>& regions, const LoopPlan& plan) {
    DispTable table = build_table(regions, plan.period);
    const double dt = plan.ms / 1000.0;
    const int total = plan.per_cycle * plan.k;
    DispTable frames;
    frames.reserve(static_cast<size_t>(std::max(0, total)));
    for (int j = 0; j < total; ++j) {
        frames.push_back(sample_table(table, plan.period, j * dt));
    }
    return {std::move(frames), dt};
}

}  // namespace bl
