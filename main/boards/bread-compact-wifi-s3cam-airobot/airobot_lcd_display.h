// 板级显示扩展：待机大时钟（AI 可经 self.clock.set 工具开关）
// 标准子类化模式（参考 boards/zhengchen/1.54tft-wifi/zhengchen_lcd_display.h），
// 不改核心 display 代码，仅在本板目录扩展。
#ifndef AIROBOT_LCD_DISPLAY_H
#define AIROBOT_LCD_DISPLAY_H

#include "display/lcd_display.h"

#include <esp_lvgl_port.h>

#include <string>

class AirobotLcdDisplay : public SpiLcdDisplay {
protected:
    lv_obj_t* clock_label_ = nullptr;
    std::string clock_time_text_;  // 最近一次显示的时间文本（避免每秒重复刷新）

public:
    using SpiLcdDisplay::SpiLcdDisplay;  // 继承构造

    // 标准钩子：Application 初始化完成后统一调用 SetupUI()，
    // 先让基类建好状态栏/聊天区，再叠加一个大号时间标签
    void SetupUI() override {
        SpiLcdDisplay::SetupUI();
        clock_label_ = lv_label_create(lv_screen_active());
#if CONFIG_LV_FONT_MONTSERRAT_48
        // 大字号数字字体（config.json 的 sdkconfig_append 开启；
        // 未开启时退回默认 14px 字体，仍可编译运行）
        lv_obj_set_style_text_font(clock_label_, &lv_font_montserrat_48, 0);
#endif
        lv_obj_set_style_text_color(clock_label_, lv_color_white(), 0);
        lv_obj_align(clock_label_, LV_ALIGN_CENTER, 0, -20);  // 上移避开底部字幕条(IP/歌词)
        lv_obj_add_flag(clock_label_, LV_OBJ_FLAG_HIDDEN);    // 默认隐藏
    }

    // 由板级 1 秒 esp_timer 调用（非 LVGL 任务，内部自锁）
    // visible=false 隐藏；time_text 非空且与上次不同才更新文本（省 SPI 刷屏）
    void UpdateClock(bool visible, const char* time_text) {
        lvgl_port_lock(-1);
        if (clock_label_ == nullptr) {
            lvgl_port_unlock();
            return;
        }
        if (visible) {
            if (time_text != nullptr && time_text != clock_time_text_) {
                clock_time_text_ = time_text;
                lv_label_set_text(clock_label_, time_text);
            }
            lv_obj_remove_flag(clock_label_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(clock_label_, LV_OBJ_FLAG_HIDDEN);
        }
        lvgl_port_unlock();
    }
};

#endif  // AIROBOT_LCD_DISPLAY_H
