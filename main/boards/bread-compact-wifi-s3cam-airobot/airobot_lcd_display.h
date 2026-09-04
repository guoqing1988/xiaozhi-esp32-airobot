// 板级显示扩展：待机大时钟 HH:MM:SS（AI 可经 self.clock.set / self.clock.theme 工具控制）
// 标准子类化模式（参考 boards/zhengchen/1.54tft-wifi/zhengchen_lcd_display.h），
// 不改核心 display 代码，仅在本板目录扩展。
//
// 时钟字体：内嵌 DSEG7 七段数码管子集字体（0-9/冒号/减号，见本目录
// clock_dseg7.c）。三档字号 40/56/76px，SetupUI 时按屏宽自动选最大放得下的
// （170px 屏用 40、240px 屏用 56、320px 以上用 76），日期固定 18px 小字。
// 数据是 const 数组放 flash(.rodata)，LVGL 按需从 flash 读取 bitmap，运行时不占 RAM。
#ifndef AIROBOT_LCD_DISPLAY_H
#define AIROBOT_LCD_DISPLAY_H

#include "display/lcd_display.h"
#include "lvgl_theme.h"

#include <esp_lvgl_port.h>
#include <lvgl.h>

#include <string>

// 字体 C 文件按 C 编译, 这里按 C linkage 声明(宏自动匹配 const)
extern "C" {
LV_FONT_DECLARE(clock_dseg7_40);
LV_FONT_DECLARE(clock_dseg7_56);
LV_FONT_DECLARE(clock_dseg7_76);
LV_FONT_DECLARE(clock_dseg7_18);  // 日期小字(YYYY-MM-DD)
}

class AirobotLcdDisplay : public SpiLcdDisplay {
protected:
    lv_obj_t* clock_label_ = nullptr;      // 时间 HH:MM:SS(大字)
    lv_obj_t* date_label_ = nullptr;       // 日期 YYYY-MM-DD(小字, 时间上方)
    const lv_font_t* clock_font_ = nullptr;  // 按屏宽选中的时间字体
    std::string clock_time_text_;      // 最近一次显示的时间(避免重复刷新)
    std::string clock_date_text_;      // 最近一次显示的日期
    int screen_width_ = 0;             // 屏宽(构造时传入, 用于选字号)
    bool clock_shown_ = false;         // 当前是否在显示时钟(用于显隐切换防抖)
    bool clock_dark_bg_ = true;        // true=黑底白字, false=白底黑字(AI 可切换)

public:
    AirobotLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                      int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                      bool swap_xy)
        : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy),
          screen_width_(swap_xy ? height : width) {}  // 注意 XY 交换后屏宽取 height

    // 按屏宽选最大放得下的 HH:MM:SS 字体：小屏字小、大屏字大
    const lv_font_t* PickClockFont() const {
        static const lv_font_t* kCandidates[] = {&clock_dseg7_40, &clock_dseg7_56, &clock_dseg7_76};
        const lv_font_t* picked = kCandidates[0];
        for (auto* f : kCandidates) {
            lv_point_t sz = {};
            lv_text_get_size(&sz, "88:88", f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            if (sz.x <= screen_width_ - 8) {
                picked = f;
            } else {
                break;  // 字号升序, 放不下后面也放不下
            }
        }
        return picked;
    }

    // 标准钩子：Application 初始化完成后统一调用 SetupUI()，
    // 先让基类建好状态栏/聊天区，再叠加时间(大字)与日期(小字)两个标签。
    void SetupUI() override {
        SpiLcdDisplay::SetupUI();
        auto* screen = lv_screen_active();

        clock_font_ = PickClockFont();  // 小屏(170px)→40px 字, 大屏(240px)→56px, 320px+→76px
        const int gap = 8;  // 日期与时间的垂直间距

        // 时间大字：屏幕中央偏下(给上方日期留位), 整体"日期+间距+时间"垂直居中
        clock_label_ = lv_label_create(screen);
        lv_obj_set_style_text_font(clock_label_, clock_font_, 0);
        lv_obj_align(clock_label_, LV_ALIGN_CENTER, 0, (clock_dseg7_18.line_height + gap) / 2);
        lv_obj_add_flag(clock_label_, LV_OBJ_FLAG_HIDDEN);  // 默认隐藏

        // 日期小字：时间正上方
        date_label_ = lv_label_create(screen);
        lv_obj_set_style_text_font(date_label_, &clock_dseg7_18, 0);
        lv_obj_align(date_label_, LV_ALIGN_CENTER, 0, -(clock_font_->line_height + gap) / 2);
        lv_obj_add_flag(date_label_, LV_OBJ_FLAG_HIDDEN);
    }

    // 时钟背景主题：黑底白字(dark) / 白底黑字(浅色)。由板级调用, 设置持久化在 NVS。
    void SetClockTheme(bool dark_bg) {
        clock_dark_bg_ = dark_bg;
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
            lv_obj_remove_flag(date_label_, LV_OBJ_FLAG_HIDDEN);
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
    // 时钟主题色: 黑底白字 / 白底黑字。屏幕底色 + 内容容器底色 + 时间/日期文字色。
    // 幂等, 每秒调用一次开销可忽略; 主题切换(SetClockTheme)立即生效。
    void ApplyClockThemeColors() {
        uint32_t bg = clock_dark_bg_ ? 0x000000 : 0xffffff;
        uint32_t fg = clock_dark_bg_ ? 0xffffff : 0x000000;
        lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(bg), 0);
        if (container_ != nullptr) {
            lv_obj_set_style_bg_color(container_, lv_color_hex(bg), 0);
        }
        lv_obj_set_style_text_color(clock_label_, lv_color_hex(fg), 0);
        lv_obj_set_style_text_color(date_label_, lv_color_hex(fg), 0);
    }
};

#endif  // AIROBOT_LCD_DISPLAY_H
