#include "zhengchen_eye_lcd_display.h"
#include "gif/lvgl_gif.h"
#include "lvgl_theme.h"
#include "assets/lang_config.h"
#include "application.h"

#include <stdio.h>
#include <vector>
#include <algorithm>
#include <font_awesome.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <string.h>

#define TAG "ZhengchenEyeLcdDisplay"

extern const lv_img_dsc_t angry;
extern const lv_img_dsc_t confused;
extern const lv_img_dsc_t cool;
extern const lv_img_dsc_t delicious;
extern const lv_img_dsc_t happy;
extern const lv_img_dsc_t love;
extern const lv_img_dsc_t sad;
extern const lv_img_dsc_t sleepy;
extern const lv_img_dsc_t thinking;


void ZhengchenEyeLcdDisplay::SetupUI() {
    ESP_LOGI(TAG, "Starting UI setup");

    // 先调用基类 SetupUI 初始化标准 UI 组件（status_label_, idle_labels_ 等）
    LcdDisplay::SetupUI();

    DisplayLockGuard lock(this);

    /* overlay - 全屏黑色覆盖层用于显示 GIF 表情 */
    overlay_container = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(overlay_container);
    lv_obj_set_size(overlay_container, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(overlay_container, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay_container, LV_OPA_COVER, 0);
    lv_obj_align(overlay_container, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(overlay_container, LV_OBJ_FLAG_HIDDEN); // 默认隐藏

    gif_label_ = lv_gif_create(overlay_container);
    lv_gif_set_src(gif_label_, &happy);
    lv_obj_set_size(gif_label_, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(gif_label_, LV_ALIGN_CENTER, 0, 0);
    ESP_LOGI(TAG, "UI setup completed. overlay_container: %p, gif_label_: %p", overlay_container, gif_label_);
}

void ZhengchenEyeLcdDisplay::SetEmotion(const char* emotion) {
    // idle状态且 neutral 情绪 - 隐藏表情覆盖层，显示正常界面
    auto& app = Application::GetInstance();
    if (strcmp(emotion, "neutral") == 0 && app.GetDeviceState() == kDeviceStateIdle) {
        ESP_LOGI(TAG, "Neutral emotion detected, hiding emotion overlay and showing normal UI");
        DisplayLockGuard lock(this);
        // 隐藏 overlay_container
        if (overlay_container != nullptr) {
            lv_obj_add_flag(overlay_container, LV_OBJ_FLAG_HIDDEN);
        }
        // 显示内容区域和其他元素
        if (content_ != nullptr) {
            lv_obj_clear_flag(content_, LV_OBJ_FLAG_HIDDEN);
        }
        if (status_bar_ != nullptr) {
            lv_obj_clear_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    struct Emotion {
        const lv_img_dsc_t* gif;
        const char* text;
    };

    static const std::vector<Emotion> emotions = {
        {&happy, "neutral"},
        {&happy, "happy"},
        {&happy, "laughing"},
        {&happy, "funny"},
        {&sad, "sad"},
        {&angry, "angry"},
        {&sad, "crying"},
        {&love, "loving"},
        {&confused, "embarrassed"},
        {&delicious, "surprised"},
        {&delicious, "shocked"},
        {&thinking, "thinking"},
        {&cool, "winking"},
        {&cool, "cool"},
        {&happy, "relaxed"},
        {&delicious, "delicious"},
        {&love, "kissy"},
        {&confused, "confident"},
        {&sleepy, "sleepy"},
        {&delicious, "silly"},
        {&confused, "confused"}
    };

    // 查找匹配的表情
    std::string_view emotion_view(emotion);
    auto it = std::find_if(emotions.begin(), emotions.end(),
        [&emotion_view](const Emotion& e) { return e.text == emotion_view; });

    DisplayLockGuard lock(this);

    if (gif_label_ == nullptr) {
        ESP_LOGI(TAG, "GIF label is null, cannot set emotion: %s", emotion);
        return;  // 锁在作用域结束时自动释放
    }

    if (it != emotions.end()) {
        lv_gif_set_src(gif_label_, it->gif);
        ESP_LOGI(TAG, "Setting emotion: %s with GIF", emotion);
    } else {
        ESP_LOGI(TAG, "Emotion '%s' not found, setting default happy GIF", emotion);
        lv_gif_set_src(gif_label_, &happy);
    }
    // 显示 overlay_container 并隐藏其他元素
    lv_obj_clear_flag(overlay_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(overlay_container);

    // 隐藏内容区域和其他元素
    if (content_ != nullptr) {
        lv_obj_add_flag(content_, LV_OBJ_FLAG_HIDDEN);
    }
    if (status_bar_ != nullptr) {
        lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
    }
    if (emoji_box_ != nullptr) {
        lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    }
    if (top_bar_ != nullptr) {
        lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
    if (preview_image_ != nullptr) {
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    }
}


void ZhengchenEyeLcdDisplay::UpdateStatusBar(bool update_all) {
    auto& app = Application::GetInstance();
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();

    // 设备空闲状态时显示待机信息
    if (app.GetDeviceState() == kDeviceStateIdle && !app.music_playing_) {
        if (update_all || idle_counter_++ % refresh_interval_ == 0) {
            ShowIdleInfo();
        }
    } else {
        // 在非空闲状态隐藏待机信息
        HideIdleInfo();
        current_idle_page_ = 0;
        idle_counter_ = 0;
    }

    static int display_mode = 0;  // 0-时间, 1-网络, 2-电池, 3-音量/静音
    static unsigned long last_switch_time = 0;
    static bool initialized = false;
    // 使用ESP-IDF的时间函数
    unsigned long current_time = esp_timer_get_time() / 1000; // 转换为毫秒
    // 定时切换显示内容
    if (current_time - last_switch_time > 5000 || !initialized) {
        DisplayLockGuard lock(this);

        // 安全检查：确保主题和状态栏已初始化
        if (current_theme_ == nullptr || status_label_ == nullptr) {
            ESP_LOGW(TAG, "Theme or status_label not initialized yet, skipping UpdateStatusBar");
            return;
        }

        // 获取图标字体（在锁内获取，确保线程安全）
        LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        auto text_font = lvgl_theme->text_font()->font();
        auto icon_font = lvgl_theme->icon_font()->font();

        switch (display_mode) {
            case 0: // 显示时间
            {
                lv_obj_set_style_text_font(status_label_, text_font, 0);
                time_t now = time(NULL);
                struct tm* tm = localtime(&now);
                if (tm->tm_year >= 2025 - 1900) {
                    char time_str[16];
                    strftime(time_str, sizeof(time_str), "%H:%M", tm);
                    lv_label_set_text(status_label_, time_str);
                } else {
                    lv_label_set_text(status_label_, "Time:N/A");
                }
                break;
            }
            case 1: // 显示网络状态
            {
                auto device_state = Application::GetInstance().GetDeviceState();
                static const std::vector<DeviceState> allowed_states = {
                    kDeviceStateIdle,
                    kDeviceStateStarting,
                    kDeviceStateWifiConfiguring,
                    kDeviceStateActivating,
                };
                
                if (std::find(allowed_states.begin(), allowed_states.end(), device_state) != allowed_states.end()) {
                    const char* icon = board.GetNetworkStateIcon();
                    lv_obj_set_style_text_font(status_label_, icon_font, 0);
                    lv_label_set_text(status_label_, icon);
                }
                break;
            }
            case 2: // 显示电池状态
            {
                int battery_level;
                bool charging, discharging;
                if (board.GetBatteryLevel(battery_level, charging, discharging)) {
                    if (charging) {
                        lv_obj_set_style_text_font(status_label_, icon_font, 0);
                        lv_label_set_text(status_label_, FONT_AWESOME_BATTERY_BOLT);
                    } else {
                        lv_obj_set_style_text_font(status_label_, text_font, 0);
                        char battery_str[16];
                        snprintf(battery_str, sizeof(battery_str), "Bat:%d", battery_level);
                        lv_label_set_text(status_label_, battery_str);
                    }
                    if (low_battery_popup_ != nullptr) {
                        if (battery_level < 10 && discharging) {
                            if (lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) { // 如果低电量提示框隐藏，则显示
                                lv_obj_remove_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                                app.PlaySound(Lang::Sounds::OGG_LOW_BATTERY);
                            }
                        } else {
                            // Hide the low battery popup when the battery is not empty
                            if (!lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) { // 如果低电量提示框显示，则隐藏
                                lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                            }
                        }
                    }
                } else {
                    lv_label_set_text(status_label_, "Bat:N/A");
                }
                break;
            }
            case 3: // 显示音量/静音状态
            {
                if (codec->output_volume() == 0) {
                    lv_obj_set_style_text_font(status_label_, icon_font, 0);
                    lv_label_set_text(status_label_, FONT_AWESOME_VOLUME_XMARK);
                } else {
                    lv_obj_set_style_text_font(status_label_, text_font, 0);
                    char volume_str[16];
                    snprintf(volume_str, sizeof(volume_str), "Vol:%d", codec->output_volume());
                    lv_label_set_text(status_label_, volume_str);
                }
                break;
            }
        }
        display_mode = (display_mode + 1) % 4;  // 循环切换模式
        last_switch_time = current_time;
        initialized = true;
    }
}