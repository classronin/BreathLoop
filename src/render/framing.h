// 取景框：可见图层的非透明内容并集。
//
// 导出取景和导出卡的自动尺寸共用这一个口径：都按画布上的实际落点/缩放算，
// 所以自动尺寸下内容永远 1:1 填满输出，不会被塞进一个更大的冻结画布里。
#pragma once

#include "domain/layers.h"
#include "render/assets.h"

namespace bl {

struct ContentFrame {
    double x = 0.0;
    double y = 0.0;
    double w = 0.0;
    double h = 0.0;
};

// 当前画布占地的并集 (x, y, w, h)；没有可见图层抛 std::runtime_error。
ContentFrame content_frame(const Document& doc, AssetStore& assets);

}  // namespace bl
