// 选择集合：按点选顺序记录「图层 / BBOX」，单选只是它只有一个元素的特例。
//
// 一个 pick 有两种：(图层, nullptr) 表示整层（连同它上面的 BBOX 一起动），
// (图层, BBOX) 表示只动那一个 BBOX。画布命中、面板行、覆盖层高亮都按这两
// 种判断，不再各写一份「谁被选中」。
//
// 比对一律用身份（指针）而不是值：两份字段相同的图层/BBOX 会「相等」，
// 而集合要的恰恰是「就是它」。
//
// 集合存的是活对象：撤销会深拷贝整棵文档树，对象身份随之换掉，所以恢复后
// 要按 uid 把 pick 重新指回新对象（resolve）。
#pragma once

#include <utility>
#include <vector>

#include "domain/layers.h"
#include "domain/region.h"

namespace bl {

struct Pick {
    Layer* layer = nullptr;
    Region* region = nullptr;  // nullptr = 整层
};

class Selection {
public:
    std::vector<Pick> picks;

    // ---- 读 ----
    Layer* layer() const;                 // 主选图层：最后点中的那一项
    Region* region() const;               // 主选的 BBOX（可能空）
    Region* sole_region() const;          // 只选了一个 BBOX 时才是它
    bool multi() const;                   // 多于一个 pick
    std::vector<Layer*> layers() const;   // 只要整层的那几个
    std::vector<Pick> regions() const;    // 只要 BBOX 的那几个

    bool has_layer(const Layer* layer) const;
    bool has_region(const Region* region) const;
    int index_of(const Layer* layer, const Region* region) const;

    // ---- 写 ----
    void clear();
    void only(Layer* layer, Region* region = nullptr);
    void add(Layer* layer, Region* region = nullptr);
    void toggle(Layer* layer, Region* region = nullptr);  // Ctrl+点击
    // 图层被删掉了：它和它的子孙名下的 pick 一起消失
    void drop_layer(const Layer* layer);
    void drop_region(const Region* region);
    // 全选：每个可见图层，连同它名下的每个 BBOX
    void select_all(const Document& doc);

    // ---- 撤销后 ----
    // region uid 为 0 表示整层（对应 Python 的 None）。
    std::vector<std::pair<int, int>> uids() const;
    void resolve(const Document& doc, const std::vector<std::pair<int, int>>& uids);
};

}  // namespace bl
