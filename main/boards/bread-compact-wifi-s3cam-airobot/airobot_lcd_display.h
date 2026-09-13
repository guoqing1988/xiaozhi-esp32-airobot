// 板级显示扩展：待机全屏大时钟（AI 可经 self.clock.set / self.clock.theme / self.clock.current 工具控制）
// 标准子类化模式（参考 boards/zhengchen/1.54tft-wifi/zhengchen_lcd_display.h），
// 不改核心 display 代码，仅在本板目录扩展。
//
// 设计要点：
// 1) 字体：内嵌 Bebas Neue（Google Fonts 开源，SIL OFL）高瘦数字字体（本目录 clock_bebas_130/60/48.c 为时间大字，
//    clock_bebas_date.c 为日期小字）。均按 lv_font_conv --bpp 2 生成（抗锯齿），**数字等宽**——任何时间 HH:MM 宽度
//    恒定，不会因窄数字(如 '1')让时间变窄、两侧留白忽大忽小，"始终占满两边"。高瘦字形，同屏宽能上更大字号、字更高。
// 2) 多屏自适应：PickClockFont() 按编译期确定的屏幕宽度(DISPLAY_WIDTH)从最大往下选能放下 HH:MM 的字号，
//    覆盖 240x240 / 240x320 / 128x160 等不同屏幕，无需逐屏硬编码。
// 3) 主题：kClockThemes[] 仅 2 套（经典黑底白字 / 白底黑字）。AI 经 self.clock.theme 循环切换（0..1），NVS 持久化。
//    仅保留「黑底白字 / 白底黑字」两种高对比经典模式，其余(Catppuccin 等)纯颜色变化已移除。
// 4) 全屏：待机时背景填满屏幕主题色，中央大字 HH:MM，上方日期 YYYY-MM-DD，
//    隐藏状态栏/字幕/表情区，更像真实电子钟；对话/聆听/播放自动恢复原 UI。
#ifndef AIROBOT_LCD_DISPLAY_H
#define AIROBOT_LCD_DISPLAY_H

#include "display/lcd_display.h"
#include "lvgl_theme.h"

#include <esp_lvgl_port.h>
#include <lvgl.h>

#include <string>

// 字体档位宏门控：按屏幕分辨率(DISPLAY_WIDTH)只声明启用的档，与 clock_bebas_*.c 同步
#include "clock_fonts_config.h"

// 字体 C 文件按 C 编译, 这里按 C linkage 声明(宏自动匹配 const)
extern "C" {
#if CLOCK_FONT_ENABLE_130
LV_FONT_DECLARE(clock_bebas_130);
#endif
#if CLOCK_FONT_ENABLE_60
LV_FONT_DECLARE(clock_bebas_60);
#endif
LV_FONT_DECLARE(clock_bebas_48);  // 最小兜底档(恒启用)
LV_FONT_DECLARE(clock_bebas_date);  // 日期小字(YYYY-MM-DD), 恒启用
}

// 时钟主题表：bg=屏幕背景, fg=时间/文字, accent=日期/点缀
struct ClockTheme {
    uint32_t bg;
    uint32_t fg;
    uint32_t accent;
};

// 注意：索引即 NVS 保存的 theme 值（0=黑底白字，1=白底黑字）。仅保留两种经典模式，
// 其余(Catppuccin 等)均为纯颜色变化、无实际意义，已移除。
static const ClockTheme kClockThemes[] = {
    {0x000000, 0xFFFFFF, 0x00E5FF},  // 0 黑底白字(经典)
    {0xFFFFFF, 0x000000, 0xE53935},  // 1 白底黑字
};
static constexpr int kClockThemeCount =
    static_cast<int>(sizeof(kClockThemes) / sizeof(kClockThemes[0]));

class AirobotLcdDisplay : public SpiLcdDisplay {
protected:
    lv_obj_t* clock_label_ = nullptr;      // 时间 HH:MM(大字)
    lv_obj_t* date_label_ = nullptr;       // 日期 YYYY-MM-DD(小字, 时间上方)
    const lv_font_t* clock_font_ = nullptr;  // 按屏幕逻辑宽选中的时间字体
    std::string clock_time_text_;      // 最近一次显示的时间(避免重复刷新)
    std::string clock_date_text_;      // 最近一次显示的日期
    int clock_theme_id_ = 0;           // 当前主题索引(0..kClockThemeCount-1)
    bool clock_shown_ = false;         // 当前是否在显示时钟(用于显隐切换防抖)

public:
    AirobotLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                      int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                      bool swap_xy)
        : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                        swap_xy) {}

    // 按屏幕"逻辑横向像素"选最大放得下的 HH:MM 字体。
    // 用编译期确定值 DISPLAY_WIDTH(而非运行时 lv_obj_get_width, 避免 SetupUI 早期
    // screen 尺寸未定导致误判过早落到小档)判断，从大往小选。
    // 字体为 Bebas Neue(Goolge Fonts 开源, 高瘦长方形), **数字等宽**——无论显示几点 HH:MM 宽度恒定，
    // 不会因窄数字(如 '1')让时间变窄、两侧留白忽大忽小，"始终占满两边"。
    //   240x240/240x320(240宽)->130px（88:88≈232px，两侧仅余 3.8px，字高约 93px，高瘦占满、更显高大）
    //   128x160 横屏(160宽)->48px（≈86px，可再加大）
    const lv_font_t* PickClockFont() const {
        // 130px 在 240 宽屏实测 88:88≈232px，左右各余 3.8px。
        // 判据边距用 4px（DISPLAY_WIDTH-4=236px）：130px(232px) 放得下，132px(236px) 会被拒，锁定 130px。
        const int avail = DISPLAY_WIDTH - 4;
        // 只包含当前分辨率启用的档(见 clock_fonts_config.h)，从大往小排；130px 为 240 屏主档
        static const lv_font_t* kCandidates[] = {
#if CLOCK_FONT_ENABLE_130
            &clock_bebas_130,
#endif
#if CLOCK_FONT_ENABLE_60
            &clock_bebas_60,
#endif
            &clock_bebas_48,  // 最小兜底档(恒启用)
        };
        for (const lv_font_t* f : kCandidates) {
            lv_point_t sz = {};
            lv_text_get_size(&sz, "88:88", f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            if (sz.x <= avail) {
                return f;  // 从大到小，第一个放得下的就是最大档
            }
        }
        return &clock_bebas_48;  // 兜底(恒启用)
    }

    // 标准钩子：Application 初始化完成后统一调用 SetupUI()，
    // 先让基类建好状态栏/聊天区，再叠加时间(大字)与日期(小字)两个标签。
    void SetupUI() override {
        SpiLcdDisplay::SetupUI();
        lv_obj_t* screen = lv_screen_active();

        clock_font_ = PickClockFont();  // 按屏幕逻辑宽自动选档
        const int gap = 8;              // 日期与时间的垂直间距

        // 时间大字：铺满屏幕宽度并水平居中，整体"日期+间距+时间"垂直居中
        clock_label_ = lv_label_create(screen);
        lv_obj_set_style_text_font(clock_label_, clock_font_, 0);
        lv_obj_set_style_text_align(clock_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(clock_label_, lv_obj_get_width(screen));  // 撑满横向
        const int time_lh = clock_font_->line_height;
        // 日期已隐藏(见 UpdateClock 的说明)，时间改为垂直居中。只改坐标、不动对象树/内存分配。
        lv_obj_align(clock_label_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(clock_label_, LV_OBJ_FLAG_HIDDEN);  // 默认隐藏

        // 日期小字：时间正上方
        date_label_ = lv_label_create(screen);
        lv_obj_set_style_text_font(date_label_, &clock_bebas_date, 0);
        lv_obj_set_style_text_align(date_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(date_label_, lv_obj_get_width(screen));
        lv_obj_align(date_label_, LV_ALIGN_CENTER, 0, -(time_lh + gap) / 2);
        lv_obj_add_flag(date_label_, LV_OBJ_FLAG_HIDDEN);
    }

    // 设置时钟主题(索引 0..kClockThemeCount-1)。由板级调用, 设置持久化在 NVS。
    void SetClockTheme(int theme_id) {
        if (theme_id < 0) theme_id = 0;
        if (theme_id >= kClockThemeCount) theme_id = kClockThemeCount - 1;
        clock_theme_id_ = theme_id;
        if (clock_shown_) {
            ApplyClockThemeColors();
        }
    }

    // 由板级 1 秒 esp_timer 调用（非 LVGL 任务，内部自锁）
    // visible=false 隐藏；time_text/date_text 非空且与上次不同才更新文本(省 SPI 刷屏)。
    // 时钟显示时把状态栏/字幕条/表情区一并隐藏并把屏幕底色换成时钟主题色, 更像真实电子钟。
    void UpdateClock(bool visible, const char* time_text, const char* date_text) {
        lvgl_port_lock(-1);
        if (clock_label_ == nullptr || date_label_ == nullptr) {
            lvgl_port_unlock();
            return;
        }
        if (visible) {
            // 状态由"关"变"开"时遮住状态栏/字幕/表情, 只留"日期+时间"
            if (!clock_shown_) {
                if (emoji_box_ != nullptr) {
                    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
                }
                if (top_bar_ != nullptr) {
                    lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
                }
                if (bottom_bar_ != nullptr) {
                    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
                }
                clock_shown_ = true;
            }
            ApplyClockThemeColors();
            if (time_text != nullptr && time_text != clock_time_text_) {
                clock_time_text_ = time_text;
                lv_label_set_text(clock_label_, time_text);
            }
            if (date_text != nullptr && date_text != clock_date_text_) {
                clock_date_text_ = date_text;
                lv_label_set_text(date_label_, date_text);
            }
            lv_obj_remove_flag(clock_label_, LV_OBJ_FLAG_HIDDEN);
            // 注意：**不要**在这里 remove_flag(date_label_)——日期小字按要求不显示。
            // 保留 date_label_ 的创建与文本更新(保持 LVGL 对象数量/堆分配/display 级
            // layout 回调注册与历史版本完全一致)，仅让它始终停留在 SetupUI 设的 HIDDEN 状态。
            // 教训：曾试图改删掉 date_label_ 对象来去日期，改变了对象树与堆布局，
            // 触发 LVGL 上游 label 的 display 级回调(update_layout_completed_cb,
            // lv_label.c:1076)对非法对象的 lv_label_refr_text 而 无限重启(LoadProhibited,
            // 崩在 is_transformed/lv_obj_pos.c:1347)。去 UI 元素请只改可见性/坐标，勿删对象。
        } else {
            if (clock_shown_) {
                if (emoji_box_ != nullptr) {
                    lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
                }
                if (top_bar_ != nullptr) {
                    lv_obj_remove_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
                }
                if (bottom_bar_ != nullptr) {
                    lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
                }
                clock_shown_ = false;
            }
            // 恢复基类主题底色(时钟主题色只覆盖在显示期间)
            auto* theme = static_cast<LvglTheme*>(current_theme_);
            if (theme != nullptr) {
                lv_obj_set_style_bg_color(lv_screen_active(), theme->background_color(), 0);
                if (container_ != nullptr) {
                    lv_obj_set_style_bg_color(container_, theme->background_color(), 0);
                }
            }
            lv_obj_add_flag(clock_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(date_label_, LV_OBJ_FLAG_HIDDEN);
        }
        lvgl_port_unlock();
    }

private:
    // 应用当前主题色：屏幕底色 + 内容容器底色 + 时间/日期文字色。
    // 幂等, 每秒调用一次开销可忽略; 主题切换(SetClockTheme)立即生效。
    void ApplyClockThemeColors() {
        const ClockTheme& t = kClockThemes[clock_theme_id_];
        lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(t.bg), 0);
        if (container_ != nullptr) {
            lv_obj_set_style_bg_color(container_, lv_color_hex(t.bg), 0);
        }
        lv_obj_set_style_text_color(clock_label_, lv_color_hex(t.fg), 0);
        lv_obj_set_style_text_color(date_label_, lv_color_hex(t.accent), 0);
    }
};

#endif  // AIROBOT_LCD_DISPLAY_H
