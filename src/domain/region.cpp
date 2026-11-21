#include "domain/region.h"

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "nlohmann/json.hpp"
#include "presets_embedded.h"

namespace bl {
namespace {

int g_uid_seq = 1;

// presets.json 里合法可写的字段名（实测成员集合，不含运行时输出与身份）
constexpr std::array<const char*, 11> kDefaultKeys{
    "amp", "speed", "cx",  "cy",     "rx",    "ry",
    "rot", "aniso_x", "aniso_y", "zeta", "freq"};

double numeric(const nlohmann::json& object, const char* key, const char* section) {
    auto it = object.find(key);
    if (it == object.end()) {
        throw std::runtime_error(std::string("presets.json ") + section +
                                 " 缺字段: " + key);
    }
    if (!it->is_number()) {
        throw std::runtime_error(std::string("presets.json ") + section +
                                 " 字段类型错误: " + key + " 期望数值");
    }
    return it->get<double>();
}

}  // namespace

double* member_by_key(Region& region, const std::string& key) {
    if (key == "amp") return &region.amp;
    if (key == "speed") return &region.speed;
    if (key == "cx") return &region.cx;
    if (key == "cy") return &region.cy;
    if (key == "rx") return &region.rx;
    if (key == "ry") return &region.ry;
    if (key == "rot") return &region.rot;
    if (key == "aniso_x") return &region.aniso_x;
    if (key == "aniso_y") return &region.aniso_y;
    if (key == "zeta") return &region.zeta;
    if (key == "freq") return &region.freq;
    return nullptr;
}

Presets::Presets() {
    nlohmann::json data;
    try {
        data = nlohmann::json::parse(kPresetsJson);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("出厂预设文件损坏: ") + e.what());
    }
    auto defaults = data.find("defaults");
    if (defaults == data.end() || !defaults->is_object()) {
        throw std::runtime_error("presets.json 缺 defaults 段");
    }
    for (const char* key : kDefaultKeys) {
        defaults_.emplace_back(key, numeric(*defaults, key, "defaults"));
    }
    auto parts = data.find("presets");
    if (parts == data.end()) return;
    if (!parts->is_object()) {
        throw std::runtime_error("presets.json 的 presets 段不是对象");
    }
    for (auto it = parts->begin(); it != parts->end(); ++it) {
        if (!it->is_object()) {
            throw std::runtime_error("presets.json presets." + it.key() +
                                     " 不是对象");
        }
        std::vector<std::pair<std::string, double>> values;
        for (auto inner = it->begin(); inner != it->end(); ++inner) {
            bool known = false;
            for (const char* key : kDefaultKeys) {
                if (inner.key() == key) {
                    known = true;
                    break;
                }
            }
            if (!known) {
                throw std::runtime_error("presets.json presets." + it.key() +
                                         " 含未知字段: " + inner.key());
            }
            if (!inner->is_number()) {
                throw std::runtime_error("presets.json presets." + it.key() + "." +
                                         inner.key() + " 期望数值");
            }
            values.emplace_back(inner.key(), inner->get<double>());
        }
        presets_.emplace_back(it.key(), std::move(values));
    }
}

const Presets& Presets::get() {
    static const Presets instance;
    return instance;
}

double Presets::default_value(const std::string& key) const {
    for (const auto& item : defaults_) {
        if (item.first == key) return item.second;
    }
    throw std::runtime_error("presets.json defaults 缺字段: " + key);
}

const std::vector<std::pair<std::string, double>>* Presets::preset(
    const std::string& name) const {
    for (const auto& item : presets_) {
        if (item.first == name) return &item.second;
    }
    return nullptr;
}

Region Region::make_default() {
    const Presets& presets = Presets::get();
    Region region;
    region.amp = presets.default_value("amp");
    region.speed = presets.default_value("speed");
    region.cx = presets.default_value("cx");
    region.cy = presets.default_value("cy");
    region.rx = presets.default_value("rx");
    region.ry = presets.default_value("ry");
    region.rot = presets.default_value("rot");
    region.aniso_x = presets.default_value("aniso_x");
    region.aniso_y = presets.default_value("aniso_y");
    region.zeta = presets.default_value("zeta");
    region.freq = presets.default_value("freq");
    region.uid = g_uid_seq++;
    return region;
}

void Region::copy_params_from(const Region& other) {
    amp = other.amp;
    speed = other.speed;
    rx = other.rx;
    ry = other.ry;
    rot = other.rot;
    aniso_x = other.aniso_x;
    aniso_y = other.aniso_y;
    zeta = other.zeta;
    freq = other.freq;
}

void Region::copy_sync_params_from(const Region& other) {
    amp = other.amp;
    speed = other.speed;
}

void Region::apply_preset(const std::string& name) {
    const auto* values = Presets::get().preset(name);
    if (values == nullptr) return;
    for (const auto& item : *values) {
        if (double* target = member_by_key(*this, item.first)) {
            *target = item.second;
        }
    }
}

void Region::reset_shape() {
    const Presets& presets = Presets::get();
    rx = presets.default_value("rx");
    ry = presets.default_value("ry");
    rot = presets.default_value("rot");
}

std::pair<double, double> Region::to_local(double px, double py) const {
    double dx = px - cx;
    double dy = py - cy;
    if (rot != 0.0) {
        const double a = rot * 3.14159265358979323846 / 180.0;
        const double c = std::cos(a);
        const double s = std::sin(a);
        const double nx = c * dx + s * dy;
        const double ny = -s * dx + c * dy;
        dx = nx;
        dy = ny;
    }
    return {dx, dy};
}

std::pair<double, double> Region::to_world(double lx, double ly) const {
    if (rot != 0.0) {
        const double a = rot * 3.14159265358979323846 / 180.0;
        const double c = std::cos(a);
        const double s = std::sin(a);
        const double nx = lx * c - ly * s;
        const double ny = lx * s + ly * c;
        lx = nx;
        ly = ny;
    }
    return {lx, ly};
}

bool Region::contains(double px, double py) const {
    const std::pair<double, double> local = to_local(px, py);
    const double nx = local.first / (rx > 1e-6 ? rx : 1e-6);
    const double ny = local.second / (ry > 1e-6 ? ry : 1e-6);
    return nx * nx + ny * ny <= 1.0;
}

}  // namespace bl
