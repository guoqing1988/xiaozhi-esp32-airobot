// 板级显示扩展：待机大时钟（AI 可经 self.clock.set 工具开关）
// 标准子类化模式（参考 boards/zhengchen/1.54tft-wifi/zhengchen_lcd_display.h），
// 不改核心 display 代码，仅在本板目录扩展。
//
// 时钟字体：内嵌 DSEG7 七段数码管子集字体（仅 0-9/冒号/减号，见本目录
// clock_dseg7_76.c / clock_dseg7_18.c）。数据是 const 数组放 flash(.rodata)，
// LVGL 按需从 flash 读取 bitmap，运行时不占 RAM。
#ifndef AIROBOT_LCD_DISPLAY_H
#define AIROBOT_LCD_DISPLAY_H

#include "display/lcd_display.h"
#include "lvgl_theme.h"

#include <esp_lvgl_port.h>
#include <lvgl.h>

#include <string>

// 字体 C 文件按 C 编译, 这里按 C linkage 声明(宏自动匹配 const)
extern "C" {
LV_FONT_DECLARE(clock_dseg7_76);  // 时间大字(高度 76px, 七段数码管)
LV_FONT_DECLARE(clock_dseg7_18);  // 日期小字(YYYY-MM-DD)
}

class AirobotLcdDisplay : public SpiLcdDisplay {
protected:
    lv_obj_t* clock_label_ = nullptr;  // 时间 HH:MM(大字)
    lv_obj_t* date_label_ = nullptr;   // 日期 YYYY-MM-DD(小字, 时间上方)
    std::string clock_time_text_;      // 最近一次显示的时间(避免每秒重复刷新)
    std::string clock_date_text_;      // 最近一次显示的日期
    bool clock_shown_ = false;         // 当前是否在显示时钟(用于显隐切换防抖)

public:
    using SpiLcdDisplay::SpiLcdDisplay;  // 继承构造

    // 标准钩子：Application 初始化完成后统一调用 SetupUI()，
    // 先让基类建好状态栏/聊天区，再叠加时间(大字)与日期(小字)两个标签。
    void SetupUI() override {
        SpiLcdDisplay::SetupUI();
        auto* screen = lv_screen_active();

        // 时间大字：屏幕中央(中央偏下约 8px, 给上方日期留位), 数字宽 ~47px,
        // 整条 "HH:MM" 约 215px, 240px 宽屏正好放下, 视觉占主导
        clock_label_ = lv_label_create(screen);
        lv_obj_set_style_text_font(clock_label_, &clock_dseg7_76, 0);
        lv_obj_align(clock_label_, LV_ALIGN_CENTER, 0, 8);
        lv_obj_add_flag(clock_label_, LV_OBJ_FLAG_HIDDEN);  // 默认隐藏

        // 日期小字：时间正上方
        date_label_ = lv_label_create(screen);
        lv_obj_set_style_text_font(date_label_, &clock_dseg7_18, 0);
        lv_obj_align(date_label_, LV_ALIGN_CENTER, 0, -44);
        lv_obj_add_flag(date_label_, LV_OBJ_FLAG_HIDDEN);
    }

    // 由板级 1 秒 esp_timer 调用（非 LVGL 任务，内部自锁）
    // visible=false 隐藏；time_text/date_text 非空且与上次不同才更新文本(省 SPI 刷屏)。
    // 时钟显示时把中央表情/聊天区(emoji_box_)一并隐藏，屏幕只留"日期+时间"，更有真时钟感。
    void UpdateClock(bool visible, const char* time_text, const char* date_text) {
        lvgl_port_lock(-1);
        if (clock_label_ == nullptr || date_label_ == nullptr) {
            lvgl_port_unlock();
            return;
        }
        if (visible) {
            // 状态由"关"变"开"时遮住表情/聊天内容
            if (!clock_shown_) {
                if (emoji_box_ != nullptr) {
                    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
                }
                clock_shown_ = true;
            }
            // 文字颜色跟随当前主题(浅色底深字/深色底白字)，与其它 UI 文字一致；
            // 主题切换(SetTheme)不重建时钟 label，故文本变化时同步一次主题色
            auto* theme = static_cast<LvglTheme*>(current_theme_);
            if (time_text != nullptr && time_text != clock_time_text_) {
                clock_time_text_ = time_text;
                if (theme != nullptr) {
                    lv_obj_set_style_text_color(clock_label_, theme->text_color(), 0);
                }
                lv_label_set_text(clock_label_, time_text);
            }
            if (date_text != nullptr && date_text != clock_date_text_) {
                clock_date_text_ = date_text;
                if (theme != nullptr) {
                    lv_obj_set_style_text_color(date_label_, theme->text_color(), 0);
                }
                lv_label_set_text(date_label_, date_text);
            }
            lv_obj_remove_flag(clock_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(date_label_, LV_OBJ_FLAG_HIDDEN);
        } else {
            if (clock_shown_) {
                if (emoji_box_ != nullptr) {
                    lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
                }
                clock_shown_ = false;
            }
            lv_obj_add_flag(clock_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(date_label_, LV_OBJ_FLAG_HIDDEN);
        }
        lvgl_port_unlock();
    }
};

#endif  // AIROBOT_LCD_DISPLAY_H
