// 图层树纯数据模型：Layer 是节点，Document 是整棵树与结构操作。
//
// 坐标约定（唯一出处，别处不得重推）：
// - 世界坐标：画布平面，y 向下，单位像素；图层 x/y 是图像中心。
// - 图层坐标：该图层图像像素，原点左上，y 向下；BBOX 的 cx/cy/rx/ry 住这里。
// - 图层间换算必须经世界坐标中转，长度系数 = 源图层 scale / 目标图层 scale。
//
// 结构与语义（设计.md 三、五）：
// - 渲染：父先于子，同级 children 末位在最上；子遮挡父。
// - 影响：BBOX 作用于挂载层和所有祖先；对目标层而言，生效集合 =
//   自己的 BBOX + 全部可见子孙的 BBOX。
// - 变换：根图层移动/缩放牵动整棵子树；非根图层只动自己。
//   无论哪种，挂在自己身上的 BBOX 都随图层走。
//
// 与 Python 版的一处实现差异（契约不变）：BBOX 用 unique_ptr 单独持有，
// 因为选择集合按对象身份存 Region*，vector 扩容不能把地址搬走。
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "domain/lasso.h"
#include "domain/region.h"

namespace bl {

struct Layer;
class Document;

struct Rect {
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
};

// 一个 BBOX 投影到目标图层坐标系后的形状。
// region 是原始参数（位移单位 = owner 图层像素）；k 把位移和长度从 owner
// 换算到目标层。
struct BBoxView {
    Layer* owner = nullptr;
    Region* region = nullptr;
    double cx = 0.0;
    double cy = 0.0;
    double rx = 0.0;
    double ry = 0.0;
    double rot = 0.0;
    double k = 1.0;

    double disp_x() const { return region->disp_x * k; }
    double disp_y() const { return region->disp_y * k; }
};

// 图层子树的独立快照：资产键共享，BBOX 与套索深拷贝。
struct LayerClip {
    std::string name;
    std::string path;
    int asset_key = 0;
    int w = 0;
    int h = 0;
    double x = 0.0;
    double y = 0.0;
    double scale = 1.0;
    double sx = 1.0;  // 非等比变换（Ctrl+T 解锁比例时才有值）
    double sy = 1.0;
    double rot = 0.0;  // 图层旋转（弧度，绕中心；Ctrl+T 旋转才有值）
    bool visible = true;
    // true = 粘贴时落在自己的原位（套索抠图件）；false = 跟光标/画布中心。
    bool in_place = false;
    // 套索抠出来的小块四周带透明补边（像素数）；变形手柄要按去掉补边的内容框画
    int pad = 0;
    std::vector<Region> bboxes;
    std::vector<LayerClip> children;
};

struct Layer {
    int uid = 0;
    std::string name;   // 面板标题 = 图片文件名
    std::string path;   // 源文件路径
    int asset_key = 0;  // 资产缓存键（纹理/缩略图/alpha 掩码）
    int w = 0;
    int h = 0;
    double x = 0.0;  // 图层中心的世界坐标
    double y = 0.0;
    double scale = 1.0;  // 等比缩放（滚轮、子层联动都走它）
    double sx = 1.0;     // 横/纵附加倍率：Ctrl+T 解除比例时才偏离 1
    double sy = 1.0;
    double rot = 0.0;    // 绕中心的旋转（弧度，Ctrl+T 旋转才有值）
    bool visible = true;
    // 位图四周的透明补边（只有套索抠出来的小块非 0）：变形手柄/命中按内容框算，
    // 位图本身仍带补边，缩小采样时不至于从边缘混进黑。
    int pad = 0;
    // 套索不挂在图层上（它属于场景，路径存世界坐标，目标由可见性 + 层序现推）
    std::vector<std::unique_ptr<Region>> bboxes;
    std::vector<std::unique_ptr<Layer>> children;
    Layer* parent = nullptr;

    // ---- 坐标换算 ----
    double scale_x() const;  // scale * sx，横向像素→世界
    double scale_y() const;  // scale * sy
    std::pair<double, double> to_world(double px, double py) const;
    std::pair<double, double> from_world(double wx, double wy) const;
    std::pair<double, double> map_point_from(const Layer& src, double px,
                                             double py) const;
    double len_factor_from(const Layer& src) const;
    Rect world_rect() const;
    bool local_contains(double px, double py) const;

    // ---- 变换（调用方负责撤销快照/标脏）----
    void move_by(double dx, double dy);
    void set_scale(double value);
    void reset_scale();

    // ---- 树查询 ----
    std::vector<Layer*> ancestors() const;
    std::vector<Layer*> descendants() const;
    bool is_visible_chain() const;
    Layer* root() const;
    std::vector<BBoxView> bbox_views() const;

    LayerClip copy_snapshot() const;
};

// 整棵图层树 + 结构操作。Region 与 Lasso 归 Layer 所有。
class Document {
public:
    Document() = default;
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    // 深拷贝（撤销快照与导出共用）：uid 保留，父子指针重接。
    std::unique_ptr<Document> clone() const;

    // ---- 查询 ----
    std::vector<Layer*> flatten() const;  // 全部图层，底 → 顶（父先于子）
    Layer* find(int uid) const;
    bool find_region(int region_uid, Layer** layer, Region** region) const;
    Layer* layer_of(const Region* region) const;
    std::vector<Region*> all_regions() const;
    // 参与遮罩/呼吸位移的 BBOX（自己的眼睛开 + 所属图层链可见）；预览与导出共用
    std::vector<Region*> visible_regions() const;
    int num_regions() const;

    // ---- 结构操作（调用方随后 refresh_regions + 撤销快照）----
    std::unique_ptr<Layer> new_layer(const std::string& name,
                                     const std::string& path, int asset_key,
                                     int w, int h, double x, double y);
    Layer* add_layer_top(std::unique_ptr<Layer> layer);
    // 把快照粘贴到 (x, y)，整棵子树保持相对位置。
    // skipped = 因 BBOX 上限被跳过的数量。
    std::pair<Layer*, int> add_layer_copy(const LayerClip& clip, double x,
                                          double y);
    // 删除图层及挂在它身上的 BBOX；子图层提升到原位置。
    // promoted 收到被提升的子图层；返回值是该图层的所有权（调用方可丢弃）。
    std::unique_ptr<Layer> remove_layer(Layer* layer,
                                        std::vector<Layer*>* promoted);
    // 排序/收养。index 是移除后目标兄弟表里的最终位置。
    // new_parent == nullptr 表示根层级；禁止挂到自己的子树下。
    bool move_layer(Layer* layer, Layer* new_parent, int index);
    int sibling_index(const Layer* layer) const;

    // ---- BBOX ----
    Region* add_bbox(Layer* layer, double cx, double cy);
    void remove_bbox(Layer* layer, Region* region);
    bool move_bbox(Layer* layer, Region* region, int index);
    bool move_bbox_to(Layer* owner, Region* region, Layer* target, int index);

    // ---- 边界 ----
    std::optional<Rect> world_bounds(bool visible_only = true) const;

    // 唯一的兄弟表访问口径（roots 与 children 二选一）
    std::vector<std::unique_ptr<Layer>>& siblings_of(const Layer* layer);
    const std::vector<std::unique_ptr<Layer>>& siblings_of(
        const Layer* layer) const;

    std::vector<std::unique_ptr<Layer>>& roots() { return roots_; }
    const std::vector<std::unique_ptr<Layer>>& roots() const { return roots_; }

    // ---- 画布 ----
    // 世界坐标原点就是画布中心；导出范围、可见边界、中心参考线都以它为准。
    // 默认 1080×1920，本次运行第一次拖入图片时改成该图尺寸（见 Scene::add_layer）。
    int canvas_w = 1080;
    int canvas_h = 1920;
    Rect canvas_rect() const;

private:
    std::unique_ptr<Layer> detach(Layer* layer);

    std::vector<std::unique_ptr<Layer>> roots_;  // 末位在最上
    int next_uid_ = 1;
    int next_bbox_ = 1;
};

}  // namespace bl
