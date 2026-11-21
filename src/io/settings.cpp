#include "io/settings.h"

#include <cstdio>
#include <fstream>
#include <string>

#include "io/paths.h"
#include "nlohmann/json.hpp"
#include "ui/param_panel.h"
#include "ui/scene.h"

namespace bl::io {
namespace {

const char kFile[] = "breathloop_settings.json";

nlohmann::json PanelJson(const ui::PanelFloat& panel) {
    nlohmann::json out;
    out["side"] = panel.side;
    out["w"] = panel.w;
    out["h"] = panel.h;
    out["left"] = panel.left;
    out["bottom"] = panel.bottom;
    out["floating"] = panel.floating;
    return out;
}

void ReadPanel(const nlohmann::json& node, ui::PanelFloat* panel) {
    if (!node.is_object()) return;
    if (node.contains("side") && node["side"].is_string()) {
        const std::string side = node["side"].get<std::string>();
        if (side == "left" || side == "right") panel->side = side;
    }
    if (node.contains("w") && node["w"].is_number())
        panel->w = node["w"].get<float>();
    if (node.contains("h") && node["h"].is_number())
        panel->h = node["h"].get<float>();
    if (node.contains("left") && node["left"].is_number())
        panel->left = node["left"].get<float>();
    if (node.contains("bottom") && node["bottom"].is_number())
        panel->bottom = node["bottom"].get<float>();
    if (node.contains("floating") && node["floating"].is_boolean())
        panel->floating = node["floating"].get<bool>();
}

}  // namespace

void SaveSettings(ui::Scene& scene) {
    nlohmann::json root;
    root["panels"]["param"] = PanelJson(scene.panel_float("param"));
    root["panels"]["layer"] = PanelJson(scene.panel_float("layer"));
    root["collapsed"]["left"] = scene.is_collapsed("left");
    root["collapsed"]["right"] = scene.is_collapsed("right");
    root["float_order"] = scene.float_order_state();
    root["cards"] = scene.param_panel != nullptr
                        ? nlohmann::json(scene.param_panel->expansion_state())
                        : nlohmann::json::object();

    nlohmann::json app;
    app["bbox_limit"] = scene.state.bbox_limit;
    app["frames"] = scene.state.frames;
    app["ms_per_frame"] = scene.state.ms_per_frame;
    app["export_lossless"] = scene.state.export_lossless;
    app["export_quality"] = scene.state.export_quality;
    app["export_effort"] = scene.state.export_effort;
    app["export_alpha_q"] = scene.state.export_alpha_q;
    app["export_w"] = scene.state.export_w;
    app["export_h"] = scene.state.export_h;
    app["export_keep_ratio"] = scene.state.export_keep_ratio;
    app["export_size_auto"] = scene.state.export_size_auto;
    app["eraser_diameter"] = scene.state.eraser_diameter;
    app["eraser_feather"] = scene.state.eraser_feather;
    app["view_x"] = scene.view.x;
    app["view_y"] = scene.view.y;
    app["view_scale"] = scene.view.scale;
    root["app"] = app;

    std::ofstream file(paths::Utf8Path(paths::BesideExe(kFile)), std::ios::binary);
    if (!file) return;
    file << root.dump(2);
}

void LoadSettings(ui::Scene& scene) {
    std::ifstream file(paths::Utf8Path(paths::BesideExe(kFile)), std::ios::binary);
    if (!file) return;
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(file);
    } catch (const std::exception&) {
        return;
    }
    if (!root.is_object()) return;

    // nlohmann 的 get<int>() 在类型不符时抛 type_error（不是返回默认值）：
    // 旧版本或手改过的设置文件会把启动打成崩溃，所以读取整体兜住。
    try {
        if (root.contains("panels") && root["panels"].is_object()) {
        if (root["panels"].contains("param"))
            ReadPanel(root["panels"]["param"], &scene.panel_float("param"));
        if (root["panels"].contains("layer"))
            ReadPanel(root["panels"]["layer"], &scene.panel_float("layer"));
    }

    bool left = false;
    bool right = false;
    if (root.contains("collapsed") && root["collapsed"].is_object()) {
        const auto& node = root["collapsed"];
        if (node.contains("left") && node["left"].is_boolean())
            left = node["left"].get<bool>();
        if (node.contains("right") && node["right"].is_boolean())
            right = node["right"].get<bool>();
    }
    scene.apply_saved_collapsed(left, right);

    if (root.contains("float_order") && root["float_order"].is_array()) {
        scene.set_float_order(root["float_order"].get<std::vector<std::string>>());
    }
    if (root.contains("cards") && root["cards"].is_object()
        && scene.param_panel != nullptr) {
        std::map<std::string, bool> expansion;
        for (auto it = root["cards"].begin(); it != root["cards"].end(); ++it) {
            if (it.value().is_boolean())
                expansion[it.key()] = it.value().get<bool>();
        }
        scene.param_panel->set_expansion(expansion);
    }

    if (root.contains("app") && root["app"].is_object()) {
        const auto& app = root["app"];
        if (app.contains("bbox_limit") && app["bbox_limit"].is_number_integer()) {
            // BBOX 上限可手改：夹在 1..kMaxRegions 之间，越界的值不收。
            const int n = app["bbox_limit"].get<int>();
            scene.state.bbox_limit =
                n < 1 ? 1 : (n > kMaxRegions ? kMaxRegions : n);
        }
        if (app.contains("frames") && app["frames"].is_number_integer())
            scene.state.frames = app["frames"].get<int>();
        if (app.contains("ms_per_frame")
            && app["ms_per_frame"].is_number())
            scene.state.ms_per_frame = app["ms_per_frame"].get<double>();
        if (app.contains("export_lossless"))
            scene.state.export_lossless = app["export_lossless"].get<bool>();
        if (app.contains("export_quality"))
            scene.state.export_quality = app["export_quality"].get<int>();
        if (app.contains("export_effort"))
            scene.state.export_effort = app["export_effort"].get<int>();
        if (app.contains("export_alpha_q"))
            scene.state.export_alpha_q = app["export_alpha_q"].get<int>();
        if (app.contains("export_w"))
            scene.state.export_w = app["export_w"].get<int>();
        if (app.contains("export_h"))
            scene.state.export_h = app["export_h"].get<int>();
        if (app.contains("export_keep_ratio"))
            scene.state.export_keep_ratio = app["export_keep_ratio"].get<bool>();
        if (app.contains("export_size_auto"))
            scene.state.export_size_auto = app["export_size_auto"].get<bool>();
        if (app.contains("eraser_diameter")) {
            // 直径上限已收到 500：旧存档可能是更早的 3000 / 1000 口径
            const int d = app["eraser_diameter"].get<int>();
            scene.state.eraser_diameter = d < 2 ? 2 : (d > 500 ? 500 : d);
        }
        if (app.contains("eraser_feather")) {
            // 羽化已改成百分比 0–100：旧存档里可能是像素值，读进来先夹一次
            const int f = app["eraser_feather"].get<int>();
            scene.state.eraser_feather = f < 0 ? 0 : (f > 100 ? 100 : f);
        }
        // eraser_strength（不透明度）这一档已经删了：旧存档里的这个键直接忽略
        // eraser_on 不读：工具状态每次启动都是关，旧存档里的这个键直接忽略
        if (app.contains("view_x")) scene.view.x = app["view_x"].get<double>();
        if (app.contains("view_y")) scene.view.y = app["view_y"].get<double>();
        if (app.contains("view_scale"))
            scene.view.scale = app["view_scale"].get<double>();
    }

    scene.relayout(scene.height());
    scene.rebuild_panels();
    } catch (const std::exception&) {
        return;
    }
}

WindowPlacement LoadWindowPlacement() {
    WindowPlacement out;
    std::ifstream file(paths::Utf8Path(paths::BesideExe(kFile)), std::ios::binary);
    if (!file) return out;
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(file);
    } catch (const std::exception&) {
        return out;
    }
    if (!root.is_object()) return out;
    if (!root.contains("window") || !root["window"].is_object()) return out;
    try {
        const auto& node = root["window"];
        if (!node.contains("w") || !node.contains("h")) return out;
        if (!node["w"].is_number_integer() || !node["h"].is_number_integer())
            return out;
        const int w = node["w"].get<int>();
        const int h = node["h"].get<int>();
        if (w < 200 || h < 200) return out;   // 坏值：当作没存过
        out.w = w;
        out.h = h;
        if (node.contains("x") && node["x"].is_number_integer())
            out.x = node["x"].get<int>();
        if (node.contains("y") && node["y"].is_number_integer())
            out.y = node["y"].get<int>();
        out.maximized = node.contains("maximized") && node["maximized"].is_boolean()
                            ? node["maximized"].get<bool>()
                            : false;
        out.valid = true;
    } catch (const std::exception&) {
        return WindowPlacement{};
    }
    return out;
}

void SaveWindowPlacement(int x, int y, int w, int h, bool maximized) {
    // 在 SaveSettings 之后调用：把窗口那一块并进刚写好的文件，别覆盖别的字段。
    nlohmann::json root = nlohmann::json::object();
    {
        std::ifstream file(paths::Utf8Path(paths::BesideExe(kFile)), std::ios::binary);
        if (file) {
            try {
                root = nlohmann::json::parse(file);
            } catch (const std::exception&) {
                root = nlohmann::json::object();
            }
        }
    }
    if (!root.is_object()) root = nlohmann::json::object();
    nlohmann::json node;
    node["x"] = x;
    node["y"] = y;
    node["w"] = w;
    node["h"] = h;
    node["maximized"] = maximized;
    root["window"] = node;

    std::ofstream file(paths::Utf8Path(paths::BesideExe(kFile)), std::ios::binary);
    if (!file) return;
    file << root.dump(2);
}

}  // namespace bl::io
