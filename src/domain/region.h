// 单个 BBOX 的参数与几何。默认值唯一来源：resources/presets.json（编译期内嵌）。
#pragma once

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace bl {

// 架构约束：全文档最多几个 BBOX；改动要同步 shaders.h 的数组上限
// 编译期容量：着色器的 uniform 数组（u_center/u_radius/u_rot/u_amp_x/u_amp_y）
// 按这个长度生成，改它等于改 GPU 侧固定数组。这是"最多能装下多少"。
constexpr int kMaxRegions = 32;
// 运行期上限：真正拦住用户加 BBOX 的数字，写在设置文件里可编辑
// （breathloop_settings.json 的 app.bbox_limit）。它只会被夹到 kMaxRegions 以内，
// 所以提高上限只要改 json，不用重新编译；要突破 32 才需要动 kMaxRegions。
constexpr int kDefaultBboxLimit = 16;

// 同步语义只覆盖幅度/速度（设计.md 六）
inline constexpr std::array<const char*, 2> kSyncParamKeys{"amp", "speed"};

struct Region {
    // 参数（面板派生滑块的那 6 个）
    double amp = 0.0;
    double speed = 0.0;
    double aniso_x = 0.0;
    double aniso_y = 0.0;
    double zeta = 0.0;
    double freq = 0.0;

    // 几何（住在挂载图层的图像坐标系；rx/ry/rot 只用手柄）
    double cx = 0.0;
    double cy = 0.0;
    double rx = 0.0;
    double ry = 0.0;
    double rot = 0.0;

    // 物理采样输出，位移单位 = 挂载图层图像像素
    double disp_x = 0.0;
    double disp_y = 0.0;

    int uid = 0;
    std::string name;
    // 这个 BBOX 自己的眼睛：关掉 = 区域不显示、不呼吸（不进遮罩/位移）。
    // 与所有者的图层眼睛是单向关系：图层关掉会连带它不生效，但它的开关值保留。
    bool visible = true;

    // 按 presets.json 的 defaults 造一个默认 BBOX（uid 由全局序列发号）
    static Region make_default();

    // 整套参数复制；位置、运行时输出、身份保持自己的。
    void copy_params_from(const Region& other);
    // 只复制同步语义覆盖的那两个参数
    void copy_sync_params_from(const Region& other);
    // 部位预设只改形变/阻尼/频率，不动幅度速度与位置
    void apply_preset(const std::string& name);
    void reset_shape();

    std::pair<double, double> to_local(double px, double py) const;
    std::pair<double, double> to_world(double lx, double ly) const;
    bool contains(double px, double py) const;
};

// 面板按字段顺序派生滑块：key/label/范围/步长/格式与取值指针同处一份声明，
// 加一个滑块只改这里一次（对应 Python 的 field metadata）。
struct ParamDesc {
    const char* key;
    const char* label;
    double min;
    double max;
    double step;
    const char* fmt;
    double Region::*member;
};

inline constexpr std::array<ParamDesc, 6> kRegionParamDescs{{
    {"amp", "幅度", 0.0, 100.0, 0.5, "{:.1f}", &Region::amp},
    {"speed", "速度", 1.0, 100.0, 1.0, "{:.0f}", &Region::speed},
    {"aniso_x", "形变X", 0.0, 1.0, 0.05, "{:.2f}", &Region::aniso_x},
    {"aniso_y", "形变Y", 0.0, 1.0, 0.05, "{:.2f}", &Region::aniso_y},
    {"zeta", "阻尼", 0.0, 1.0, 0.05, "{:.2f}", &Region::zeta},
    {"freq", "频率", 0.5, 8.0, 0.5, "{:.1f}", &Region::freq},
}};

// 出厂预设：JSON 里的 defaults 段与 parts 段。
// 解析失败会抛 std::runtime_error（对应 Python 的 import 期 RuntimeError）。
class Presets {
public:
    static const Presets& get();

    // defaults 里的某个字段值；字段缺失在构造时就已抛错，这里必然命中。
    double default_value(const std::string& key) const;

    // 部位预设；没有这个部位返回 nullptr。
    const std::vector<std::pair<std::string, double>>* preset(const std::string& name) const;

    const std::vector<std::pair<std::string, std::vector<std::pair<std::string, double>>>>&
    all() const {
        return presets_;
    }

private:
    Presets();
    std::vector<std::pair<std::string, double>> defaults_;
    std::vector<std::pair<std::string, std::vector<std::pair<std::string, double>>>> presets_;
};

// 字段名 → 成员指针（apply_preset / presets.json 校验共用）
double* member_by_key(Region& region, const std::string& key);

}  // namespace bl
