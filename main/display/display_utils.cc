#include "display_utils.h"
#include "system_info.h"
#include "board.h"
#include <cJSON.h>
#include <esp_log.h>
#include <lvgl.h>

#define TAG "DisplayUtils"

void display_utils::LoadIdleInfoSettings(std::string* idle_info_texts, std::vector<int>& valid_idle_indices, int& refresh_interval) {
    ESP_LOGI(TAG, "Loading idle info settings via network request");
    
    // 获取网络实例
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(2);
    if (!http) {
        ESP_LOGE(TAG, "Failed to create HTTP client for display info");
        return;
    }

    // 设置较短超时，避免服务器无响应时后台任务长时间挂起（/device_display 异常时会卡死）
    http->SetTimeout(10000);
    
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("Device-ID", SystemInfo::GetMacAddress().c_str());
    std::string url = std::string(CONFIG_SERVER_URL) + "/device_display";
    // ESP_LOGI(TAG, "Fetching display info from: %s", url.c_str());
    
    if (http->Open("GET", url)) {
        int status_code = http->GetStatusCode();
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
                            idle_info_texts[0] = std::string(weather_str->valuestring);
                            ESP_LOGI(TAG, "Loaded weather: %s", weather_str->valuestring);
                        }
                        cJSON* aqi_str = cJSON_GetObjectItem(weather_obj, "aqi");
                        if (aqi_str) {
                            idle_info_texts[1] = std::string(aqi_str->valuestring);
                            ESP_LOGI(TAG, "Loaded aqi: %s", aqi_str->valuestring);
                        }
                    }
                    // 解析并保存时间信息
                    cJSON* timeday_obj = cJSON_GetObjectItem(data_obj, "timeday");
                    if (timeday_obj) {
                        cJSON* day_y_item = cJSON_GetObjectItem(timeday_obj, "day_y");
                        if (day_y_item) {
                            idle_info_texts[2] = std::string(day_y_item->valuestring);
                            ESP_LOGI(TAG, "Loaded day_y: %s", day_y_item->valuestring);
                        }
                        
                        cJSON* day_n_item = cJSON_GetObjectItem(timeday_obj, "day_n");
                        if (day_n_item) {
                            idle_info_texts[3] = std::string(day_n_item->valuestring);
                            ESP_LOGI(TAG, "Loaded day_n: %s", day_n_item->valuestring);
                        }
                        
                        cJSON* wuhou_item = cJSON_GetObjectItem(timeday_obj, "wuhou");
                        if (wuhou_item) {
                            idle_info_texts[4] = std::string(wuhou_item->valuestring);
                            ESP_LOGI(TAG, "Loaded wuhou: %s", wuhou_item->valuestring);
                        }
                        
                        cJSON* xiuluck_item = cJSON_GetObjectItem(timeday_obj, "xiuluck");
                        if (xiuluck_item) {
                            idle_info_texts[5] = std::string(xiuluck_item->valuestring);
                            ESP_LOGI(TAG, "Loaded xiuluck: %s", xiuluck_item->valuestring);
                        }
                        
                        cJSON* yiji_item = cJSON_GetObjectItem(timeday_obj, "yiji");
                        if (yiji_item) {
                            idle_info_texts[6] = std::string(yiji_item->valuestring);
                        }
                        
                        cJSON* today_history_item = cJSON_GetObjectItem(timeday_obj, "today_history");
                        if (today_history_item) {
                            idle_info_texts[7] = std::string(today_history_item->valuestring);
                            ESP_LOGI(TAG, "Loaded today_history: %s", today_history_item->valuestring);
                        }
                    }
                    
                    // 解析并保存设置信息
                    cJSON* settings_obj = cJSON_GetObjectItem(data_obj, "settings");
                    if (settings_obj) {
                        cJSON* refresh_interval_item = cJSON_GetObjectItem(settings_obj, "refresh_interval");
                        if (refresh_interval_item) {
                            refresh_interval = refresh_interval_item->valueint;
                            ESP_LOGI(TAG, "Loaded refresh interval: %d", refresh_interval);
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
    valid_idle_indices.clear();
    for (int i = 0; i < 8; i++) {
        if (!idle_info_texts[i].empty()) {
            valid_idle_indices.push_back(i);
        }
    }
    ESP_LOGI(TAG, "Loaded idle info settings with %d valid entries, refresh interval: %d", 
             valid_idle_indices.size(), refresh_interval);
}

void display_utils::SetLabelScrollSpeed(lv_obj_t* label, int px_per_sec, int refresh_interval) {
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
    int max_duration = refresh_interval * 1000 - 200;
    if (duration > max_duration) duration = max_duration;
    // 应用动画时长
    lv_obj_set_style_anim_duration(label, duration, LV_PART_MAIN);
    ESP_LOGI(TAG, "SetLabelScrollSpeed: txt='%s' width=%d label_w=%d duration=%dms",
             txt, px, label_w, duration);
}