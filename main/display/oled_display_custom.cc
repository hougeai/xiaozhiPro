#include "oled_display_custom.h"
#include "assets/lang_config.h"
#include "lvgl_theme.h"
#include "lvgl_font.h"
#include "board.h"
#include "application.h"
#include "audio_codec.h"
#include "settings.h"
#include "system_info.h"

#include <string>
#include <algorithm>

#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include <material_symbols.h>
#include <noto_emoji.h>
#include <lvgl.h>

#define TAG "CustomOledDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_material_symbols_30_1);

CustomOledDisplay::CustomOledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
    int width, int height, bool mirror_x, bool mirror_y)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_material_symbols_30_1);
    
    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);

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
        .rotation = {
            .swap_xy = false,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
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

    if (height_ == 64) {
        screen_type_ = SCREEN_128X64;
        SetupUI_128x64();
    } else {
        screen_type_ = SCREEN_128X32;
        items_per_page_ = 1;
        SetupUI_128x32();
    }
}

CustomOledDisplay::~CustomOledDisplay() {
    // 析构函数可以保持为空，基类会处理对象删除
}

// Lock 函数尝试获取 LVGL 的互斥锁：用于多线程环境中保护对 LVGL 的访问
bool CustomOledDisplay::Lock(int timeout_ms) {
    return lvgl_port_lock(timeout_ms);
}

// Unlock 函数释放 LVGL 的互斥锁
void CustomOledDisplay::Unlock() {
    lvgl_port_unlock();
}

void CustomOledDisplay::SetChatMessage(const char* role, const char* content) {
    // 使用 DisplayLockGuard 自动加锁/解锁
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        return;
    }

    // Replace all newlines with spaces
    std::string content_str = content;
    std::replace(content_str.begin(), content_str.end(), '\n', ' ');

    if (content_right_ == nullptr) {
        lv_label_set_text(chat_message_label_, content_str.c_str());
    } else {
        if (content == nullptr || content[0] == '\0') {
            // 隐藏右侧部分
            lv_obj_add_flag(content_right_, LV_OBJ_FLAG_HIDDEN);
        } else {
            // 设置聊天消息文本
            lv_label_set_text(chat_message_label_, content_str.c_str());
            // 去除隐藏标签 显示聊天内容
            lv_obj_remove_flag(content_right_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void CustomOledDisplay::SetupUI_128x64() {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();
    // 获取当前活动屏幕并设置文本字体和颜色
    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);

    // 创建主容器 container_，垂直排列 LV_FLEX_FLOW_COLUMN
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);

    // 在主容器 container_ 创建状态栏 status_bar_，高度为 16 像素
    status_bar_ = lv_obj_create(container_);
    lv_obj_set_size(status_bar_, LV_HOR_RES, 16);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_radius(status_bar_, 0, 0);

    // 在主容器 container_ 创建内容区域 content_，占据剩余空间，水平排列LV_FLEX_FLOW_ROW
    content_ = lv_obj_create(container_);
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_style_pad_all(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    // content中子元素 left right 水平排列，在主轴上居中显示，当right隐藏时，left就会在中间
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_flex_main_place(content_, LV_FLEX_ALIGN_CENTER, 0);

    // 创建左侧固定宽度的容器：宽度为 32 像素
    content_left_ = lv_obj_create(content_);
    lv_obj_set_size(content_left_, 32, LV_SIZE_CONTENT);  // 固定宽度32像素
    lv_obj_set_style_pad_all(content_left_, 0, 0);
    lv_obj_set_style_border_width(content_left_, 0, 0);
    // 表情符号标签，默认为 AI 芯片图标并居中显示
    emotion_label_ = lv_label_create(content_left_);
    lv_obj_set_style_text_font(emotion_label_, large_icon_font, 0);
    lv_label_set_text(emotion_label_, MATERIAL_SYMBOLS_ROBOT_2);
    lv_obj_center(emotion_label_);
    lv_obj_set_style_pad_top(emotion_label_, 8, 0);

    // 创建右侧可扩展的容器：初始状态为隐藏
    content_right_ = lv_obj_create(content_);
    lv_obj_set_size(content_right_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(content_right_, 0, 0);
    lv_obj_set_style_border_width(content_right_, 0, 0);
    lv_obj_set_flex_grow(content_right_, 1);
    // LV_OBJ_FLAG_HIDDEN就是隐藏这个区域
    lv_obj_add_flag(content_right_, LV_OBJ_FLAG_HIDDEN);
    // 右侧容器中创建聊天消息标签 chat_message_label_
    chat_message_label_ = lv_label_create(content_right_);
    lv_label_set_text(chat_message_label_, "");
    // 为循环滚动模式LV_LABEL_LONG_SCROLL_CIRCULAR
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(chat_message_label_, width_ - 32);
    lv_obj_set_style_pad_top(chat_message_label_, 14, 0);

    // 给聊天消息标签设置滚动动画，延迟 1 秒后开始无限循环滚动
    static lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_delay(&a, 1000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_obj_set_style_anim(chat_message_label_, &a, LV_PART_MAIN);
    lv_obj_set_style_anim_duration(chat_message_label_, lv_anim_speed_clamped(60, 300, 60000), LV_PART_MAIN);

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

    // 状态栏为水平排列（LV_FLEX_FLOW_ROW）
    lv_obj_set_flex_flow(status_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_column(status_bar_, 0, 0); // 各列（标签）之间没有间距
    // 网络标签
    network_label_ = lv_label_create(status_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    // 通知标签（初始隐藏）
    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(notification_label_, 1);  // 占据剩余空间，会平均分配父容器中剩余的空间，这就导致了标签之间出现了间隔
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    // 状态标签（显示初始化文本）
    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(status_label_, 1);  // 占据剩余空间
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    // 静音标签
    mute_label_ = lv_label_create(status_bar_);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    // 电池图标标签
    battery_label_ = lv_label_create(status_bar_);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    // 电池文字标签
    battery_text_label_ = lv_label_create(status_bar_);
    lv_label_set_text(battery_text_label_, "");
    lv_obj_set_style_text_font(battery_text_label_, text_font, 0);
    // 低电量提示框 默认隐藏
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
}

void CustomOledDisplay::SetupUI_128x32() {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);

    /* Container 整个屏幕容器，横向流式布局 */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_column(container_, 0, 0);
    // 减少容器内元素之间的间距
    lv_obj_set_style_pad_row(container_, 0, 0);

    /* Emotion label on the left side 左侧 32x32 区域 */
    content_ = lv_obj_create(container_);
    lv_obj_set_size(content_, 32, 32);
    lv_obj_set_style_pad_all(content_, 0, 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_radius(content_, 0, 0);

    emotion_label_ = lv_label_create(content_);
    lv_obj_set_style_text_font(emotion_label_, large_icon_font, 0);
    lv_label_set_text(emotion_label_, MATERIAL_SYMBOLS_ROBOT_2);
    lv_obj_center(emotion_label_);

    /* Right side：右侧占据剩余宽度的区域，采用纵向流式布局，包含状态栏和聊天消息 */
    side_bar_ = lv_obj_create(container_);
    lv_obj_set_size(side_bar_, width_ - 32, 32);
    lv_obj_set_flex_flow(side_bar_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(side_bar_, 0, 0);
    lv_obj_set_style_border_width(side_bar_, 0, 0);
    lv_obj_set_style_radius(side_bar_, 0, 0);
    lv_obj_set_style_pad_row(side_bar_, 0, 0);

    /* Status bar：右侧顶部 96x16 包含多个标签 */
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
    
    // 创建电池文字标签
    battery_text_label_ = lv_label_create(status_bar_);
    lv_label_set_text(battery_text_label_, "");
    lv_obj_set_style_text_font(battery_text_label_, text_font, 0);

    // 右侧下方 96x16 聊天消息
    chat_message_label_ = lv_label_create(side_bar_);
    lv_obj_set_size(chat_message_label_, width_ - 32, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_left(chat_message_label_, 2, 0);
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(chat_message_label_, "");

    // 延迟一定的时间后开始滚动字幕
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

// 根据传入的 emotion 字符串获取对应的 FontAwesome 图标并设置到 emotion_label_
bool CustomOledDisplay::SetEmotion(const char* emotion) {
    const char* utf8 = noto_emoji_get_utf8(emotion);
    DisplayLockGuard lock(this);
    if (emotion_label_ == nullptr) {
        return false;
    }
    if (utf8 != nullptr) {
        lv_label_set_text(emotion_label_, utf8);
        return true;
    }
    utf8 = material_symbols_get_utf8(emotion);
    if (utf8 != nullptr) {
        lv_label_set_text(emotion_label_, utf8);
        return true;
    }
    // 两个字体都没有该表情的字形，显示中性占位（robot），而非用户要求的表情
    lv_label_set_text(emotion_label_, MATERIAL_SYMBOLS_ROBOT_2);
    return false;
}

// 根据传入的主题设置屏幕文本字体
void CustomOledDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    auto text_font = lvgl_theme->text_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
}

void CustomOledDisplay::UpdateStatusBar(bool update_all) {
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

    // 更新电池信息：获取电池电量、充电和放电状态，更新电池图标和文字，电量低则弹窗
    esp_pm_lock_acquire(pm_lock_);
    int battery_level;
    bool charging, discharging;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        // ESP_LOGI(TAG, "Battery level: %d%%, charging: %s, discharging: %s", battery_level, charging ? "true" : "false", discharging ? "true" : "false");
        const char* battery_text = GetBatteryText(battery_level, charging);
        
        // 获取电池图标
        const char* battery_icon = nullptr;
        if (charging) {
            battery_icon = MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_BOLT;
        } else {
            const char* levels[] = {
                MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_1, // 0-19%
                MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_2,    // 20-39%
                MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_3,    // 40-59%
                MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_4,    // 60-79%
                MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_6, // 80-99%
                MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_6, // 100%
            };
            battery_icon = levels[battery_level / 20];
        }
        
        DisplayLockGuard lock(this);
        // 更新电池图标
        if (battery_label_ != nullptr && battery_icon_ != battery_icon) {
            battery_icon_ = battery_icon;
            lv_label_set_text(battery_label_, battery_icon_);
        }
        // 更新电池文字
        if (battery_text_label_ != nullptr) {
            lv_label_set_text(battery_text_label_, battery_text);
        }

        if (low_battery_popup_ != nullptr) {
            if (battery_level <= 10 && discharging) {
                if (lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) {
                    lv_obj_remove_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                    app.PlaySound(Lang::Sounds::OGG_LOW_BATTERY);
                }
            } else {
                if (!lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) {
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

const char* CustomOledDisplay::GetBatteryText(int battery_level, bool charging) {
    static char battery_text[10];
    snprintf(battery_text, sizeof(battery_text), "%d", battery_level);
    return battery_text;
}

void CustomOledDisplay::ShowIdleInfo() {   
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
            ESP_LOGI(TAG, "No idle info to display, showing emotion icon only");
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

void CustomOledDisplay::HideIdleInfo() {
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

void CustomOledDisplay::LoadIdleInfoSettingsAsync() {
    idle_info_loading_ = true;
    // 在独立任务中执行阻塞式 HTTPS 请求，避免拖垮 Application 主任务（唤醒/状态机/MQTT）
    xTaskCreate(
        [](void* arg) {
            auto* display = static_cast<CustomOledDisplay*>(arg);
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

void CustomOledDisplay::LoadIdleInfoSettings() {
    ESP_LOGI(TAG, "Loading idle info settings via network request");
    // 获取网络实例
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(2);
    if (!http) {
        ESP_LOGE(TAG, "Failed to create HTTP client for display info");
        return;
    }
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("Device-ID", SystemInfo::GetMacAddress().c_str());
    std::string url = std::string(CONFIG_SERVER_URL) + "/device_display";
    ESP_LOGI(TAG, "Fetching display info from: %s", url.c_str());
    
    if (http->Open("GET", url)) {
        int status_code = http->GetStatusCode().value_or(0);
        if (status_code == 200) {
            std::string response = http->ReadAll();
            ESP_LOGI(TAG, "Successfully fetched display info");
            // 解析JSON响应
            cJSON* root = cJSON_Parse(response.c_str());
            if (root) {
                cJSON* data_obj = cJSON_GetObjectItem(root, "data");
                if (data_obj) {
                    // 解析并保存天气信息
                    cJSON* weather_obj = cJSON_GetObjectItem(data_obj, "weather");
                    if (weather_obj) {
                        cJSON* weather_str = cJSON_GetObjectItem(weather_obj, "weather");
                        if (weather_str) {
                            idle_info_texts_[0] = std::string(weather_str->valuestring);
                            ESP_LOGI(TAG, "Loaded weather: %s", weather_str->valuestring);
                        }
                        cJSON* aqi_str = cJSON_GetObjectItem(weather_obj, "aqi");
                        if (aqi_str) {
                            idle_info_texts_[1] = std::string(aqi_str->valuestring);
                            ESP_LOGI(TAG, "Loaded aqi: %s", aqi_str->valuestring);
                        }
                    }
                    // 解析并保存时间信息
                    cJSON* timeday_obj = cJSON_GetObjectItem(data_obj, "timeday");
                    if (timeday_obj) {
                        cJSON* day_y_item = cJSON_GetObjectItem(timeday_obj, "day_y");
                        if (day_y_item) {
                            idle_info_texts_[2] = std::string(day_y_item->valuestring);
                            ESP_LOGI(TAG, "Loaded day_y: %s", day_y_item->valuestring);
                        }
                        
                        cJSON* day_n_item = cJSON_GetObjectItem(timeday_obj, "day_n");
                        if (day_n_item) {
                            idle_info_texts_[3] = std::string(day_n_item->valuestring);
                            ESP_LOGI(TAG, "Loaded day_n: %s", day_n_item->valuestring);
                        }
                        
                        cJSON* wuhou_item = cJSON_GetObjectItem(timeday_obj, "wuhou");
                        if (wuhou_item) {
                            idle_info_texts_[4] = std::string(wuhou_item->valuestring);
                            ESP_LOGI(TAG, "Loaded wuhou: %s", wuhou_item->valuestring);
                        }
                        
                        cJSON* xiuluck_item = cJSON_GetObjectItem(timeday_obj, "xiuluck");
                        if (xiuluck_item) {
                            idle_info_texts_[5] = std::string(xiuluck_item->valuestring);
                            ESP_LOGI(TAG, "Loaded xiuluck: %s", xiuluck_item->valuestring);
                        }
                        
                        cJSON* yiji_item = cJSON_GetObjectItem(timeday_obj, "yiji");
                        if (yiji_item) {
                            idle_info_texts_[6] = std::string(yiji_item->valuestring);
                        }
                        
                        cJSON* today_history_item = cJSON_GetObjectItem(timeday_obj, "today_history");
                        if (today_history_item) {
                            idle_info_texts_[7] = std::string(today_history_item->valuestring);
                            ESP_LOGI(TAG, "Loaded today_history: %s", today_history_item->valuestring);
                        }
                    }
                    
                    // 解析并保存设置信息
                    cJSON* settings_obj = cJSON_GetObjectItem(data_obj, "settings");
                    if (settings_obj) {
                        cJSON* refresh_interval_item = cJSON_GetObjectItem(settings_obj, "refresh_interval");
                        if (refresh_interval_item) {
                            refresh_interval_ = refresh_interval_item->valueint;
                            ESP_LOGI(TAG, "Loaded refresh interval: %d", refresh_interval_);
                        }
                    }
                }
                cJSON_Delete(root);
            }
        } else {
            ESP_LOGW(TAG, "Server returned status code: %d", status_code);
        }
    } else {
        ESP_LOGE(TAG, "Failed to connect to display info server");
    }
    
    http->Close();
    
    // 更新有效索引列表
    valid_idle_indices_.clear();
    for (int i = 0; i < 8; i++) {
        if (!idle_info_texts_[i].empty()) {
            valid_idle_indices_.push_back(i);
        }
    }
    ESP_LOGI(TAG, "Loaded idle info settings with %d valid entries, refresh interval: %d", 
             valid_idle_indices_.size(), refresh_interval_);
}

void CustomOledDisplay::SetLabelScrollSpeed(lv_obj_t* label, int px_per_sec) {
    if (label == nullptr) return;

    const char* txt = lv_label_get_text(label);

    // 计算文本宽度 - 使用 LVGL 的公共 API
    lv_point_t size_res;
    lv_text_get_size(&size_res, txt, lv_obj_get_style_text_font(label, LV_PART_MAIN),
                     lv_obj_get_style_text_letter_space(label, LV_PART_MAIN),
                     lv_obj_get_style_text_line_space(label, LV_PART_MAIN),
                     lv_obj_get_width(label), LV_TEXT_FLAG_NONE);
    int px = size_res.x;
    int label_w = lv_obj_get_width(label);

    // 文本长度短于控件宽度时不滚动
    if (px <= label_w) {
        lv_obj_set_style_anim_duration(label, 0, LV_PART_MAIN);
        return;
    }
    // 计算动画持续时间：滚动距离 = 文本宽 + 标签宽
    int duration = (px + label_w) * 1000 / px_per_sec;
    // 限制 duration 不超过翻页间隔
    int max_duration = refresh_interval_ * 1000 - 200;
    if (duration > max_duration) duration = max_duration;
    // 应用动画时长
    lv_obj_set_style_anim_duration(label, duration, LV_PART_MAIN);
    ESP_LOGI(TAG, "SetLabelScrollSpeed: txt='%s' width=%d label_w=%d duration=%dms",
             txt, px, label_w, duration);
}

