// 画布顶部居中的全局状态提示：四向黑描边 + 主文字。
//
// 与导出卡片里的局部状态分开：载入、撤销、模式切换等全局反馈走这里，
// 导出进度仍留在导出卡片内。
#pragma once

#include <string>

namespace bl::ui {

class Scene;
class Painter;

class CenterStatus {
public:
    void init(Scene* scene);
    void set(const std::string& text);
    void relayout(int height);
    void draw(Painter& painter);

    std::string text;

private:
    Scene* scene_ = nullptr;
    float x_ = 0.0f;
    float y_ = 0.0f;
};

}  // namespace bl::ui
