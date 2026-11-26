#include "oled_display.h"
#include "assets/lang_config.h"
#include "lvgl_font.h"
#include "lvgl_theme.h"
#include "settings.h"
#include "board.h"
#include "application.h"
#include "audio_codec.h"
#include "system_info.h"
#include "display_utils.h"

#include <algorithm>
#include <string>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <material_symbols.h>
#include <noto_emoji.h>

#define TAG "OledDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_material_symbols_30_1);
LV_FONT_DECLARE(font_noto_emoji_30_1);

OledDisplay::OledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                         int width, int height, bool mirror_x, bool mirror_y)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_material_symbols_30_1);
    auto emoji_font = std::make_shared<LvglBuiltInFont>(&font_noto_emoji_30_1);

    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);
    dark_theme->set_emoji_font(emoji_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("dark", dark_theme);
    current_theme_ = dark_theme;

    ESP_LOGI(TAG, "Initialize LVGL");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.task_stack = 6144;
    port_cfg.timer_period_ms = 50;  // 稳定的 LVGL 刷新节拍，避免刷新过于集中触发看门狗
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding OLED display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * height_),
        .double_buffer = false,
        // trans_size 限制单笔 SPI 传输字节数，避免 flush 一次性发送整块缓冲造成长时间
        // 阻塞，导致高负载场景下 wait_for_flushing 卡死、看门狗触发。拆成多笔小传输后每
        // 笔之间释放 CPU，idle 任务可正常喂 task_wdt。OLED 为单色 1bpp，单行字节数为 width_/8。
        .trans_size = static_cast<uint32_t>(width_ / 8 * 4),
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = true,
        .rotation =
            {
                .swap_xy = false,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = 1,
                .buff_spiram = 0,
                .sw_rotate = 0,
                .full_refresh = 0,
                .direct_mode = 0,
            },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    // Note: SetupUI() should be called by Application::Initialize(), not in constructor
    // to ensure lvgl objects are created after the display is fully initialized.
}

void OledDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    Display::SetupUI();  // Mark SetupUI as called
    if (height_ == 64) {
        screen_type_ = SCREEN_128X64;
        SetupUI_128x64();
    } else {
        screen_type_ = SCREEN_128X32;
        items_per_page_ = 1;
        SetupUI_128x32();
    }
}

OledDisplay::~OledDisplay() {
    if (content_ != nullptr) {
        lv_obj_del(content_);
    }

    bool is_128x64_layout = (top_bar_ != nullptr);
    if (status_bar_ != nullptr && is_128x64_layout) {
        status_label_ = nullptr;
        notification_label_ = nullptr;
        lv_obj_del(status_bar_);
    }
    if (top_bar_ != nullptr) {
        network_label_ = nullptr;
        mute_label_ = nullptr;
        battery_label_ = nullptr;
        lv_obj_del(top_bar_);
    }
    if (side_bar_ != nullptr) {
        if (!is_128x64_layout) {
            status_label_ = nullptr;
            notification_label_ = nullptr;
            network_label_ = nullptr;
            mute_label_ = nullptr;
            battery_label_ = nullptr;
        }
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
    lvgl_port_deinit();
}

bool OledDisplay::Lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }

void OledDisplay::Unlock() { lvgl_port_unlock(); }

void OledDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        return;
    }

    // Replace all newlines with spaces
    std::string content_str = content;
    std::replace(content_str.begin(), content_str.end(), '\n', ' ');

    lv_anim_delete(chat_message_label_, nullptr);
    if (content_right_ == nullptr) {
        lv_label_set_text(chat_message_label_, content_str.c_str());
    } else {
        if (content == nullptr || content[0] == '\0') {
            lv_obj_add_flag(content_right_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(chat_message_label_, content_str.c_str());
            lv_obj_remove_flag(content_right_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void OledDisplay::RemoveChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr || content == nullptr) {
        return;
    }
    // OLED has one text slot and does not distinguish message roles.
    std::string content_str = content;
    std::replace(content_str.begin(), content_str.end(), '\n', ' ');
    if (content_str != lv_label_get_text(chat_message_label_)) {
        return;
    }
    lv_anim_delete(chat_message_label_, nullptr);
    lv_label_set_text(chat_message_label_, "");
    if (content_right_ != nullptr) {
        lv_obj_add_flag(content_right_, LV_OBJ_FLAG_HIDDEN);
    }
}

void OledDisplay::SetupUI_128x64() {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);

    /* Layer 1: Top bar - for status icons */
    top_bar_ = lv_obj_create(container_);
    lv_obj_set_size(top_bar_, LV_HOR_RES, 16);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);

    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);

    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, 16);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);        // Overlap with top_bar_

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    /* Content */
    content_ = lv_obj_create(container_);
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_style_pad_all(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_flex_main_place(content_, LV_FLEX_ALIGN_CENTER, 0);

    content_left_ = lv_obj_create(content_);
    lv_obj_set_size(content_left_, 32, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(content_left_, 0, 0);
    lv_obj_set_style_border_width(content_left_, 0, 0);

    emotion_label_ = lv_label_create(content_left_);
    lv_obj_set_style_text_font(emotion_label_, large_icon_font, 0);
    lv_label_set_text(emotion_label_, MATERIAL_SYMBOLS_ROBOT_2);
    lv_obj_center(emotion_label_);
    lv_obj_set_style_pad_top(emotion_label_, 8, 0);

    content_right_ = lv_obj_create(content_);
    lv_obj_set_size(content_right_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(content_right_, 0, 0);
    lv_obj_set_style_border_width(content_right_, 0, 0);
    lv_obj_set_flex_grow(content_right_, 1);
    lv_obj_add_flag(content_right_, LV_OBJ_FLAG_HIDDEN);

    chat_message_label_ = lv_label_create(content_right_);
    lv_label_set_text(chat_message_label_, "");
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(chat_message_label_, width_ - 32);
    lv_obj_set_style_pad_top(chat_message_label_, 14, 0);

    // Start scrolling subtitle after a delay
    static lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_delay(&a, 1000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_obj_set_style_anim(chat_message_label_, &a, LV_PART_MAIN);
    lv_obj_set_style_anim_duration(chat_message_label_, lv_anim_speed_clamped(60, 300, 60000),
                                   LV_PART_MAIN);

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(low_battery_popup_, lv_color_black(), 0);
    lv_obj_set_style_radius(low_battery_popup_, 10, 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    // 创建用于待机状态显示信息的标签，默认隐藏
    for (int i = 0; i < 8; i++) {
        idle_labels_[i] = lv_label_create(content_);
        lv_label_set_text(idle_labels_[i], "");
        lv_label_set_long_mode(idle_labels_[i], LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(idle_labels_[i], width_);
        lv_obj_set_style_text_align(idle_labels_[i], LV_TEXT_ALIGN_CENTER, 0); // 居中显示
        // 减少标签的垂直内边距
        lv_obj_set_style_pad_top(idle_labels_[i], 0, 0);
        lv_obj_set_style_pad_bottom(idle_labels_[i], 0, 0);
        lv_obj_add_flag(idle_labels_[i], LV_OBJ_FLAG_HIDDEN);
        // show会重新计算标签动画，不再为每个标签设置滚动动画
    }
}

void OledDisplay::SetupUI_128x32() {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_column(container_, 0, 0);

    /* Emotion label on the left side */
    content_ = lv_obj_create(container_);
    lv_obj_set_size(content_, 32, 32);
    lv_obj_set_style_pad_all(content_, 0, 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_radius(content_, 0, 0);

    emotion_label_ = lv_label_create(content_);
    lv_obj_set_style_text_font(emotion_label_, large_icon_font, 0);
    lv_label_set_text(emotion_label_, MATERIAL_SYMBOLS_ROBOT_2);
    lv_obj_center(emotion_label_);

    /* Right side */
    side_bar_ = lv_obj_create(container_);
    lv_obj_set_size(side_bar_, width_ - 32, 32);
    lv_obj_set_flex_flow(side_bar_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(side_bar_, 0, 0);
    lv_obj_set_style_border_width(side_bar_, 0, 0);
    lv_obj_set_style_radius(side_bar_, 0, 0);
    lv_obj_set_style_pad_row(side_bar_, 0, 0);

    /* Status bar */
    status_bar_ = lv_obj_create(side_bar_);
    lv_obj_set_size(status_bar_, width_ - 32, 16);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_flex_flow(status_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_column(status_bar_, 0, 0);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(status_label_, 1);
    lv_obj_set_style_pad_left(status_label_, 2, 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(notification_label_, 1);
    lv_obj_set_style_pad_left(notification_label_, 2, 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    mute_label_ = lv_label_create(status_bar_);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);

    network_label_ = lv_label_create(status_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);

    battery_label_ = lv_label_create(status_bar_);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);

    chat_message_label_ = lv_label_create(side_bar_);
    lv_obj_set_size(chat_message_label_, width_ - 32, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_left(chat_message_label_, 2, 0);
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(chat_message_label_, "");

    // Start scrolling subtitle after a delay
    static lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_delay(&a, 1000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_obj_set_style_anim(chat_message_label_, &a, LV_PART_MAIN);
    lv_obj_set_style_anim_duration(chat_message_label_, lv_anim_speed_clamped(60, 300, 60000), LV_PART_MAIN);
    // 为128x32屏幕创建idle信息标签
    for (int i = 0; i < 8; i++) {
        idle_labels_[i] = lv_label_create(side_bar_);
        lv_label_set_text(idle_labels_[i], "");
        lv_label_set_long_mode(idle_labels_[i], LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(idle_labels_[i], width_ - 32);
        lv_obj_set_style_text_align(idle_labels_[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(idle_labels_[i], 0, 0);
        lv_obj_set_style_pad_bottom(idle_labels_[i], 0, 0);
        lv_obj_add_flag(idle_labels_[i], LV_OBJ_FLAG_HIDDEN);
    }
}

bool OledDisplay::SetEmotion(const char* emotion) {
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    const char* utf8 = noto_emoji_get_utf8(emotion);
    const lv_font_t* emotion_font = lvgl_theme->emoji_font()->font();
    if (utf8 == nullptr) {
        utf8 = material_symbols_get_utf8(emotion);
        emotion_font = lvgl_theme->large_icon_font()->font();
    }
    DisplayLockGuard lock(this);
    if (emotion_label_ == nullptr) {
        return false;
    }
    if (utf8 != nullptr) {
        lv_obj_set_style_text_font(emotion_label_, emotion_font, 0);
        lv_label_set_text(emotion_label_, utf8);
        return true;
    }
    // Neither font has a glyph for this name, so what is shown is the neutral placeholder and
    // not the emotion that was asked for.
    lv_obj_set_style_text_font(emotion_label_, lvgl_theme->emoji_font()->font(), 0);
    lv_label_set_text(emotion_label_, NOTO_EMOJI_NEUTRAL);
    return false;
}

void OledDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    auto text_font = lvgl_theme->text_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
}

void OledDisplay::SetPowerSaveMode(bool on) {
    if (panel_) {
        // NVS key names are limited to 15 characters (NVS_KEY_NAME_MAX_SIZE
        // includes the null terminator), so keep the key on a constant that
        // refuses to compile if it ever outgrows the limit.
        static constexpr char kPowerSaveDisplayOffKey[] = "ps_display_off";
        static_assert(sizeof(kPowerSaveDisplayOffKey) <= NVS_KEY_NAME_MAX_SIZE,
                      "NVS key name must fit in 15 characters");
        Settings settings("wifi", false);
        if (settings.GetBool(kPowerSaveDisplayOffKey, false)) {
            esp_lcd_panel_disp_on_off(panel_, !on);
        }
    }
    LvglDisplay::SetPowerSaveMode(on);
}

void OledDisplay::UpdateStatusBar(bool update_all) {
    auto& app = Application::GetInstance();
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();

    // Update mute icon
    {
        DisplayLockGuard lock(this);
        if (mute_label_ == nullptr) {
            return;
        }

        // 如果静音状态改变，则更新图标
        if (codec->output_volume() == 0 && !muted_) {
            muted_ = true;
            lv_label_set_text(mute_label_, MATERIAL_SYMBOLS_VOLUME_MUTE);
        } else if (codec->output_volume() > 0 && muted_) {
            muted_ = false;
            lv_label_set_text(mute_label_, "");
        }
    }

    // 设备空闲状态时更新时间显示：每 10 秒更新一次时间显示，设置到状态标签
    if (app.GetDeviceState() == kDeviceStateIdle && !app.music_playing_) {
        if (last_status_update_time_ + std::chrono::seconds(10) < std::chrono::system_clock::now()) {
            // Set status to clock "HH:MM"
            time_t now = time(NULL);
            struct tm* tm = localtime(&now);
            // Check if the we have already set the time
            if (tm->tm_year >= 2025 - 1900) {
                char time_str[16];
                strftime(time_str, sizeof(time_str), "%H:%M  ", tm);
                SetStatus(time_str);
            } else {
                ESP_LOGW(TAG, "System time is not set, tm_year: %d", tm->tm_year);
            }
        }
        if (update_all || idle_counter_++ % refresh_interval_ == 0) {
            ShowIdleInfo();
        }
    } else {
        // 在非空闲状态隐藏待机信息
        HideIdleInfo();
        current_idle_page_ = 0;
        idle_counter_ = 0;
    }

    // 更新电池图标
    int battery_level;
    bool charging, discharging;
    const char* icon = nullptr;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        if (charging) {
            icon = MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_BOLT;
        } else {
            const char* levels[] = {
                MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_1, // 0-19%
                MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_2,    // 20-39%
                MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_3,    // 40-59%
                MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_4,    // 60-79%
                MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_6, // 80-99%
                MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_6, // 100%
            };
            icon = levels[battery_level / 20];
        }
        DisplayLockGuard lock(this);
        if (battery_label_ != nullptr && battery_icon_ != icon) {
            battery_icon_ = icon;
            lv_label_set_text(battery_label_, battery_icon_);
        }

        if (low_battery_popup_ != nullptr) {
            if (strcmp(icon, MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_1) == 0 && discharging) {
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
    }

    // 每 10 秒更新一次网络图标
    static int seconds_counter = 0;
    if (update_all || seconds_counter++ % 10 == 0) {
        // 升级固件时，不读取 4G 网络状态，避免占用 UART 资源
        auto device_state = Application::GetInstance().GetDeviceState();
        static const std::vector<DeviceState> allowed_states = {
            kDeviceStateIdle,
            kDeviceStateStarting,
            kDeviceStateWifiConfiguring,
            kDeviceStateListening,
            kDeviceStateActivating,
        };
        if (std::find(allowed_states.begin(), allowed_states.end(), device_state) != allowed_states.end()) {
            const char* icon = board.GetNetworkStateIcon();
            if (network_label_ != nullptr && icon != nullptr && network_icon_ != icon) {
                DisplayLockGuard lock(this);
                network_icon_ = icon;
                lv_label_set_text(network_label_, network_icon_);
            }
        }
    }

    esp_pm_lock_release(pm_lock_);
}

void OledDisplay::ShowIdleInfo() {   
    DisplayLockGuard lock(this);

    // 第一次显示待机信息时异步加载设置，避免阻塞主任务（服务器响应慢/超时会拖垮唤醒与状态机）
    if (!idle_info_loaded_ && !idle_info_loading_) {
        LoadIdleInfoSettingsAsync();
    }
    // 先隐藏所有标签
    for (int i = 0; i < 8; i++) {
        if (idle_labels_[i] != nullptr) {
            lv_obj_add_flag(idle_labels_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (screen_type_ == SCREEN_128X64) {
        // 隐藏聊天和表情区域
        if (content_left_ != nullptr) {
            lv_obj_add_flag(content_left_, LV_OBJ_FLAG_HIDDEN);
        }
        if (content_right_ != nullptr) {
            lv_obj_add_flag(content_right_, LV_OBJ_FLAG_HIDDEN);
        }

        if (valid_idle_indices_.empty()) {
            // 没有标签信息时，显示左侧表情符号区域并居中
            if (content_left_ != nullptr) {
                lv_obj_remove_flag(content_left_, LV_OBJ_FLAG_HIDDEN);
            }
            // 恢复content_的布局设置使左侧区域居中
            lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_ROW);
            lv_obj_set_style_flex_main_place(content_, LV_FLEX_ALIGN_CENTER, 0);
            // ESP_LOGI(TAG, "No idle info to display, showing emotion icon only");
            return;
        }
        // 如果有可见标签，调整content_的布局以适应垂直排列
        total_idle_pages_ = (valid_idle_indices_.size() + items_per_page_ - 1) / items_per_page_;
        // 显示当前页的标签
        int start_idx = current_idle_page_ * items_per_page_;
        int end_idx = std::min(start_idx + items_per_page_, (int)valid_idle_indices_.size());
        for (int i = start_idx; i < end_idx; i++) {
            int label_idx = valid_idle_indices_[i];
            if (idle_labels_[label_idx] != nullptr) {
                lv_label_set_text(idle_labels_[label_idx], idle_info_texts_[label_idx].c_str());
                SetLabelScrollSpeed(idle_labels_[i], 30);
                lv_obj_remove_flag(idle_labels_[label_idx], LV_OBJ_FLAG_HIDDEN);
                ESP_LOGI(TAG, "Displaying idle info label %d (page %d): %s", 
                        label_idx, current_idle_page_, idle_info_texts_[label_idx].c_str());
            }
        }
        
        // 切换到下一页
        current_idle_page_ = (current_idle_page_ + 1) % total_idle_pages_;
        // 更新content_的布局
        lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_flex_main_place(content_, LV_FLEX_ALIGN_START, 0);
    } else if (screen_type_ == SCREEN_128X32) {
        if (valid_idle_indices_.empty()) {
            for (int i = 0; i < 8; i++) {
                if (idle_labels_[i] != nullptr) {
                    lv_obj_add_flag(idle_labels_[i], LV_OBJ_FLAG_HIDDEN);
                }
            }
            return;
        }
        // 隐藏聊天消息标签
        if (chat_message_label_ != nullptr) {
            lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
        }
        // 计算总页数
        total_idle_pages_ = (valid_idle_indices_.size() + items_per_page_ - 1) / items_per_page_;
        // 显示当前页的标签
        int start_idx = current_idle_page_ * items_per_page_;
        int end_idx = std::min(start_idx + items_per_page_, (int)valid_idle_indices_.size());
        for (int i = start_idx; i < end_idx; i++) {
            int label_idx = valid_idle_indices_[i];
            if (idle_labels_[label_idx] != nullptr) {
                lv_label_set_text(idle_labels_[label_idx], idle_info_texts_[label_idx].c_str());
                SetLabelScrollSpeed(idle_labels_[label_idx], 30);
                lv_obj_remove_flag(idle_labels_[label_idx], LV_OBJ_FLAG_HIDDEN);
                ESP_LOGI(TAG, "Displaying idle info label %d (page %d): %s", 
                        label_idx, current_idle_page_, idle_info_texts_[label_idx].c_str());
            }
        }
        // 切换到下一页
        current_idle_page_ = (current_idle_page_ + 1) % total_idle_pages_;
    }
    ESP_LOGI(TAG, "Switched to idle info display with %d labels", valid_idle_indices_.size());
}

void OledDisplay::HideIdleInfo() {
    DisplayLockGuard lock(this);
    // 隐藏待机信息标签
    for (int i = 0; i < 8; i++) {
        if (idle_labels_[i] != nullptr) {
            lv_obj_add_flag(idle_labels_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (screen_type_ == SCREEN_128X64) {
        // 恢复聊天布局
        if (content_left_ != nullptr) {
            lv_obj_remove_flag(content_left_, LV_OBJ_FLAG_HIDDEN);
        }
        
        // 恢复content_的布局设置
        lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_flex_main_place(content_, LV_FLEX_ALIGN_CENTER, 0);
    }  else if (screen_type_ == SCREEN_128X32) {
        if (chat_message_label_ != nullptr) {
            lv_obj_remove_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void OledDisplay::LoadIdleInfoSettings() {
    display_utils::LoadIdleInfoSettings(idle_info_texts_, valid_idle_indices_, refresh_interval_);
}

void OledDisplay::LoadIdleInfoSettingsAsync() {
    idle_info_loading_ = true;
    // 在独立任务中执行阻塞式 HTTPS 请求，避免拖垮 Application 主任务（唤醒/状态机/MQTT）
    xTaskCreate(
        [](void* arg) {
            auto* display = static_cast<OledDisplay*>(arg);
            // 阻塞加载（可能耗时/超时），运行在非主任务上下文中
            display->LoadIdleInfoSettings();
            // 回到主任务上下文完成状态切换与刷新，保证对 idle_info_texts_ 的访问线程安全
            Application::GetInstance().Schedule([display]() {
                display->idle_info_loading_ = false;
                display->idle_info_loaded_ = true;
                display->idle_counter_ = 0;
                display->ShowIdleInfo();
            });
            vTaskDelete(nullptr);
        },
        "idle_info_loader", 8192, this, 1, nullptr);
}

void OledDisplay::SetLabelScrollSpeed(lv_obj_t* label, int px_per_sec) {
    display_utils::SetLabelScrollSpeed(label, px_per_sec, refresh_interval_);
}
