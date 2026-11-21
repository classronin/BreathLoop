#include "domain/selection.h"

#include <algorithm>

namespace bl {

Layer* Selection::layer() const {
    return picks.empty() ? nullptr : picks.back().layer;
}

Region* Selection::region() const {
    return picks.empty() ? nullptr : picks.back().region;
}

Region* Selection::sole_region() const {
    // 手柄只在这种时候出现
    return picks.size() == 1 ? picks[0].region : nullptr;
}

bool Selection::multi() const {
    return picks.size() > 1;
}

std::vector<Layer*> Selection::layers() const {
    std::vector<Layer*> out;
    for (const Pick& pick : picks) {
        if (pick.region == nullptr) out.push_back(pick.layer);
    }
    return out;
}

std::vector<Pick> Selection::regions() const {
    std::vector<Pick> out;
    for (const Pick& pick : picks) {
        if (pick.region != nullptr) out.push_back(pick);
    }
    return out;
}

bool Selection::has_layer(const Layer* layer) const {
    for (const Pick& pick : picks) {
        if (pick.layer == layer && pick.region == nullptr) return true;
    }
    return false;
}

bool Selection::has_region(const Region* region) const {
    for (const Pick& pick : picks) {
        if (pick.region == region) return true;
    }
    return false;
}

int Selection::index_of(const Layer* layer, const Region* region) const {
    for (size_t i = 0; i < picks.size(); ++i) {
        if (picks[i].layer == layer && picks[i].region == region) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void Selection::clear() {
    picks.clear();
}

void Selection::only(Layer* layer, Region* region) {
    picks.clear();
    picks.push_back(Pick{layer, region});
}

void Selection::add(Layer* layer, Region* region) {
    if (index_of(layer, region) < 0) picks.push_back(Pick{layer, region});
}

void Selection::toggle(Layer* layer, Region* region) {
    const int index = index_of(layer, region);
    if (index < 0) {
        picks.push_back(Pick{layer, region});
    } else {
        picks.erase(picks.begin() + index);
    }
}

void Selection::drop_layer(const Layer* layer) {
    std::vector<int> gone{layer->uid};
    for (Layer* node : layer->descendants()) gone.push_back(node->uid);
    picks.erase(std::remove_if(picks.begin(), picks.end(), [&gone](const Pick& pick) {
                    return std::find(gone.begin(), gone.end(), pick.layer->uid) !=
                           gone.end();
                }),
                picks.end());
}

void Selection::drop_region(const Region* region) {
    picks.erase(std::remove_if(picks.begin(), picks.end(),
                               [region](const Pick& pick) {
                                   return pick.region == region;
                               }),
                picks.end());
}

void Selection::select_all(const Document& doc) {
    // BBOX 排在前面、图层排在后面，主选因此落在图层上：全选后 Ctrl+C
    // 复制的是图片，整体拖动走的也是整层（BBOX 随层，不重复计）。
    picks.clear();
    std::vector<Layer*> visible;
    for (Layer* layer : doc.flatten()) {
        if (layer->is_visible_chain()) visible.push_back(layer);
    }
    for (Layer* layer : visible) {
        for (const auto& region : layer->bboxes) {
            picks.push_back(Pick{layer, region.get()});
        }
    }
    for (Layer* layer : visible) picks.push_back(Pick{layer, nullptr});
}

std::vector<std::pair<int, int>> Selection::uids() const {
    std::vector<std::pair<int, int>> out;
    for (const Pick& pick : picks) {
        out.emplace_back(pick.layer->uid, pick.region == nullptr ? 0 : pick.region->uid);
    }
    return out;
}

void Selection::resolve(const Document& doc,
                        const std::vector<std::pair<int, int>>& uids) {
    std::vector<Pick> out;
    for (const auto& item : uids) {
        if (item.second == 0) {
            if (Layer* layer = doc.find(item.first)) out.push_back(Pick{layer, nullptr});
            continue;
        }
        Layer* layer = nullptr;
        Region* region = nullptr;
        if (doc.find_region(item.second, &layer, &region)) {
            out.push_back(Pick{layer, region});
        }
    }
    picks = out;
}

}  // namespace bl
