#ifndef OLED_DISPLAY_H
#define OLED_DISPLAY_H

#include "lvgl_display.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

// 屏幕类型枚举
enum ScreenType {
    SCREEN_128X64,
    SCREEN_128X32
};

class OledDisplay : public LvglDisplay {
private:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* content_left_ = nullptr;
    lv_obj_t* content_right_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t *emotion_label_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;

    // 添加用于显示天气和时间信息的标签
    lv_obj_t* idle_labels_[8] = {nullptr}; // 待机信息标签
    // 翻页参数
    int current_idle_page_ = 0;
    int total_idle_pages_ = 0;
    int items_per_page_ = 2;  // 每页显示2条信息
    // 存储待机信息的成员变量
    std::string idle_info_texts_[8];
    std::vector<int> valid_idle_indices_;
    int refresh_interval_ = 15; // 默认刷新间隔
    int idle_counter_ = 0;
    bool idle_info_loaded_ = false; // 标记是否已加载待机信息
    ScreenType screen_type_ = SCREEN_128X64; // 屏幕类型

    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

    void SetupUI_128x64();
    void SetupUI_128x32();

public:
    OledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height, bool mirror_x, bool mirror_y);
    ~OledDisplay();

    virtual void SetupUI() override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetTheme(Theme* theme) override;
    virtual void UpdateStatusBar(bool update_all = false) override;
    // 显示待机信息
    void ShowIdleInfo();
    void HideIdleInfo();
    void LoadIdleInfoSettings(); // 加载设置
    void SetLabelScrollSpeed(lv_obj_t* label, int px_per_sec);
};

#endif // OLED_DISPLAY_H
