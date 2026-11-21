// 画布光标：空格拖动的手型、面板边缘的缩放箭头。
//
// 手型与斜向双箭头是软件栅格化的 32×32 位图（与 Python 版同一套形状数据），
// 正/横向缩放直接借系统光标。
#pragma once

#include <string>

struct GLFWcursor;
struct GLFWwindow;

namespace bl::ui {

class SceneCursors {
public:
    void init(GLFWwindow* window);
    void destroy();
    void apply(const std::string& name);   // "" = 默认箭头

private:
    GLFWwindow* window_ = nullptr;
    GLFWcursor* grab_ = nullptr;
    GLFWcursor* grabbing_ = nullptr;
    GLFWcursor* size_h_ = nullptr;
    GLFWcursor* size_v_ = nullptr;
    GLFWcursor* size_nwse_ = nullptr;
    GLFWcursor* size_nesw_ = nullptr;
    GLFWcursor* rotate_ = nullptr;
    std::string current_;
    bool applied_ = false;
};

}  // namespace bl::ui
