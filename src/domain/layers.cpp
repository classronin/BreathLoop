#include "domain/layers.h"

#include <algorithm>
#include <cmath>

namespace bl {
namespace {

// 递归深拷贝子树；uid 与名字原样保留，parent 由调用方接上。
std::unique_ptr<Layer> clone_layer(const Layer& src) {
    std::unique_ptr<Layer> node = std::make_unique<Layer>();
    node->uid = src.uid;
    node->name = src.name;
    node->path = src.path;
    node->asset_key = src.asset_key;
    node->w = src.w;
    node->h = src.h;
    node->x = src.x;
    node->y = src.y;
    node->scale = src.scale;
    node->sx = src.sx;
    node->sy = src.sy;
    node->rot = src.rot;
    node->visible = src.visible;
    node->pad = src.pad;
    for (const auto& region : src.bboxes) {
        node->bboxes.push_back(std::make_unique<Region>(*region));
    }
    for (const auto& child : src.children) {
        std::unique_ptr<Layer> copy = clone_layer(*child);
        copy->parent = node.get();
        node->children.push_back(std::move(copy));
    }
    return node;
}

}  // namespace

// ---- Layer 坐标 ----

double Layer::scale_x() const { return scale * sx; }

double Layer::scale_y() const { return scale * sy; }

std::pair<double, double> Layer::to_world(double px, double py) const {
    const double lx = (px - w / 2.0) * scale_x();
    const double ly = (py - h / 2.0) * scale_y();
    const double c = std::cos(rot);
    const double s = std::sin(rot);
    return {x + lx * c - ly * s, y + lx * s + ly * c};
}

std::pair<double, double> Layer::from_world(double wx, double wy) const {
    const double kx = scale_x();
    const double ky = scale_y();
    const double dx = wx - x;
    const double dy = wy - y;
    const double c = std::cos(rot);
    const double s = std::sin(rot);
    // 旋转的逆变换（R(-rot)）
    const double lx = dx * c + dy * s;
    const double ly = -dx * s + dy * c;
    return {lx / (kx != 0.0 ? kx : 1.0) + w / 2.0,
            ly / (ky != 0.0 ? ky : 1.0) + h / 2.0};
}

std::pair<double, double> Layer::map_point_from(const Layer& src, double px,
                                                double py) const {
    const std::pair<double, double> world = src.to_world(px, py);
    return from_world(world.first, world.second);
}

double Layer::len_factor_from(const Layer& src) const {
    // BBOX 位移是各向同性的长度，等比部分足够；非等比只影响图像四边形。
    const double mine = scale;
    return mine != 0.0 ? src.scale / mine : 1.0;
}

Rect Layer::world_rect() const {
    // 旋转后取轴对齐包围盒（命中/吸附用包围盒，绘制用真实四角）
    const double hw = w / 2.0 * scale_x();
    const double hh = h / 2.0 * scale_y();
    const double c = std::cos(rot);
    const double s = std::sin(rot);
    const double ext_x = std::abs(hw * c) + std::abs(hh * s);
    const double ext_y = std::abs(hw * s) + std::abs(hh * c);
    return Rect{x - ext_x, y - ext_y, x + ext_x, y + ext_y};
}

bool Layer::local_contains(double px, double py) const {
    return px >= 0.0 && px < w && py >= 0.0 && py < h;
}

// ---- Layer 变换 ----

void Layer::move_by(double dx, double dy) {
    std::vector<Layer*> nodes;
    nodes.push_back(this);
    if (parent == nullptr) {
        for (Layer* node : descendants()) nodes.push_back(node);
    }
    for (Layer* node : nodes) {
        node->x += dx;
        node->y += dy;
    }
}

void Layer::set_scale(double value) {
    if (scale == 0.0) {
        scale = value;
        return;
    }
    const double factor = value / scale;
    if (parent == nullptr) {
        for (Layer* child : descendants()) {
            child->x = x + (child->x - x) * factor;
            child->y = y + (child->y - y) * factor;
            child->scale *= factor;
        }
    }
    scale = value;
}

void Layer::reset_scale() {
    scale = 1.0;
    sx = 1.0;
    sy = 1.0;
}

// ---- Layer 树 ----

std::vector<Layer*> Layer::ancestors() const {
    std::vector<Layer*> out;
    Layer* node = parent;
    while (node != nullptr) {
        out.push_back(node);
        node = node->parent;
    }
    return out;
}

std::vector<Layer*> Layer::descendants() const {
    std::vector<Layer*> out;
    struct Walker {
        static void walk(Layer* node, std::vector<Layer*>& sink) {
            for (const auto& child : node->children) {
                sink.push_back(child.get());
                walk(child.get(), sink);
            }
        }
    };
    Walker::walk(const_cast<Layer*>(this), out);
    return out;
}

bool Layer::is_visible_chain() const {
    if (!visible) return false;
    for (const Layer* ancestor : ancestors()) {
        if (!ancestor->visible) return false;
    }
    return true;
}

Layer* Layer::root() const {
    Layer* node = const_cast<Layer*>(this);
    while (node->parent != nullptr) node = node->parent;
    return node;
}

std::vector<BBoxView> Layer::bbox_views() const {
    std::vector<BBoxView> views;
    std::vector<Layer*> owners;
    owners.push_back(const_cast<Layer*>(this));
    for (Layer* node : descendants()) owners.push_back(node);

    for (Layer* owner : owners) {
        if (!owner->is_visible_chain()) continue;
        const double k = len_factor_from(*owner);
        for (const auto& region : owner->bboxes) {
            // 关了眼睛的 BBOX 不参与变形/遮罩：预览和导出共用这唯一的过滤点
            if (!region->visible) continue;
            const std::pair<double, double> mapped =
                map_point_from(*owner, region->cx, region->cy);
            BBoxView view;
            view.owner = owner;
            view.region = region.get();
            view.cx = mapped.first;
            view.cy = mapped.second;
            view.rx = region->rx * k;
            view.ry = region->ry * k;
            view.rot = region->rot + rot;   // BBOX 跟着图层一起转
            view.k = k;
            views.push_back(view);
        }
    }
    return views;
}

LayerClip Layer::copy_snapshot() const {
    LayerClip clip;
    clip.name = name;
    clip.path = path;
    clip.asset_key = asset_key;
    clip.w = w;
    clip.h = h;
    clip.x = x;
    clip.y = y;
    clip.scale = scale;
    clip.sx = sx;
    clip.sy = sy;
    clip.rot = rot;
    clip.visible = visible;
    // 套索不随复制走：它是单例，粘出来的层不带套索（否则会凭空多一个）。
    for (const auto& region : bboxes) clip.bboxes.push_back(*region);
    for (const auto& child : children) clip.children.push_back(child->copy_snapshot());
    return clip;
}

// ---- Document 查询 ----

Rect Document::canvas_rect() const {
    const double hw = canvas_w / 2.0;
    const double hh = canvas_h / 2.0;
    return Rect{-hw, -hh, hw, hh};
}

std::vector<Layer*> Document::flatten() const {
    std::vector<Layer*> out;
    struct Walker {
        static void walk(Layer* node, std::vector<Layer*>& sink) {
            sink.push_back(node);
            for (const auto& child : node->children) walk(child.get(), sink);
        }
    };
    for (const auto& layer : roots_) Walker::walk(layer.get(), out);
    return out;
}

Layer* Document::find(int uid) const {
    for (Layer* layer : flatten()) {
        if (layer->uid == uid) return layer;
    }
    return nullptr;
}

bool Document::find_region(int region_uid, Layer** layer, Region** region) const {
    for (Layer* node : flatten()) {
        for (const auto& item : node->bboxes) {
            if (item->uid == region_uid) {
                if (layer != nullptr) *layer = node;
                if (region != nullptr) *region = item.get();
                return true;
            }
        }
    }
    return false;
}

Layer* Document::layer_of(const Region* region) const {
    for (Layer* node : flatten()) {
        for (const auto& item : node->bboxes) {
            if (item.get() == region) return node;
        }
    }
    return nullptr;
}

std::vector<Region*> Document::all_regions() const {
    std::vector<Region*> out;
    for (Layer* layer : flatten()) {
        for (const auto& item : layer->bboxes) out.push_back(item.get());
    }
    return out;
}

// 真正参与"遮罩 + 呼吸位移"的那批 BBOX：自己的眼睛开着，且所属图层链可见。
// 预览物理表和导出都吃这一份，所以"眼睛关掉 = 预览和导出一起停"。
std::vector<Region*> Document::visible_regions() const {
    std::vector<Region*> out;
    for (Layer* layer : flatten()) {
        if (!layer->is_visible_chain()) continue;
        for (const auto& item : layer->bboxes) {
            if (item->visible) out.push_back(item.get());
        }
    }
    return out;
}

int Document::num_regions() const {
    int count = 0;
    for (const auto& layer : roots_) {
        struct Counter {
            static void count(const Layer* node, int* sink) {
                *sink += static_cast<int>(node->bboxes.size());
                for (const auto& child : node->children) count(child.get(), sink);
            }
        };
        Counter::count(layer.get(), &count);
    }
    return count;
}

// ---- Document 结构 ----

std::unique_ptr<Layer> Document::new_layer(const std::string& name,
                                           const std::string& path,
                                           int asset_key, int w, int h, double x,
                                           double y) {
    std::unique_ptr<Layer> layer = std::make_unique<Layer>();
    layer->uid = next_uid_++;
    layer->name = name;
    layer->path = path;
    layer->asset_key = asset_key;
    layer->w = w;
    layer->h = h;
    layer->x = x;
    layer->y = y;
    return layer;
}

Layer* Document::add_layer_top(std::unique_ptr<Layer> layer) {
    layer->parent = nullptr;
    Layer* raw = layer.get();
    roots_.push_back(std::move(layer));
    return raw;
}

std::pair<Layer*, int> Document::add_layer_copy(const LayerClip& clip, double x,
                                                double y) {
    const double dx = x - clip.x;
    const double dy = y - clip.y;
    int skipped = 0;
    int count = num_regions();

    struct Builder {
        Document* doc;
        double dx, dy;
        int* skipped;
        int* count;

        std::unique_ptr<Layer> build(const LayerClip& node) {
            std::unique_ptr<Layer> layer = doc->new_layer(
                node.name, node.path, node.asset_key, node.w, node.h,
                node.x + dx, node.y + dy);
            layer->scale = node.scale;
            layer->sx = node.sx;
            layer->sy = node.sy;
            layer->rot = node.rot;
            layer->visible = node.visible;
            layer->pad = node.pad;
            for (const Region& source : node.bboxes) {
                if (*count >= kMaxRegions) {
                    ++(*skipped);
                    continue;
                }
                std::unique_ptr<Region> region =
                    std::make_unique<Region>(Region::make_default());
                region->copy_params_from(source);
                region->cx = source.cx;
                region->cy = source.cy;
                region->name = "B" + std::to_string(doc->next_bbox_++);
                layer->bboxes.push_back(std::move(region));
                ++(*count);
            }
            for (const LayerClip& child_clip : node.children) {
                std::unique_ptr<Layer> child = build(child_clip);
                child->parent = layer.get();
                layer->children.push_back(std::move(child));
            }
            return layer;
        }
    };

    Builder builder{this, dx, dy, &skipped, &count};
    std::unique_ptr<Layer> layer = builder.build(clip);
    layer->parent = nullptr;
    Layer* raw = layer.get();
    roots_.push_back(std::move(layer));
    return {raw, skipped};
}

std::unique_ptr<Layer> Document::detach(Layer* layer) {
    std::vector<std::unique_ptr<Layer>>& siblings = siblings_of(layer);
    for (size_t i = 0; i < siblings.size(); ++i) {
        if (siblings[i].get() == layer) {
            std::unique_ptr<Layer> owned = std::move(siblings[i]);
            siblings.erase(siblings.begin() + static_cast<long>(i));
            owned->parent = nullptr;
            return owned;
        }
    }
    return nullptr;
}

std::unique_ptr<Layer> Document::remove_layer(Layer* layer,
                                              std::vector<Layer*>* promoted) {
    std::vector<std::unique_ptr<Layer>>& siblings = siblings_of(layer);
    auto found = std::find_if(siblings.begin(), siblings.end(),
                              [layer](const std::unique_ptr<Layer>& item) {
                                  return item.get() == layer;
                              });
    if (found == siblings.end()) return nullptr;
    const int index = static_cast<int>(found - siblings.begin());
    std::unique_ptr<Layer> owned = std::move(*found);
    siblings.erase(found);

    int insert_at = index;
    for (auto& child : owned->children) {
        child->parent = owned->parent;
        siblings.insert(siblings.begin() + insert_at, std::move(child));
        if (promoted != nullptr) promoted->push_back(siblings[insert_at].get());
        ++insert_at;
    }
    owned->children.clear();
    owned->parent = nullptr;
    return owned;
}

bool Document::move_layer(Layer* layer, Layer* new_parent, int index) {
    if (new_parent == layer) return false;
    if (new_parent != nullptr) {
        for (Layer* node : layer->descendants()) {
            if (node == new_parent) return false;
        }
    }
    std::vector<std::unique_ptr<Layer>>& old_siblings = siblings_of(layer);
    const bool present = std::any_of(
        old_siblings.begin(), old_siblings.end(),
        [layer](const std::unique_ptr<Layer>& item) { return item.get() == layer; });
    if (!present) return false;

    std::unique_ptr<Layer> owned = detach(layer);
    if (!owned) return false;

    std::vector<std::unique_ptr<Layer>>& target =
        new_parent == nullptr ? roots_ : new_parent->children;
    const int clamped =
        std::max(0, std::min(index, static_cast<int>(target.size())));
    owned->parent = new_parent;
    target.insert(target.begin() + clamped, std::move(owned));
    return true;
}

int Document::sibling_index(const Layer* layer) const {
    const std::vector<std::unique_ptr<Layer>>& siblings = siblings_of(layer);
    for (size_t i = 0; i < siblings.size(); ++i) {
        if (siblings[i].get() == layer) return static_cast<int>(i);
    }
    return -1;
}

std::vector<std::unique_ptr<Layer>>& Document::siblings_of(const Layer* layer) {
    return layer->parent == nullptr ? roots_ : layer->parent->children;
}

const std::vector<std::unique_ptr<Layer>>& Document::siblings_of(
    const Layer* layer) const {
    return layer->parent == nullptr ? roots_ : layer->parent->children;
}

// ---- Document BBOX ----

Region* Document::add_bbox(Layer* layer, double cx, double cy) {
    if (num_regions() >= kMaxRegions) return nullptr;
    std::unique_ptr<Region> region =
        std::make_unique<Region>(Region::make_default());
    region->name = "B" + std::to_string(next_bbox_++);
    region->cx = cx;
    region->cy = cy;
    layer->bboxes.push_back(std::move(region));
    return layer->bboxes.back().get();
}

void Document::remove_bbox(Layer* layer, Region* region) {
    for (size_t i = 0; i < layer->bboxes.size(); ++i) {
        if (layer->bboxes[i].get() == region) {
            layer->bboxes.erase(layer->bboxes.begin() + static_cast<long>(i));
            return;
        }
    }
}

bool Document::move_bbox(Layer* layer, Region* region, int index) {
    return move_bbox_to(layer, region, layer, index);
}

bool Document::move_bbox_to(Layer* owner, Region* region, Layer* target,
                            int index) {
    int current = -1;
    for (size_t i = 0; i < owner->bboxes.size(); ++i) {
        if (owner->bboxes[i].get() == region) {
            current = static_cast<int>(i);
            break;
        }
    }
    if (current < 0) return false;

    if (target != owner) {
        const std::pair<double, double> mapped =
            target->map_point_from(*owner, region->cx, region->cy);
        const double k = target->len_factor_from(*owner);
        region->cx = mapped.first;
        region->cy = mapped.second;
        region->rx *= k;
        region->ry *= k;
    }

    std::unique_ptr<Region> owned = std::move(owner->bboxes[current]);
    owner->bboxes.erase(owner->bboxes.begin() + current);
    const int clamped =
        std::max(0, std::min(index, static_cast<int>(target->bboxes.size())));
    if (target == owner && clamped == current) {
        owner->bboxes.insert(owner->bboxes.begin() + current, std::move(owned));
        return false;
    }
    target->bboxes.insert(target->bboxes.begin() + clamped, std::move(owned));
    return true;
}

// ---- Document 边界与复制 ----

std::optional<Rect> Document::world_bounds(bool visible_only) const {
    bool any = false;
    Rect box{};
    for (Layer* layer : flatten()) {
        if (visible_only && !layer->is_visible_chain()) continue;
        const Rect rect = layer->world_rect();
        if (!any) {
            box = rect;
            any = true;
            continue;
        }
        box.x0 = std::min(box.x0, rect.x0);
        box.y0 = std::min(box.y0, rect.y0);
        box.x1 = std::max(box.x1, rect.x1);
        box.y1 = std::max(box.y1, rect.y1);
    }
    if (!any) return std::nullopt;
    return box;
}

std::unique_ptr<Document> Document::clone() const {
    std::unique_ptr<Document> copy = std::make_unique<Document>();
    copy->next_uid_ = next_uid_;
    copy->next_bbox_ = next_bbox_;
    copy->canvas_w = canvas_w;
    copy->canvas_h = canvas_h;
    for (const auto& root : roots_) {
        std::unique_ptr<Layer> node = clone_layer(*root);
        node->parent = nullptr;
        copy->roots_.push_back(std::move(node));
    }
    return copy;
}

}  // namespace bl
