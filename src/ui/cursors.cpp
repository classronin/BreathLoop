#include "ui/cursors.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <GLFW/glfw3.h>

namespace bl::ui {
namespace {

constexpr int kSize = 32;
constexpr int kHotX = 14;
constexpr int kHotY = 18;

struct Shape {
    int ellipse;   // 1 = 椭圆，0 = 胶囊（线段 + 半径）
    float a, b, c, d, e, f;
};

const Shape kOpen[] = {
    {1, 15.5f, 20.0f, 8.2f, 7.0f, 0.0f, 0.0f},
    {0, 10.0f, 16.0f, 8.5f, 5.0f, 2.3f, 0.0f},
    {0, 14.0f, 15.0f, 13.5f, 4.0f, 2.3f, 0.0f},
    {0, 18.0f, 15.0f, 19.0f, 4.5f, 2.3f, 0.0f},
    {0, 21.5f, 17.0f, 23.0f, 7.5f, 2.2f, 0.0f},
    {0, 9.0f, 21.0f, 4.5f, 15.5f, 2.6f, 0.0f},
};

const Shape kClosed[] = {
    {1, 15.5f, 20.5f, 8.8f, 6.8f, 0.0f, 0.0f},
    {0, 10.0f, 17.0f, 10.5f, 12.5f, 2.3f, 0.0f},
    {0, 14.0f, 17.0f, 14.0f, 11.5f, 2.4f, 0.0f},
    {0, 18.0f, 17.0f, 18.5f, 12.0f, 2.3f, 0.0f},
    {0, 21.5f, 18.0f, 22.0f, 13.5f, 2.1f, 0.0f},
    {0, 9.5f, 21.5f, 4.5f, 16.5f, 2.8f, 0.0f},
};

float SegmentDistance(float px, float py, float ax, float ay, float bx,
                      float by) {
    const float pax = px - ax;
    const float pay = py - ay;
    const float bax = bx - ax;
    const float bay = by - ay;
    const float denom = bax * bax + bay * bay;
    float h = denom == 0.0f ? 0.0f : (pax * bax + pay * bay) / denom;
    h = std::min(std::max(h, 0.0f), 1.0f);
    return std::sqrt((pax - bax * h) * (pax - bax * h)
                     + (pay - bay * h) * (pay - bay * h));
}

float Distance(const Shape& shape, float x, float y) {
    if (shape.ellipse) {
        const float d = std::sqrt(((x - shape.a) / shape.c)
                                      * ((x - shape.a) / shape.c)
                                  + ((y - shape.b) / shape.d)
                                        * ((y - shape.b) / shape.d));
        return (d - 1.0f) * std::min(shape.c, shape.d);
    }
    return SegmentDistance(x, y, shape.a, shape.b, shape.c, shape.d) - shape.e;
}

// 双箭头：一条主杆 + 两端各一对 V 形箭头（胶囊近似）
std::vector<Shape> DoubleArrow(bool nwse) {
    std::vector<Shape> shapes;
    const float cx = 16.0f;
    const float cy = 16.0f;
    const float dx = nwse ? 9.0f : 9.0f;
    const float dy = nwse ? 9.0f : -9.0f;
    shapes.push_back({0, cx - dx, cy + dy, cx + dx, cy - dy, 1.4f, 0.0f});
    const float tips[2][2] = {{cx - dx, cy + dy}, {cx + dx, cy - dy}};
    for (const auto& tip : tips) {
        const float ux = nwse ? 1.0f : 1.0f;
        const float uy = nwse ? -1.0f : 1.0f;
        shapes.push_back({0, tip[0], tip[1], tip[0] + ux * 4.5f,
                          tip[1] + uy * 4.5f, 1.4f, 0.0f});
        shapes.push_back({0, tip[0], tip[1], tip[0] + 4.5f, tip[1] - uy * 4.5f,
                          1.4f, 0.0f});
    }
    return shapes;
}

// 旋转：一段开口圆环 + 一个 V 形箭头，和 Blender/ImageEditor 的旋转手势同义。
std::vector<Shape> RotateShape() {
    std::vector<Shape> shapes;
    const float cx = 16.0f;
    const float cy = 16.0f;
    const float r = 8.5f;
    const float start = -2.2f;
    const float sweep = 4.8f;
    const int steps = 12;
    for (int i = 0; i < steps; ++i) {
        const float a0 = start + sweep * static_cast<float>(i) / steps;
        const float a1 = start + sweep * static_cast<float>(i + 1) / steps;
        shapes.push_back({0, cx + r * std::cos(a0), cy + r * std::sin(a0),
                          cx + r * std::cos(a1), cy + r * std::sin(a1), 1.6f,
                          0.0f});
    }
    const float a = start + sweep;
    const float tip_x = cx + r * std::cos(a);
    const float tip_y = cy + r * std::sin(a);
    // 箭头两翼沿切线方向张开（切线 = 弧线在该点的方向）
    const float tx = -std::sin(a);
    const float ty = std::cos(a);
    shapes.push_back({0, tip_x, tip_y, tip_x + tx * 4.2f - ty * 3.0f,
                      tip_y + ty * 4.2f + tx * 3.0f, 1.4f, 0.0f});
    shapes.push_back({0, tip_x, tip_y, tip_x - tx * 2.2f + ty * 3.6f,
                      tip_y - ty * 2.2f - tx * 3.6f, 1.4f, 0.0f});
    return shapes;
}

GLFWcursor* BuildCursor(GLFWwindow* window, const std::vector<Shape>& shapes,
                        int hot_x, int hot_y) {
    (void)window;
    std::vector<unsigned char> pixels(
        static_cast<size_t>(kSize) * kSize * 4, 0);
    for (int row = 0; row < kSize; ++row) {
        const float y = static_cast<float>(kSize - 1 - row) + 0.5f;
        for (int col = 0; col < kSize; ++col) {
            const float x = static_cast<float>(col) + 0.5f;
            float d = 1e9f;
            for (const Shape& shape : shapes)
                d = std::min(d, Distance(shape, x, y));
            unsigned char r = 24, g = 24, b = 24, a = 0;
            if (d <= 0.0f) {
                // 内芯纯白：线宽只有 1.4px，描边必须只长在外侧，
                // 否则深色环会把白芯整个吃掉，看起来就是"一团黑边"。
                r = 248;
                g = 248;
                b = 248;
                a = 255;
            } else if (d <= 0.8f) {
                a = 200;   // 0.8px 外描边
            } else {
                const float alpha = std::max(0.0f, 1.0f - (d - 0.8f) / 0.8f);
                a = static_cast<unsigned char>(std::round(170.0f * alpha));
                if (a == 0) continue;
            }
            const size_t at =
                (static_cast<size_t>(row) * kSize + col) * 4;
            pixels[at] = r;
            pixels[at + 1] = g;
            pixels[at + 2] = b;
            pixels[at + 3] = a;
        }
    }
    GLFWimage image;
    image.width = kSize;
    image.height = kSize;
    image.pixels = pixels.data();
    return glfwCreateCursor(&image, hot_x, hot_y);
}

}  // namespace

void SceneCursors::init(GLFWwindow* window) {
    window_ = window;
    grab_ = BuildCursor(window, std::vector<Shape>(kOpen,
                                                   kOpen + 6), kHotX, kHotY);
    grabbing_ = BuildCursor(window, std::vector<Shape>(kClosed,
                                                       kClosed + 6), kHotX,
                            kHotY);
    size_h_ = glfwCreateStandardCursor(GLFW_HRESIZE_CURSOR);
    size_v_ = glfwCreateStandardCursor(GLFW_VRESIZE_CURSOR);
    size_nwse_ = BuildCursor(window, DoubleArrow(true), 16, 16);
    size_nesw_ = BuildCursor(window, DoubleArrow(false), 16, 16);
    rotate_ = BuildCursor(window, RotateShape(), 16, 16);
}

void SceneCursors::destroy() {
    if (grab_ != nullptr) glfwDestroyCursor(grab_);
    if (grabbing_ != nullptr) glfwDestroyCursor(grabbing_);
    if (size_h_ != nullptr) glfwDestroyCursor(size_h_);
    if (size_v_ != nullptr) glfwDestroyCursor(size_v_);
    if (size_nwse_ != nullptr) glfwDestroyCursor(size_nwse_);
    if (size_nesw_ != nullptr) glfwDestroyCursor(size_nesw_);
    if (rotate_ != nullptr) glfwDestroyCursor(rotate_);
    grab_ = grabbing_ = size_h_ = size_v_ = size_nwse_ = size_nesw_ = nullptr;
    rotate_ = nullptr;
}

void SceneCursors::apply(const std::string& name) {
    if (window_ == nullptr) return;
    if (applied_ && name == current_) return;
    applied_ = true;
    current_ = name;
    GLFWcursor* cursor = nullptr;
    if (name == "grab") cursor = grab_;
    else if (name == "grabbing") cursor = grabbing_;
    else if (name == "size_h") cursor = size_h_;
    else if (name == "size_v") cursor = size_v_;
    else if (name == "size_nwse") cursor = size_nwse_;
    else if (name == "size_nesw") cursor = size_nesw_;
    else if (name == "rotate") cursor = rotate_;
    glfwSetCursor(window_, cursor);
}

}  // namespace bl::ui
