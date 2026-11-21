// 橡皮：一笔 = 一张覆盖率遮罩（0..255），落笔时按"取最大值"叠加，最后一次性
// 乘在图层 alpha 上。坐标一律用"位图像素、自顶向下"的行序，调用方负责把图层的
// 局部坐标（y 向上）翻过来。
//
// 为什么不直接在每个印上改 alpha：逐印相乘会让重叠处越擦越透，羽化带被自己
// 的下一印吃掉，结果就成了硬边，而且效果随拖动速度/印间距变化（PS 里不会）。
// 取最大值保证"同一笔内无论覆盖多少次，某像素最终覆盖率只由它离笔迹边缘最近
// 的那一次决定"，所以软边稳定、与速度无关；跨笔擦除才继续累积。
#pragma once

#include <vector>

namespace bl {

// 往覆盖率遮罩盖一个圆形印：圆心 (cx,cy)，半径 radius（像素），羽化带宽
// feather（像素，0 = 硬边；等于 radius 时从圆心开始过渡）。每个像素取
// max(旧覆盖率, 本印覆盖率)。返回本印覆盖到的包围盒（x0,y0,x1,y1，右/下开
// 区间），并且只有真的有像素覆盖率变大时才返回 true；半径整个在图像外返回
// false。
bool stamp_coverage(std::vector<unsigned char>& cov, int w, int h, double cx,
                    double cy, double radius, double feather, int* x0, int* y0,
                    int* x1, int* y1);

}  // namespace bl
