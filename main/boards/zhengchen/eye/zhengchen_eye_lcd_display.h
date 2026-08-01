#ifndef ZHENGCHEN_EYE_LCD_DISPLAY_H
#define ZHENGCHEN_EYE_LCD_DISPLAY_H

#include "display/lcd_display.h"
#include <esp_lvgl_port.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <font_emoji.h>

#include <atomic>
#include <memory>

class ZhengchenEyeLcdDisplay : public SpiLcdDisplay {
public:
    // 基类中的SetupUI不是虚方法，依旧会调用SpiLcdDisplay的setupui，所以不必自定义构造函数
    // ZhengchenEyeLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
    //                       int width, int height, int offset_x, int offset_y,
    //                       bool mirror_x, bool mirror_y, bool swap_xy);
    // 使用基类构造函数
    using SpiLcdDisplay::SpiLcdDisplay;
    // 重写需要定制的方法
    void SetupUI();
    void SetEmotion(const char* emotion) override;
    void UpdateStatusBar(bool update_all = false) override;
    lv_obj_t* overlay_container = nullptr;
    lv_obj_t* gif_label_ = nullptr;
};

#endif // LCD_DISPLAY_H


