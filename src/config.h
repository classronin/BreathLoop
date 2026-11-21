// 全局可调常量：窗口、面板、颜色、参数范围、交互步进。
// 这里只放用户可能改的值。算法常量与架构契约就近放在使用它们的模块里：
//   MAX_REGIONS / SYNC_PARAM_KEYS -> domain/region.h
//   MIN_POINTS                    -> domain/lasso.h
//   PHYS_POISSON                  -> render/shaders.h
//   MASK_MAX_SIDE / FIELD_BIAS    -> render/mask.h
//   CYCLE_SECONDS / PHYS_SUBSTEP  -> domain/seamless.h
//   PHYS_REBUILD_INTERVAL         -> domain/state.h
//   EXPORT_SUBSTEPS / KMAX / KMIN -> render/exporter.h
#pragma once

#include <array>

namespace cfg {

struct Rgba {
    unsigned char r, g, b, a;
};

constexpr Rgba FromRgb(unsigned char r, unsigned char g, unsigned char b) {
    return Rgba{r, g, b, 255};
}

// ---- 窗口 ----
constexpr int WIN_W = 1280;
constexpr int WIN_H = 720;
constexpr int MIN_W = 1200;
constexpr int MIN_H = 720;
constexpr char WINDOW_TITLE[] = "BreathLoop - ClassRonin";
constexpr int GUI_ORDER = 4;

// ---- 面板 ----
constexpr int PANEL_W = 300;          // 参数面板默认宽度（停靠时恢复）
constexpr int LAYER_PANEL_W = 320;    // 图层面板默认宽度（停靠时恢复）
constexpr int COLLAPSED_W = 16;       // 面板收起后的竖条
constexpr int LAYER_ROW_H = 80;
constexpr int BBOX_ROW_H = 40;
constexpr int THUMB_SIZE = LAYER_ROW_H - 4;   // 行高上下各留 2px
constexpr int LAYER_INDENT = 14;
constexpr int PANEL_HEADER_H = 28;
constexpr int PANEL_PAD = 8;
constexpr int CARD_HEAD_H = 32;       // 卡片标题行：折叠后整张卡只剩这一行
constexpr int CARD_GAP = 10;          // 相邻卡片之间的间隔
constexpr int PANEL_FLOAT_MARGIN = 40;
constexpr int PANEL_DOCK_SNAP = 24;
constexpr int PANEL_DRAG_MIN = 4;
constexpr int PANEL_MIN_W = 260;
constexpr int PANEL_RESIZE_EDGE = 5;
constexpr int PANEL_MIN_FLOAT_H = 180;
constexpr int CANVAS_MIN_W = 240;

// ---- 字号（唯一出处）----
// 界面文字只声明角色，不再写点号。改一张表就能整体缩放。
//   Body  正文：控件、状态行、中心顶部、卡片摘要
//   Title 标题：面板标题、卡片标题（惯例粗体）
//   Small 小字：图层名、占位提示、行内小符号
//   Icon  行内图标：眼睛 / + / 重置这类纯符号，跟着图层行高一起放大
//   IconSmall BBOX 行眼睛：行高 40，装不下 53px 的大图标
enum class FontRole { Body, Title, Small, Icon, IconSmall };
constexpr int FONT_BODY_PT = 12;
constexpr int FONT_TITLE_PT = 11;
constexpr int FONT_SMALL_PT = 10;
constexpr int FONT_ICON_PT = 40;
constexpr int FONT_ICON_SMALL_PT = 24;
constexpr int FontPointOf(FontRole role) {
    switch (role) {
        case FontRole::Body:
            return FONT_BODY_PT;
        case FontRole::Title:
            return FONT_TITLE_PT;
        case FontRole::Icon:
            return FONT_ICON_PT;
        case FontRole::IconSmall:
            return FONT_ICON_SMALL_PT;
        case FontRole::Small:
        default:
            return FONT_SMALL_PT;
    }
}

// ---- 主题（Blender 风格深色）----
constexpr Rgba BG{24, 24, 24, 255};
constexpr Rgba PANEL_BG{32, 32, 32, 255};
constexpr Rgba BAR_BG{20, 20, 20, 255};
constexpr Rgba BAR_BG_HOVER{40, 40, 40, 255};
constexpr Rgba DIVIDER{15, 15, 15, 255};
constexpr Rgba CARD_BG{42, 42, 42, 255};
constexpr Rgba TRACK{15, 15, 15, 255};
constexpr Rgba FILL{71, 114, 179, 255};
constexpr Rgba KNOB{160, 160, 160, 255};
constexpr Rgba TEXT{255, 255, 255, 255};
constexpr Rgba TEXT_DIM{140, 140, 140, 255};
constexpr Rgba STATUS_TEXT{255, 255, 255, 255};
constexpr Rgba ENTRY_BG{15, 15, 15, 255};
constexpr Rgba DANGER{200, 80, 80, 255};
constexpr Rgba DISABLED{90, 90, 90, 255};

constexpr Rgba BTN_GRAY{60, 60, 60, 255};
constexpr Rgba BTN_GRAY_HOVER{80, 80, 80, 255};
constexpr Rgba BTN_ORANGE{200, 120, 50, 255};
constexpr Rgba BTN_ORANGE_HOVER{220, 140, 70, 255};
constexpr Rgba BTN_SUBTLE{50, 50, 50, 255};
constexpr Rgba BTN_SUBTLE_HOVER{70, 70, 70, 255};

constexpr Rgba ROW_HOVER{45, 45, 45, 255};
constexpr Rgba ROW_SEL{55, 72, 98, 255};
constexpr Rgba DROP_LINE{100, 160, 240, 255};
constexpr Rgba PANEL_BORDER{80, 80, 80, 255};

// ---- 画布 ----
constexpr int CANVAS_DEFAULT_W = 1080;
constexpr int CANVAS_DEFAULT_H = 1920;
// 调色对齐 E:\GitHub\C\ImageEditor\src\nodes\Style.h:26-28：
// canvas 是画布外底色（48,48,54），checkA/checkB 是画布内棋盘（58,58,66 / 70,70,80）。
constexpr Rgba CANVAS_DARK{48, 48, 54, 255};       // 画布外
constexpr Rgba CANVAS_ALPHA_A{58, 58, 66, 255};    // 画布内棋盘格
constexpr Rgba CANVAS_ALPHA_B{70, 70, 80, 255};
constexpr double CANVAS_CHECK_CELL = 16.0;        // 棋盘格边长（屏幕像素）
constexpr Rgba CANVAS_EDGE{255, 255, 255, 255};   // 画布边界：1px 白线
constexpr Rgba GUIDE_LINE{190, 120, 255, 255};    // 中心参考线：1px 紫线
constexpr double GUIDE_SNAP_PX = 2.0;             // 吸附判定（屏幕像素）

constexpr Rgba LASSO_LINE{255, 100, 100, 200};
constexpr Rgba LASSO_DOT{255, 100, 100, 255};
constexpr Rgba LASSO_ANT{255, 190, 90, 235};
constexpr Rgba LASSO_ANT_SELECTED{255, 235, 150, 255};

constexpr std::array<Rgba, 16> REGION_COLORS{{
    FromRgb(79, 150, 220),  FromRgb(220, 150, 70),  FromRgb(150, 100, 180),
    FromRgb(200, 80, 80),   FromRgb(80, 180, 100),  FromRgb(200, 180, 60),
    FromRgb(70, 160, 160),  FromRgb(140, 80, 160),  FromRgb(170, 120, 90),
    FromRgb(90, 200, 170),  FromRgb(190, 90, 130),  FromRgb(110, 130, 200),
    FromRgb(200, 210, 80),  FromRgb(80, 200, 210),  FromRgb(150, 90, 200),
    FromRgb(210, 160, 90),
}};

// ---- 字体 ----
constexpr char FONT_FAMILY[] = "Microsoft YaHei";

// ---- 交互 / 拖放 ----
constexpr double DOUBLE_CLICK_SECONDS = 0.35;
constexpr double DROP_ZONE_EDGE = 0.3;   // 行顶/底 30% = 同级排序，中间 = 收为子层
constexpr double ROW_DRAG_MIN = 6.0;
constexpr double WHEEL_LAYER = 1.1;
constexpr int DRAG_GHOST_ALPHA = 185;

// ---- 数值范围 ----
constexpr double AMP_MIN = 0.0, AMP_MAX = 100.0;
constexpr double SPEED_MIN = 1.0, SPEED_MAX = 100.0;
constexpr double RADIUS_MIN = 5.0;
constexpr double ANISO_MIN = 0.0, ANISO_MAX = 1.0;
constexpr double ZETA_MIN = 0.0, ZETA_MAX = 1.0;
constexpr double FREQ_MIN = 0.5, FREQ_MAX = 8.0;

constexpr int FRAMES_MIN = 10, FRAMES_MAX = 300;
constexpr int DEFAULT_FRAMES = 60;
constexpr std::array<int, 9> FPS_PRESETS{120, 60, 48, 30, 25, 24, 18, 15, 12};

constexpr int EXPORT_QUALITY_MAX = 100;
constexpr double EXPORT_EFFORT_MAX = 6.0;
constexpr int EXPORT_ALPHA_Q_MAX = 100;

constexpr bool EXPORT_DEFAULT_LOSSLESS = true;
constexpr int EXPORT_DEFAULT_QUALITY = 100;
constexpr int EXPORT_DEFAULT_EFFORT = 0;
constexpr int EXPORT_DEFAULT_ALPHA_Q = 100;

// ---- 画布 ----
constexpr double ZOOM_MIN = 0.1, ZOOM_MAX = 10.0;
constexpr double WHEEL_VIEW = 1.1;
constexpr double FIT_MARGIN = 0.95;
constexpr double LAYER_SCALE_MIN = 0.05;
constexpr double LAYER_SCALE_MAX = 20.0;
constexpr int UNDO_LIMIT = 50;
constexpr double HANDLE_RADIUS = 6.0;
constexpr double HANDLE_ROT_GAP = 20.0;
constexpr double HANDLE_HIT_RADIUS = 10.0;
constexpr int BBOX_OPACITY = 60;
constexpr int BBOX_OPACITY_SELECTED = 110;
constexpr double ALPHA_HIT = 8.0;

// ---- 套索 ----
constexpr double LASSO_FEATHER = 12.0;
constexpr double CUTOUT_PAD = 24.0;
constexpr double MASK_PREVIEW_DETAIL = 2.0;
constexpr int LASSO_THICKNESS = 2;
constexpr double LASSO_DOT_R = 3.0;

constexpr double LASSO_ANT_DASH = 6.0;
constexpr double LASSO_ANT_GAP = 4.0;
constexpr double LASSO_ANT_SPEED = 30.0;

constexpr double SCROLL_STEP = 30.0;
constexpr double DEFAULT_MS_PER_FRAME = 33.3;
constexpr double FPS_REFRESH_INTERVAL = 0.5;
// 界面刷新上限：垂直同步在部分机器/远程桌面下不生效，靠它兜住 CPU 占用
constexpr double MAX_FPS = 60.0;

}  // namespace cfg
