// 应用状态：文档 + 同步规则 + 无缝位移表。位移唯一来源是 seamless。
#pragma once

#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "domain/layers.h"
#include "domain/region.h"
#include "domain/seamless.h"

namespace bl {

// 拖动参数时两次重建位移表的最小间隔（秒）
constexpr double kPhysRebuildInterval = 0.1;

struct UndoSnapshot {
    std::unique_ptr<Document> doc;
    bool sync_on = true;
    std::map<int, std::pair<double, double>> sync_saved;
    int frames = 0;
    double ms_per_frame = 0.0;
    bool export_lossless = true;
    int export_quality = 0;
    int export_effort = 0;
    int export_alpha_q = 0;
    int export_w = 0;
    int export_h = 0;
    bool export_keep_ratio = true;
    bool export_size_auto = true;
    int eraser_diameter = 80;
    int eraser_feather = 16;
};

class AppState {
public:
    AppState();

    std::unique_ptr<Document> doc;
    std::vector<Region*> regions;  // 平铺视图，结构变化后原地更新

    bool sync_on = true;

    bool export_lossless = true;
    int export_quality = 0;
    int export_effort = 0;
    int export_alpha_q = 0;
    int export_w = 0;
    int export_h = 0;
    bool export_keep_ratio = true;
    bool export_size_auto = true;

    // 橡皮参数：跟着文档走，撤销能回到旧的直径/羽化。
    // 工具开关（套索/橡皮）不在 state 里：它是 Scene 的唯一 Tool 状态，
    // 不进快照也不存档（撤销不该顺手切换工具，重开也不带上次的工具）。
    int eraser_diameter = 80;
    int eraser_feather = 16;

    double ms_per_frame = 0.0;
    int frames = 0;
    double time = 0.0;

    // BBOX 数量的运行期上限（设置文件可编辑，钳到 kMaxRegions 以内）。
    // 不进撤销快照：它是"这台机器的配置"，不是文档内容。
    int bbox_limit = kDefaultBboxLimit;

    // ---- 文档与平铺视图 ----
    void refresh_regions();
    void rebind_doc(std::unique_ptr<Document> next_doc);
    void mark_physics_dirty();

    // ---- 图层 ----
    Layer* add_layer(std::unique_ptr<Layer> layer);
    std::unique_ptr<Layer> remove_layer(Layer* layer);
    bool move_layer(Layer* layer, Layer* new_parent, int index);

    // ---- BBOX ----
    bool can_add_bbox() const;
    Region* add_bbox(Layer* layer, double cx, double cy, Region* source = nullptr);
    void remove_bbox(Layer* layer, Region* region);
    bool move_bbox(Layer* layer, Region* region, int index);
    bool move_bbox_to(Layer* owner, Region* region, Layer* target, int index);

    // ---- 同步（设计.md 六）----
    void set_sync(bool on, Region* source = nullptr);
    void broadcast_sync(Region* source);
    void sync_param_changed(Region* region, const std::string& key);

    // ---- 撤销 ----
    UndoSnapshot snapshot() const;
    void restore(const UndoSnapshot& snap);

    // ---- 物理位移表 ----
    void update(double t);

private:
    std::vector<double> physics_key() const;
    const DispTable& rebuild_table(double t);

    std::map<int, std::pair<double, double>> sync_saved_;
    DispTable table_;
    double table_period_ = 0.0;
    std::vector<double> table_key_;
    bool dirty_ = true;
    double last_rebuild_ = -1e18;
};

}  // namespace bl
