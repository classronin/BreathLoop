// 无缝循环：整数周期对齐 + A-B-A 升余弦驱动 + 弹簧稳态模拟。
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "domain/region.h"

namespace bl {

// 算法常量（不属于可调配置）：speed 基准、预热周期数、积分子步
constexpr double kCycleSeconds = 20.0;
constexpr int kPhysWarmup = 3;
constexpr double kPhysSubstep = 1.0 / 240.0;

// 每帧每个 BBOX 的位移（单位 = 挂载图层图像像素）
using FrameDisp = std::vector<std::pair<double, double>>;
using DispTable = std::vector<FrameDisp>;

// 多个 BBOX 的公共周期（秒）。speed 含义：kCycleSeconds/speed 秒一个周期。
double common_period(const std::vector<double>& speeds);

struct LoopPlan {
    bool ok = false;
    int n = 0;            // 一整个周期需要几帧
    double ms = 0.0;      // 每帧毫秒（绝不修改用户设定）
    int k = 1;            // 补齐几个周期
    double period = 0.0;  // 周期秒数
    int per_cycle = 0;
    double duration = 0.0;
    bool changed_n = false;
    bool changed_ms = false;
    std::string hint;     // ok == false 时给人看的原因
};

// 估算完整循环：P = round(T × 1000 / 每帧ms)，直接补全 1 个完整周期。
LoopPlan plan_loop(int n_frames, double ms_per_frame,
                   const std::vector<double>& speeds);

std::string format_duration(double seconds);

// 模拟一个完整 A-B-A 周期，返回 frames 个位移快照。
DispTable simulate_cycle(const std::vector<Region*>& regions,
                         double period, int frames, double warmup = kPhysWarmup,
                         double substep = kPhysSubstep);

// 稳态一个完整周期的位移表，按 substep 均匀采样。
// 契约核心：预览 / 导出 / 测试全部从这张表取位移，别无第二处物理实现。
DispTable build_table(const std::vector<Region*>& regions, double period,
                      double warmup = kPhysWarmup, double substep = kPhysSubstep);

// 取 t 时刻各位移：对周期取模 + 线性插值，尾首环绕闭合。
FrameDisp sample_table(const DispTable& table, double period, double t);

// 返回 (位移帧表, 每帧秒数)。与预览共用同一张稳态位移表。
std::pair<DispTable, double> build_animation(const std::vector<Region*>& regions,
                                             const LoopPlan& plan);

}  // namespace bl
