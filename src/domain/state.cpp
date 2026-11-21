#include "domain/state.h"

#include <algorithm>

#include "config.h"

namespace bl {

AppState::AppState() {
    doc = std::make_unique<Document>();
    export_lossless = cfg::EXPORT_DEFAULT_LOSSLESS;
    export_quality = cfg::EXPORT_DEFAULT_QUALITY;
    export_effort = cfg::EXPORT_DEFAULT_EFFORT;
    export_alpha_q = cfg::EXPORT_DEFAULT_ALPHA_Q;
    export_w = 0;
    export_h = 0;
    export_keep_ratio = true;
    export_size_auto = true;
    ms_per_frame = cfg::DEFAULT_MS_PER_FRAME;
    frames = cfg::DEFAULT_FRAMES;
    time = 0.0;
    refresh_regions();
}

// ---- 文档与平铺视图 ----

void AppState::refresh_regions() {
    // 物理表只喂"眼睛开着且图层可见"的 BBOX：关掉眼睛的区域不进呼吸位移计算
    regions = doc->visible_regions();
    mark_physics_dirty();
}

void AppState::rebind_doc(std::unique_ptr<Document> next_doc) {
    doc = std::move(next_doc);
    refresh_regions();
}

void AppState::mark_physics_dirty() {
    dirty_ = true;
}

// ---- 图层 ----

Layer* AppState::add_layer(std::unique_ptr<Layer> layer) {
    Layer* raw = doc->add_layer_top(std::move(layer));
    refresh_regions();
    return raw;
}

std::unique_ptr<Layer> AppState::remove_layer(Layer* layer) {
    for (const auto& region : layer->bboxes) sync_saved_.erase(region->uid);
    std::vector<Layer*> promoted;
    std::unique_ptr<Layer> owned = doc->remove_layer(layer, &promoted);
    refresh_regions();
    return owned;
}

bool AppState::move_layer(Layer* layer, Layer* new_parent, int index) {
    const bool moved = doc->move_layer(layer, new_parent, index);
    if (moved) refresh_regions();
    return moved;
}

// ---- BBOX ----

bool AppState::can_add_bbox() const {
    // 用户可编辑的上限再夹一次容量，防止手改 json 写个 999 把着色器数组写越界。
    const int limit = bbox_limit < 1 ? 1
                     : (bbox_limit > kMaxRegions ? kMaxRegions : bbox_limit);
    return static_cast<int>(regions.size()) < limit;
}

Region* AppState::add_bbox(Layer* layer, double cx, double cy, Region* source) {
    Region* region = doc->add_bbox(layer, cx, cy);
    if (region == nullptr) return nullptr;
    // 还原点取出生值；同步开启时新生 BBOX 立刻跟随当前主 BBOX
    sync_saved_[region->uid] = {region->amp, region->speed};
    if (sync_on && source != nullptr) region->copy_sync_params_from(*source);
    refresh_regions();
    return region;
}

void AppState::remove_bbox(Layer* layer, Region* region) {
    doc->remove_bbox(layer, region);
    sync_saved_.erase(region->uid);
    refresh_regions();
}

bool AppState::move_bbox(Layer* layer, Region* region, int index) {
    const bool moved = doc->move_bbox(layer, region, index);
    if (moved) refresh_regions();
    return moved;
}

bool AppState::move_bbox_to(Layer* owner, Region* region, Layer* target,
                            int index) {
    const bool moved = doc->move_bbox_to(owner, region, target, index);
    if (moved) refresh_regions();
    return moved;
}

// ---- 同步（设计.md 六）----

void AppState::set_sync(bool on, Region* source) {
    if (on == sync_on) return;
    sync_on = on;
    if (on) {
        sync_saved_.clear();
        for (Region* region : regions) {
            sync_saved_[region->uid] = {region->amp, region->speed};
        }
        if (source != nullptr) broadcast_sync(source);
    } else {
        for (Region* region : regions) {
            auto it = sync_saved_.find(region->uid);
            if (it != sync_saved_.end()) {
                region->amp = it->second.first;
                region->speed = it->second.second;
            }
        }
    }
    mark_physics_dirty();
}

void AppState::broadcast_sync(Region* source) {
    for (Region* region : regions) {
        if (region != source) region->copy_sync_params_from(*source);
    }
    mark_physics_dirty();
}

void AppState::sync_param_changed(Region* region, const std::string& key) {
    if (sync_on && (key == "amp" || key == "speed")) broadcast_sync(region);
    mark_physics_dirty();
}

// ---- 撤销 ----

UndoSnapshot AppState::snapshot() const {
    UndoSnapshot snap;
    snap.doc = doc->clone();
    snap.sync_on = sync_on;
    snap.sync_saved = sync_saved_;
    snap.frames = frames;
    snap.ms_per_frame = ms_per_frame;
    snap.export_lossless = export_lossless;
    snap.export_quality = export_quality;
    snap.export_effort = export_effort;
    snap.export_alpha_q = export_alpha_q;
    snap.export_w = export_w;
    snap.export_h = export_h;
    snap.export_keep_ratio = export_keep_ratio;
    snap.export_size_auto = export_size_auto;
    snap.eraser_diameter = eraser_diameter;
    snap.eraser_feather = eraser_feather;
    return snap;
}

void AppState::restore(const UndoSnapshot& snap) {
    rebind_doc(snap.doc->clone());
    sync_on = snap.sync_on;
    sync_saved_ = snap.sync_saved;
    frames = snap.frames;
    ms_per_frame = snap.ms_per_frame;
    export_lossless = snap.export_lossless;
    export_quality = snap.export_quality;
    export_effort = snap.export_effort;
    export_alpha_q = snap.export_alpha_q;
    export_w = snap.export_w;
    export_h = snap.export_h;
    export_keep_ratio = snap.export_keep_ratio;
    export_size_auto = snap.export_size_auto;
    eraser_diameter = snap.eraser_diameter;
    eraser_feather = snap.eraser_feather;
    table_.clear();
    mark_physics_dirty();
}

// ---- 物理位移表 ----

std::vector<double> AppState::physics_key() const {
    std::vector<double> key;
    key.reserve(regions.size() * 6);
    for (const Region* region : regions) {
        key.push_back(region->amp);
        key.push_back(region->speed);
        key.push_back(region->aniso_x);
        key.push_back(region->aniso_y);
        key.push_back(region->zeta);
        key.push_back(region->freq);
    }
    return key;
}

const DispTable& AppState::rebuild_table(double t) {
    std::vector<double> speeds;
    speeds.reserve(regions.size());
    for (const Region* region : regions) speeds.push_back(region->speed);
    table_period_ = common_period(speeds);
    table_ = build_table(regions, table_period_);
    table_key_ = physics_key();
    dirty_ = false;
    last_rebuild_ = t;
    return table_;
}

void AppState::update(double t) {
    if (regions.empty()) {
        table_.clear();
        table_key_.clear();
        dirty_ = true;
        return;
    }
    if (table_.empty() || dirty_ ||
        (physics_key() != table_key_ && t - last_rebuild_ >= kPhysRebuildInterval)) {
        rebuild_table(t);
    }
    FrameDisp disp = sample_table(table_, table_period_, t);
    const size_t count = std::min(regions.size(), disp.size());
    for (size_t i = 0; i < count; ++i) {
        regions[i]->disp_x = disp[i].first;
        regions[i]->disp_y = disp[i].second;
    }
}

}  // namespace bl
