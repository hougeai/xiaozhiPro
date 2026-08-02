/*
 * MCP Server Implementation
 * Reference: https://modelcontextprotocol.io/specification/2024-11-05
 */

#include "mcp_server.h"
#include <esp_log.h>
#include <esp_app_desc.h>
#include <algorithm>
#include <cstring>
#include <esp_pthread.h>
#include <memory>

#include "application.h"
#include "display.h"
#include "oled_display.h"
#include "board.h"
#include "alarm.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "lvgl_display.h"
#include "system_info.h"
#include "led/gpio_led.h" 

#define TAG "MCP"

McpServer::McpServer() {
}

McpServer::~McpServer() {
    for (auto tool : tools_) {
        delete tool;
    }
    tools_.clear();
}

void McpServer::AddCommonTools() {
    // *Important* To speed up the response time, we add the common tools to the beginning of
    // the tools list to utilize the prompt cache.
    // **重要** 为了提升响应速度，我们把常用的工具放在前面，利用 prompt cache 的特性。

    // Backup the original tools list and restore it after adding the common tools.
    auto original_tools = std::move(tools_);
    auto& board = Board::GetInstance();

    // Do not add custom tools here.
    // Custom tools must be added in the board's InitializeTools function.

    AddTool("self.get_device_status",
        "Provides the real-time information of the device, including the current status of the audio speaker, screen, battery, network, etc.\n"
        "Use this tool for: \n"
        "1. Answering questions about current condition (e.g. what is the current volume of the audio speaker?)\n"
        "2. As the first step to control the device (e.g. turn up / down the volume of the audio speaker, etc.)",
        PropertyList(),
        [&board](const PropertyList& properties) -> ReturnValue {
            return board.GetDeviceStatusJson();
        });

    AddTool("self.audio_speaker.set_volume", 
        "Set the volume of the audio speaker. If the current volume is unknown, you must call `self.get_device_status` tool first and then call this tool.",
        PropertyList({
            Property("volume", kPropertyTypeInteger, 0, 100)
        }), 
        [&board](const PropertyList& properties) -> ReturnValue {
            auto codec = board.GetAudioCodec();
            codec->SetOutputVolume(properties["volume"].value<int>());
            return true;
        });
    
    auto backlight = board.GetBacklight();
    if (backlight) {
        AddTool("self.screen.set_brightness",
            "Set the brightness of the screen.",
            PropertyList({
                Property("brightness", kPropertyTypeInteger, 0, 100)
            }),
            [backlight](const PropertyList& properties) -> ReturnValue {
                uint8_t brightness = static_cast<uint8_t>(properties["brightness"].value<int>());
                backlight->SetBrightness(brightness, true);
                return true;
            });
    }

#ifdef HAVE_LVGL
    auto display = board.GetDisplay();
    if (display && display->GetTheme() != nullptr) {
        AddTool("self.screen.set_theme",
            "Set the theme of the screen. The theme can be `light` or `dark`.",
            PropertyList({
                Property("theme", kPropertyTypeString)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto theme_name = properties["theme"].value<std::string>();
                auto& theme_manager = LvglThemeManager::GetInstance();
                auto theme = theme_manager.GetTheme(theme_name);
                if (theme != nullptr) {
                    display->SetTheme(theme);
                    return true;
                }
                return false;
            });
    }

    auto camera = board.GetCamera();
    if (camera) {
        AddTool("self.camera.take_photo",
            "Always remember you have a camera. If the user asks you to see something, use this tool to take a photo and then explain it.\n"
            "Args:\n"
            "  `question`: The question that you want to ask about the photo.\n"
            "Return:\n"
            "  A JSON object that provides the photo information.",
            PropertyList({
                Property("question", kPropertyTypeString)
            }),
            [camera](const PropertyList& properties) -> ReturnValue {
                // Lower the priority to do the camera capture
                TaskPriorityReset priority_reset(1);

                if (!camera->Capture()) {
                    throw std::runtime_error("Failed to capture photo");
                }
                auto question = properties["question"].value<std::string>();
                return camera->Explain(question);
            });
    }
#endif
    
    // led
    auto led = board.GetManualLed();
    auto gpio_led = dynamic_cast<GpioLed*>(led);
    if (gpio_led) {
        AddTool("self.led.turn_on",
            "Turn on the built-in LED",
            PropertyList(),
            [gpio_led](const PropertyList& properties) -> ReturnValue {
                gpio_led->SetBrightness(50); 
                gpio_led->TurnOn();
                return true;
            });    
        AddTool("self.led.turn_off",
            "Turn off the built-in LED",
            PropertyList(),
            [gpio_led](const PropertyList& properties) -> ReturnValue {
                gpio_led->TurnOff();
                return true;
            });
        AddTool("self.led.set_brightness",
            "Set the brightness of the built-in LED",
            PropertyList({
                Property("brightness", kPropertyTypeInteger, 0, 100)
            }),
            [gpio_led](const PropertyList& properties) -> ReturnValue {
                uint8_t brightness = static_cast<uint8_t>(properties["brightness"].value<int>());
                gpio_led->SetBrightness(brightness);
                return true;
            });
    }
    // alarm
    AddTool("self.alarm.set",
        "为用户添加各种闹钟提醒功能，比如：帮我定一个10分钟的闹钟；20秒后提醒我关煤气炉；每天下午2点提醒我吃药"
        "Args:"
        "delay: int, 以秒为单位，多久之后执行；如果是定时的闹钟那么delay为-1"
        "hour: int,  表示时；如早上7点 the hour can be 7 ,晚上8点 the hour can be 20，如果没明确说 the hour can be -1"
        "minute: int, 分钟；早上7:00， the minute can be 0， 如果7点半 the minute can be 30 如果没明确说 the minute can be -1"
        "repeat: int , 表示重复次数；如果没说则默认1次，否则为具体的次数，如果说每天/每小时等重复但是没有具体次数的，最多重复10000次"
        "interval: int, 提醒间隔时间，单位为秒；repeat > 0时生效，比如每小时提醒我，interval should be 3600，每天提醒我，interval should be 86400"
        "name: string, 提醒名称"
        "Return:"
        "A JSON object that indicates whether the alarm is set successfully.",
        PropertyList({
            Property("delay", kPropertyTypeInteger),
            Property("hour", kPropertyTypeInteger),
            Property("minute", kPropertyTypeInteger),
            Property("repeat", kPropertyTypeInteger),
            Property("interval", kPropertyTypeInteger),
            Property("name", kPropertyTypeString)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            if(app.alarm_m_ == nullptr){
                ESP_LOGE(TAG, "AlarmManager is nullptr");
                 return R"({"success":false,"message":"AlarmManager is not initialized"})";
            }
            // ESP_LOGI(TAG, "SetAlarm");
            int delay = properties["delay"].value<int>();
            int hour = properties["hour"].value<int>();
            int minute = properties["minute"].value<int>();
            int repeat = properties["repeat"].value<int>();
            int interval = properties["interval"].value<int>();
            std::string name = properties["name"].value<std::string>();
            ESP_LOGI(TAG, "SetAlarm delay %d hour %d minute %d repeat %d interval %d name %s", delay, hour, minute, repeat, interval, name.c_str());
            std::string result = app.alarm_m_->SetAlarm(delay, hour, minute, repeat, interval, name);
            if (result.empty()) {
                return R"({"success":true,"message":"Alarm set successfully"})";
            } else {
                return std::string(R"({"success":false,"message":")" + result + R"("})");
            }
        });
    AddTool("self.alarm.del",
        "根据id删除一个闹钟提醒，首先用self.alarm.queryall查询所有闹钟，然后基于用户意图给出闹钟列表中的一个id，删除前需要和用户确认"
        "Args:\n"
        "  `id`: The id of the alarm.\n"
        "Return:\n"
        "  A JSON object that indicates whether the alarm is deleted successfully.",
        PropertyList({
            Property("id", kPropertyTypeInteger)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            if(app.alarm_m_ == nullptr){
                ESP_LOGE(TAG, "AlarmManager is nullptr");
                 return R"({"success":false,"message":"AlarmManager is not initialized"})";
            }
            ESP_LOGI(TAG, "DelAlarm");
            int id = properties["id"].value<int>();
            std::string result = app.alarm_m_->DelAlarmById(id);
            if (result.empty()) {
                return R"({"success":true,"message":"Alarm delete successfully"})";
            } else {
                return std::string(R"({"success":false,"message":")" + result + R"("})");
            }
        });
    AddTool("self.alarm.cleanup",
        "清空所有的提醒和闹钟；比如：我想清空/关闭所有的提醒和闹钟",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            if(app.alarm_m_ == nullptr){
                ESP_LOGE(TAG, "AlarmManager is nullptr");
                 return R"({"success":false,"message":"AlarmManager is not initialized"})";
            }
            ESP_LOGI(TAG, "CleanUp Alarm");
            std::string result = app.alarm_m_->CleanUpAlarm();
            if (!result.empty()) {
                return std::string(R"({"success":false,"message":")" + result + R"("})");
            }
            return R"({"success":true,"message":"Alarm cleaned up successfully"})";
        });
    AddTool("self.alarm.queryall",
        "查询所有的提醒和闹钟",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            if(app.alarm_m_ == nullptr){
                ESP_LOGE(TAG, "AlarmManager is nullptr");
                 return R"({"success":false,"message":"AlarmManager is not initialized"})";
            }
            ESP_LOGI(TAG, "Query All Alarm");
            std::string alarm_list = app.alarm_m_->QueryAllAlarm();
            return std::string(R"({"success":true,"alarms":)") + alarm_list + R"(})";
        });
    AddTool("self.alarm.reload",
        "重新加载所有的提醒和闹钟",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            if(app.alarm_m_ == nullptr){
                ESP_LOGE(TAG, "AlarmManager is nullptr");
                 return R"({"success":false,"message":"AlarmManager is not initialized"})";
            }
            ESP_LOGI(TAG, "Reload All Alarm");
            std::string result = app.alarm_m_->ReloadAlarms();
            if (!result.empty()) {
                return std::string(R"({"success":false,"message":")" + result + R"("})");
            }
            return R"({"success":true,"message":"Alarm reloaded successfully"})";
        });
    // knowledge base
    AddTool("self.kb.query",
        "当用户咨询的问题超出你的认知范围，不可胡编乱造，调用本工具发起检索。"
        "本工具为异步：调用后立即返回'知识库检索中'，并非检索结果，应告知用户正在检索请稍候；检索完成后会收到'知识库报告'。"
        "Args:"
        "query: str, 用户的原始问题（自然语句，不要提炼关键词）",
        PropertyList({
            Property("query", kPropertyTypeString)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto http = Board::GetInstance().GetNetwork()->CreateHttp(1);
            http->SetHeader("Content-Type", "application/json");
            http->SetHeader("Device-ID", SystemInfo::GetMacAddress().c_str());
            http->SetTimeout(5000); // 设置超时时间
            // 构造请求的JSON数据
            std::string query = properties["query"].value<std::string>();
            if (query.empty()) {
                return R"({"success":false,"message":"Query is empty"})";
            }
            cJSON* json_data = cJSON_CreateObject();
            cJSON_AddStringToObject(json_data, "query", query.c_str());
            char* json_string = cJSON_PrintUnformatted(json_data);
            if (!json_string) {
                cJSON_Delete(json_data);
                return R"({"success":false,"message":"Failed to serialize JSON"})";
            }
            std::string data(json_string);
            cJSON_free(json_string);
            cJSON_Delete(json_data);
            http->SetContent(std::move(data));
            std::string base_url = std::string(CONFIG_SERVER_URL) + "/kb_search";
    
            if (!http->Open("POST", base_url)) {
                ESP_LOGE(TAG, "Failed to open HTTP connection");
                return R"({"success":false,"message":"Failed to connect to knowledge base"})";
            }
            int status_code = http->GetStatusCode();
            std::string response = http->ReadAll();
            ESP_LOGI(TAG, "kb.query response: %s", response.c_str());
            cJSON* root = cJSON_Parse(response.c_str());
            http->Close();
            if (status_code != 200) {
                ESP_LOGE(TAG, "Knowledge base request failed with status code: %d", status_code);
                if (root) {
                    cJSON* msg = cJSON_GetObjectItem(root, "msg");
                    std::string error_msg = msg ? msg->valuestring : "Unknown error";
                    ESP_LOGE(TAG, "Knowledge base error message: %s", error_msg.c_str());
                    cJSON_Delete(root);
                    return std::string(R"({"success":false,"message":")" + error_msg + R"("})");
                }
                return R"({"success":false,"message":"Knowledge base request failed"})";
            }
            
            if (!root) {
                ESP_LOGE(TAG, "Failed to parse knowledge base response");
                return R"({"success":false,"message":"Failed to parse knowledge base response"})";
            }
            cJSON* data_item = cJSON_GetObjectItem(root, "data");
            if (!data_item || !cJSON_IsString(data_item)) {
                cJSON_Delete(root);
                return R"({"success":false,"message":"Empty"})";
            }
            std::string data_str = data_item->valuestring;
            cJSON_Delete(root);
            // 构造简化后的响应
            cJSON* result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "data", data_str.c_str());
            cJSON_AddBoolToObject(result, "success", true);
            char* result_json = cJSON_PrintUnformatted(result);
            std::string result_str(result_json);
            cJSON_free(result_json);
            cJSON_Delete(result);
            return result_str;
        });
    AddTool("self.kb.retrieve",
        "当且仅当收到'知识库报告'时，调用此工具检索最新消息，否则不要调用",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto http = Board::GetInstance().GetNetwork()->CreateHttp(1);
            http->SetHeader("Content-Type", "application/json");
            http->SetHeader("Device-ID", SystemInfo::GetMacAddress().c_str());
            http->SetTimeout(5000); // 设置超时时间
            std::string base_url = std::string(CONFIG_SERVER_URL) + "/kb_retrieve";
    
            if (!http->Open("GET", base_url)) {
                ESP_LOGE(TAG, "Failed to open HTTP connection");
                return R"({"success":false,"message":"Failed to connect to kb"})";
            }
            int status_code = http->GetStatusCode();
            std::string response = http->ReadAll();
            // ESP_LOGI(TAG, "Mem response: %s", response.c_str());
            cJSON* root = cJSON_Parse(response.c_str());
            http->Close();
            if (status_code != 200) {
                ESP_LOGE(TAG, "kb retrieve failed with status code: %d", status_code);
                if (root) {
                    cJSON* msg = cJSON_GetObjectItem(root, "msg");
                    std::string error_msg = msg ? msg->valuestring : "Unknown error";
                    ESP_LOGE(TAG, "kb retrieve error message: %s", error_msg.c_str());
                    cJSON_Delete(root);
                    return std::string(R"({"success":false,"message":")" + error_msg + R"("})");
                }
                return R"({"success":false,"message":"kb retrieve failed"})";
            }
            
            if (!root) {
                ESP_LOGE(TAG, "Failed to parse kb retrieve response");
                return R"({"success":false,"message":"Failed to parse kb retrieve response"})";
            }
            cJSON* data_item = cJSON_GetObjectItem(root, "data");
            if (!data_item || !cJSON_IsString(data_item)) {
                cJSON_Delete(root);
                return R"({"success":false,"message":"Empty"})";
            }
            std::string data_str = data_item->valuestring;
            cJSON_Delete(root);
            // 构造简化后的响应
            cJSON* result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "data", data_str.c_str());
            cJSON_AddBoolToObject(result, "success", true);
            char* result_json = cJSON_PrintUnformatted(result);
            std::string result_str(result_json);
            cJSON_free(result_json);
            cJSON_Delete(result);
            return result_str;
        });
    // 长期记忆
    AddTool("self.memory.search",
        "搜索用户个人记忆，获取与当前话题相关的历史信息。以下场景必须调用此工具：1) 用户提到'之前/上次/以前'等时间回溯词；2) 用户提及具体人名、地点等专有名词且可能存在于记忆中；3) 用户要求继续/修改之前的讨论；4) 回答需结合用户个人偏好、习惯或历史背景"
        "Args:"
        "query: str, 检索词，多个检索词用空格隔开",
        PropertyList({
            Property("query", kPropertyTypeString)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto http = Board::GetInstance().GetNetwork()->CreateHttp(1);
            http->SetHeader("Content-Type", "application/json");
            http->SetHeader("Device-ID", SystemInfo::GetMacAddress().c_str());
            http->SetTimeout(5000); // 设置超时时间
            // 构造请求的JSON数据
            std::string query = properties["query"].value<std::string>();
            if (query.empty()) {
                return R"({"success":false,"message":"Query is empty"})";
            }
            cJSON* json_data = cJSON_CreateObject();
            cJSON_AddStringToObject(json_data, "query", query.c_str());
            char* json_string = cJSON_PrintUnformatted(json_data);
            if (!json_string) {
                cJSON_Delete(json_data);
                return R"({"success":false,"message":"Failed to serialize JSON"})";
            }
            std::string data(json_string);
            cJSON_free(json_string);
            cJSON_Delete(json_data);
            http->SetContent(std::move(data));
            std::string base_url = std::string(CONFIG_SERVER_URL) + "/mem_search";
    
            if (!http->Open("POST", base_url)) {
                ESP_LOGE(TAG, "Failed to open HTTP connection");
                return R"({"success":false,"message":"Failed to connect to mem"})";
            }
            int status_code = http->GetStatusCode();
            std::string response = http->ReadAll();
            // ESP_LOGI(TAG, "Mem response: %s", response.c_str());
            cJSON* root = cJSON_Parse(response.c_str());
            http->Close();
            if (status_code != 200) {
                ESP_LOGE(TAG, "Mem request failed with status code: %d", status_code);
                if (root) {
                    cJSON* msg = cJSON_GetObjectItem(root, "msg");
                    std::string error_msg = msg ? msg->valuestring : "Unknown error";
                    ESP_LOGE(TAG, "Mem error message: %s", error_msg.c_str());
                    cJSON_Delete(root);
                    return std::string(R"({"success":false,"message":")" + error_msg + R"("})");
                }
                return R"({"success":false,"message":"Mem request failed"})";
            }
            
            if (!root) {
                ESP_LOGE(TAG, "Failed to parse mem response");
                return R"({"success":false,"message":"Failed to parse mem response"})";
            }
            cJSON* data_item = cJSON_GetObjectItem(root, "data");
            if (!data_item || !cJSON_IsString(data_item)) {
                cJSON_Delete(root);
                return R"({"success":false,"message":"Empty"})";
            }
            std::string data_str = data_item->valuestring;
            cJSON_Delete(root);
            // 构造简化后的响应
            cJSON* result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "data", data_str.c_str());
            cJSON_AddBoolToObject(result, "success", true);
            char* result_json = cJSON_PrintUnformatted(result);
            std::string result_str(result_json);
            cJSON_free(result_json);
            cJSON_Delete(result);
            return result_str;
        });
    // 视觉理解
    AddTool("self.vision.analysis",
        "当用户要求你看东西或打开摄像头时，请调用此工具获取视觉分析内容。"
        "Args:\n"
        "  `question`: The question that you want to ask about the vision.\n"
        "Return:\n"
        "  A JSON object that provides the vision information.",
        PropertyList({
            Property("question", kPropertyTypeString)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto http = Board::GetInstance().GetNetwork()->CreateHttp(1);
            http->SetHeader("Content-Type", "application/json");
            http->SetHeader("Device-ID", SystemInfo::GetMacAddress().c_str());
            http->SetTimeout(8000); // 设置超时时间
            // 构造请求的JSON数据
            std::string question = properties["question"].value<std::string>();
            cJSON* json_data = cJSON_CreateObject();
            cJSON_AddStringToObject(json_data, "question", question.c_str());
            char* json_string = cJSON_PrintUnformatted(json_data);
            std::string data(json_string);
            cJSON_free(json_string);
            cJSON_Delete(json_data);
            http->SetContent(std::move(data));
            std::string base_url = std::string(CONFIG_SERVER_URL) + "/api/v1/vision/analysis";
    
            if (!http->Open("POST", base_url)) {
                ESP_LOGE(TAG, "Failed to open HTTP connection");
                return R"({"success":false,"message":"Failed to connect to vision"})";
            }
            int status_code = http->GetStatusCode();
            std::string response = http->ReadAll();
            // ESP_LOGI(TAG, "Mem response: %s", response.c_str());
            cJSON* root = cJSON_Parse(response.c_str());
            http->Close();
            if (status_code != 200) {
                ESP_LOGE(TAG, "Vision request failed with status code: %d", status_code);
                if (root) {
                    cJSON* msg = cJSON_GetObjectItem(root, "msg");
                    std::string error_msg = msg ? msg->valuestring : "Unknown error";
                    ESP_LOGE(TAG, "Vision error message: %s", error_msg.c_str());
                    cJSON_Delete(root);
                    return std::string(R"({"success":false,"message":")" + error_msg + R"("})");
                }
                return R"({"success":false,"message":"Vision request failed"})";
            }
            
            if (!root) {
                ESP_LOGE(TAG, "Failed to parse Vision response");
                return R"({"success":false,"message":"Failed to parse Vision response"})";
            }
            cJSON* data_item = cJSON_GetObjectItem(root, "data");
            if (!data_item || !cJSON_IsString(data_item)) {
                cJSON_Delete(root);
                return R"({"success":false,"message":"Empty"})";
            }
            std::string data_str = data_item->valuestring;
            cJSON_Delete(root);
            // 构造简化后的响应
            cJSON* result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "data", data_str.c_str());
            cJSON_AddBoolToObject(result, "success", true);
            char* result_json = cJSON_PrintUnformatted(result);
            std::string result_str(result_json);
            cJSON_free(result_json);
            cJSON_Delete(result);
            return result_str;
        });
    // openclaw
    AddTool("self.openclaw.send",
        "当用户要求你给openclaw(欧克劳)发消息时，请调用此工具发送消息内容。"
        "Args:\n"
        "  `message`: The message that you should send to openclaw.\n"
        "Return:\n"
        "  A JSON object that provides the response from openclaw.",
        PropertyList({
            Property("message", kPropertyTypeString)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto http = Board::GetInstance().GetNetwork()->CreateHttp(1);
            http->SetHeader("Content-Type", "application/json");
            http->SetHeader("Device-ID", SystemInfo::GetMacAddress().c_str());
            http->SetTimeout(10000); // 设置超时时间
            // 构造请求的JSON数据
            std::string message = properties["message"].value<std::string>();
            cJSON* json_data = cJSON_CreateObject();
            cJSON_AddStringToObject(json_data, "message", message.c_str());
            char* json_string = cJSON_PrintUnformatted(json_data);
            std::string data(json_string);
            cJSON_free(json_string);
            cJSON_Delete(json_data);
            http->SetContent(std::move(data));
            std::string base_url = std::string(CONFIG_SERVER_URL) + "/openclaw_send";
    
            if (!http->Open("POST", base_url)) {
                ESP_LOGE(TAG, "Failed to open HTTP connection");
                return R"({"success":false,"message":"Failed to connect to openclaw"})";
            }
            int status_code = http->GetStatusCode();
            std::string response = http->ReadAll();
            // ESP_LOGI(TAG, "openclaw.send response: %s", response.c_str());
            cJSON* root = cJSON_Parse(response.c_str());
            http->Close();
            if (status_code != 200) {
                ESP_LOGE(TAG, "Openclaw request failed with status code: %d", status_code);
                if (root) {
                    cJSON* msg = cJSON_GetObjectItem(root, "msg");
                    std::string error_msg = msg ? msg->valuestring : "Unknown error";
                    ESP_LOGE(TAG, "Openclaw error message: %s", error_msg.c_str());
                    cJSON_Delete(root);
                    return std::string(R"({"success":false,"message":")" + error_msg + R"("})");
                }
                return R"({"success":false,"message":"Openclaw request failed"})";
            }
            
            if (!root) {
                ESP_LOGE(TAG, "Failed to parse Openclaw response");
                return R"({"success":false,"message":"Failed to parse Openclaw response"})";
            }
            cJSON* data_item = cJSON_GetObjectItem(root, "data");
            if (!data_item || !cJSON_IsString(data_item)) {
                cJSON_Delete(root);
                return R"({"success":false,"message":"Empty"})";
            }
            std::string data_str = data_item->valuestring;
            cJSON_Delete(root);
            // 构造简化后的响应
            cJSON* result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "data", data_str.c_str());
            cJSON_AddBoolToObject(result, "success", true);
            char* result_json = cJSON_PrintUnformatted(result);
            std::string result_str(result_json);
            cJSON_free(result_json);
            cJSON_Delete(result);
            return result_str;
        });
    AddTool("self.openclaw.retrieve",
        "当收到'欧克劳报告'时，请调用此工具检索openclaw(欧克劳)最新消息",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto http = Board::GetInstance().GetNetwork()->CreateHttp(1);
            http->SetHeader("Content-Type", "application/json");
            http->SetHeader("Device-ID", SystemInfo::GetMacAddress().c_str());
            http->SetTimeout(10000); // 设置超时时间
            std::string base_url = std::string(CONFIG_SERVER_URL) + "/openclaw_retrieve";
    
            if (!http->Open("GET", base_url)) {
                ESP_LOGE(TAG, "Failed to open HTTP connection");
                return R"({"success":false,"message":"Failed to connect to openclaw"})";
            }
            int status_code = http->GetStatusCode();
            std::string response = http->ReadAll();
            // ESP_LOGI(TAG, "openclaw.retrieve response: %s", response.c_str());
            cJSON* root = cJSON_Parse(response.c_str());
            http->Close();
            if (status_code != 200) {
                ESP_LOGE(TAG, "Openclaw request failed with status code: %d", status_code);
                if (root) {
                    cJSON* msg = cJSON_GetObjectItem(root, "msg");
                    std::string error_msg = msg ? msg->valuestring : "Unknown error";
                    ESP_LOGE(TAG, "Openclaw error message: %s", error_msg.c_str());
                    cJSON_Delete(root);
                    return std::string(R"({"success":false,"message":")" + error_msg + R"("})");
                }
                return R"({"success":false,"message":"Openclaw request failed"})";
            }
            
            if (!root) {
                ESP_LOGE(TAG, "Failed to parse Openclaw response");
                return R"({"success":false,"message":"Failed to parse Openclaw response"})";
            }
            cJSON* data_item = cJSON_GetObjectItem(root, "data");
            if (!data_item || !cJSON_IsString(data_item)) {
                cJSON_Delete(root);
                return R"({"success":false,"message":"Empty"})";
            }
            std::string data_str = data_item->valuestring;
            cJSON_Delete(root);
            // 构造简化后的响应
            cJSON* result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "data", data_str.c_str());
            cJSON_AddBoolToObject(result, "success", true);
            char* result_json = cJSON_PrintUnformatted(result);
            std::string result_str(result_json);
            cJSON_free(result_json);
            cJSON_Delete(result);
            return result_str;
        });
    // hermes
    AddTool("self.hermes.send",
        "当用户要求你给hermes(爱马仕)发消息时，请调用此工具发送消息内容。"
        "Args:\n"
        "  `message`: The message that you should send to hermes.\n"
        "Return:\n"
        "  A JSON object that provides the response from hermes.",
        PropertyList({
            Property("message", kPropertyTypeString)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto http = Board::GetInstance().GetNetwork()->CreateHttp(1);
            http->SetHeader("Content-Type", "application/json");
            http->SetHeader("Device-ID", SystemInfo::GetMacAddress().c_str());
            http->SetTimeout(10000); // 设置超时时间
            // 构造请求的JSON数据
            std::string message = properties["message"].value<std::string>();
            cJSON* json_data = cJSON_CreateObject();
            cJSON_AddStringToObject(json_data, "message", message.c_str());
            char* json_string = cJSON_PrintUnformatted(json_data);
            std::string data(json_string);
            cJSON_free(json_string);
            cJSON_Delete(json_data);
            http->SetContent(std::move(data));
            std::string base_url = std::string(CONFIG_SERVER_URL) + "/hermes_send";
    
            if (!http->Open("POST", base_url)) {
                ESP_LOGE(TAG, "Failed to open HTTP connection");
                return R"({"success":false,"message":"Failed to connect to hermes"})";
            }
            int status_code = http->GetStatusCode();
            std::string response = http->ReadAll();
            // ESP_LOGI(TAG, "hermes.send response: %s", response.c_str());
            cJSON* root = cJSON_Parse(response.c_str());
            http->Close();
            if (status_code != 200) {
                ESP_LOGE(TAG, "hermes request failed with status code: %d", status_code);
                if (root) {
                    cJSON* msg = cJSON_GetObjectItem(root, "msg");
                    std::string error_msg = msg ? msg->valuestring : "Unknown error";
                    ESP_LOGE(TAG, "hermes error message: %s", error_msg.c_str());
                    cJSON_Delete(root);
                    return std::string(R"({"success":false,"message":")" + error_msg + R"("})");
                }
                return R"({"success":false,"message":"hermes request failed"})";
            }
            
            if (!root) {
                ESP_LOGE(TAG, "Failed to parse hermes response");
                return R"({"success":false,"message":"Failed to parse hermes response"})";
            }
            cJSON* data_item = cJSON_GetObjectItem(root, "data");
            if (!data_item || !cJSON_IsString(data_item)) {
                cJSON_Delete(root);
                return R"({"success":false,"message":"Empty"})";
            }
            std::string data_str = data_item->valuestring;
            cJSON_Delete(root);
            // 构造简化后的响应
            cJSON* result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "data", data_str.c_str());
            cJSON_AddBoolToObject(result, "success", true);
            char* result_json = cJSON_PrintUnformatted(result);
            std::string result_str(result_json);
            cJSON_free(result_json);
            cJSON_Delete(result);
            return result_str;
        });
    AddTool("self.hermes.retrieve",
        "当收到'Hermes报告'时，请调用此工具检索hermes(爱马仕)最新消息",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto http = Board::GetInstance().GetNetwork()->CreateHttp(1);
            http->SetHeader("Content-Type", "application/json");
            http->SetHeader("Device-ID", SystemInfo::GetMacAddress().c_str());
            http->SetTimeout(10000); // 设置超时时间
            std::string base_url = std::string(CONFIG_SERVER_URL) + "/hermes_retrieve";
    
            if (!http->Open("GET", base_url)) {
                ESP_LOGE(TAG, "Failed to open HTTP connection");
                return R"({"success":false,"message":"Failed to connect to hermes"})";
            }
            int status_code = http->GetStatusCode();
            std::string response = http->ReadAll();
            // ESP_LOGI(TAG, "hermes.retrieve response: %s", response.c_str());
            cJSON* root = cJSON_Parse(response.c_str());
            http->Close();
            if (status_code != 200) {
                ESP_LOGE(TAG, "hermes request failed with status code: %d", status_code);
                if (root) {
                    cJSON* msg = cJSON_GetObjectItem(root, "msg");
                    std::string error_msg = msg ? msg->valuestring : "Unknown error";
                    ESP_LOGE(TAG, "hermes error message: %s", error_msg.c_str());
                    cJSON_Delete(root);
                    return std::string(R"({"success":false,"message":")" + error_msg + R"("})");
                }
                return R"({"success":false,"message":"hermes request failed"})";
            }
            
            if (!root) {
                ESP_LOGE(TAG, "Failed to parse hermes response");
                return R"({"success":false,"message":"Failed to parse hermes response"})";
            }
            cJSON* data_item = cJSON_GetObjectItem(root, "data");
            if (!data_item || !cJSON_IsString(data_item)) {
                cJSON_Delete(root);
                return R"({"success":false,"message":"Empty"})";
            }
            std::string data_str = data_item->valuestring;
            cJSON_Delete(root);
            // 构造简化后的响应
            cJSON* result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "data", data_str.c_str());
            cJSON_AddBoolToObject(result, "success", true);
            char* result_json = cJSON_PrintUnformatted(result);
            std::string result_str(result_json);
            cJSON_free(result_json);
            cJSON_Delete(result);
            return result_str;
        });
    // Home Assistant
    AddTool("self.ha.sensors",
        "当用户询问 Home Assistant (HA) 智能家居传感器数据（如温度、湿度、滤芯寿命、网络速度等）时，调用此工具获取所有传感器列表。"
        "返回按设备分组的传感器数据，每个传感器包含 entity_id、name、state、unit 字段。",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto http = Board::GetInstance().GetNetwork()->CreateHttp(1);
            http->SetHeader("Content-Type", "application/json");
            http->SetHeader("Device-ID", SystemInfo::GetMacAddress().c_str());
            http->SetTimeout(10000);
            std::string base_url = std::string(CONFIG_SERVER_URL) + "/ha_sensors";

            if (!http->Open("GET", base_url)) {
                ESP_LOGE(TAG, "Failed to open HTTP connection");
                return R"({"success":false,"message":"Failed to connect to HA"})";
            }
            int status_code = http->GetStatusCode();
            std::string response = http->ReadAll();
            cJSON* root = cJSON_Parse(response.c_str());
            http->Close();
            if (status_code != 200) {
                ESP_LOGE(TAG, "HA sensors request failed with status code: %d", status_code);
                if (root) {
                    cJSON* msg = cJSON_GetObjectItem(root, "msg");
                    std::string error_msg = msg ? msg->valuestring : "Unknown error";
                    cJSON_Delete(root);
                    return std::string(R"({"success":false,"message":")" + error_msg + R"("})");
                }
                return R"({"success":false,"message":"HA sensors request failed"})";
            }
            if (!root) {
                ESP_LOGE(TAG, "Failed to parse HA sensors response");
                return R"({"success":false,"message":"Failed to parse HA response"})";
            }
            cJSON* data_item = cJSON_GetObjectItem(root, "data");
            if (!data_item) {
                cJSON_Delete(root);
                return R"({"success":false,"message":"Empty"})";
            }
            char* data_json = cJSON_PrintUnformatted(data_item);
            std::string data_str(data_json);
            cJSON_free(data_json);
            cJSON_Delete(root);
            cJSON* result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "success", true);
            cJSON_AddStringToObject(result, "data", data_str.c_str());
            char* result_json = cJSON_PrintUnformatted(result);
            std::string result_str(result_json);
            cJSON_free(result_json);
            cJSON_Delete(result);
            return result_str;
        });
    AddTool("self.ha.controls",
        "当用户想要控制 Home Assistant (HA) 智能家居设备（如开灯、关灯、打开摄像头移动检测等）时，先调用此工具获取可控制设备列表。"
        "返回按设备分组的可控制设备，每个设备包含 entity_id、name、state、domain、services 字段。"
        "你需要从中找到用户想控制的设备对应的 entity_id 和可用的 service，然后调用 self.ha.control 工具执行控制。",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto http = Board::GetInstance().GetNetwork()->CreateHttp(1);
            http->SetHeader("Content-Type", "application/json");
            http->SetHeader("Device-ID", SystemInfo::GetMacAddress().c_str());
            http->SetTimeout(10000);
            std::string base_url = std::string(CONFIG_SERVER_URL) + "/ha_controls";

            if (!http->Open("GET", base_url)) {
                ESP_LOGE(TAG, "Failed to open HTTP connection");
                return R"({"success":false,"message":"Failed to connect to HA"})";
            }
            int status_code = http->GetStatusCode();
            std::string response = http->ReadAll();
            cJSON* root = cJSON_Parse(response.c_str());
            http->Close();
            if (status_code != 200) {
                ESP_LOGE(TAG, "HA controls request failed with status code: %d", status_code);
                if (root) {
                    cJSON* msg = cJSON_GetObjectItem(root, "msg");
                    std::string error_msg = msg ? msg->valuestring : "Unknown error";
                    cJSON_Delete(root);
                    return std::string(R"({"success":false,"message":")" + error_msg + R"("})");
                }
                return R"({"success":false,"message":"HA controls request failed"})";
            }
            if (!root) {
                ESP_LOGE(TAG, "Failed to parse HA controls response");
                return R"({"success":false,"message":"Failed to parse HA response"})";
            }
            cJSON* data_item = cJSON_GetObjectItem(root, "data");
            if (!data_item) {
                cJSON_Delete(root);
                return R"({"success":false,"message":"Empty"})";
            }
            char* data_json = cJSON_PrintUnformatted(data_item);
            std::string data_str(data_json);
            cJSON_free(data_json);
            cJSON_Delete(root);
            cJSON* result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "success", true);
            cJSON_AddStringToObject(result, "data", data_str.c_str());
            char* result_json = cJSON_PrintUnformatted(result);
            std::string result_str(result_json);
            cJSON_free(result_json);
            cJSON_Delete(result);
            return result_str;
        });
    AddTool("self.ha.control",
        "控制 Home Assistant (HA) 智能家居设备。调用前请先用 self.ha.controls 工具获取可用的 entity_id 和 service。"
        "Args:\n"
        "  `entity_id`: 设备实体ID，如 light.xxx、switch.xxx、camera.xxx\n"
        "  `service`: 服务名称，如 turn_on、turn_off、toggle、snapshot\n"
        "  `fields`: 额外参数（JSON字符串，无额外参数时传空对象 {}）\n"
        "Return:\n"
        "  A JSON object with entity_id, state and success.",
        PropertyList({
            Property("entity_id", kPropertyTypeString),
            Property("service", kPropertyTypeString),
            Property("fields", kPropertyTypeString, "{}")
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto http = Board::GetInstance().GetNetwork()->CreateHttp(1);
            http->SetHeader("Content-Type", "application/json");
            http->SetHeader("Device-ID", SystemInfo::GetMacAddress().c_str());
            http->SetTimeout(10000);
            // 构造请求JSON
            std::string entity_id = properties["entity_id"].value<std::string>();
            std::string service = properties["service"].value<std::string>();
            std::string fields_str = properties["fields"].value<std::string>();
            cJSON* json_data = cJSON_CreateObject();
            cJSON_AddStringToObject(json_data, "entity_id", entity_id.c_str());
            cJSON_AddStringToObject(json_data, "service", service.c_str());
            // 解析 fields 字符串为 JSON 对象，解析失败则用空对象
            cJSON* fields_json = cJSON_Parse(fields_str.c_str());
            if (fields_json && cJSON_IsObject(fields_json)) {
                cJSON_AddItemReferenceToObject(json_data, "fields", fields_json);
                char* json_string = cJSON_PrintUnformatted(json_data);
                std::string data(json_string);
                cJSON_free(json_string);
                cJSON_Delete(json_data);
                cJSON_Delete(fields_json);
                http->SetContent(std::move(data));
            } else {
                cJSON_AddNullToObject(json_data, "fields");
                char* json_string = cJSON_PrintUnformatted(json_data);
                std::string data(json_string);
                cJSON_free(json_string);
                cJSON_Delete(json_data);
                if (fields_json) cJSON_Delete(fields_json);
                http->SetContent(std::move(data));
            }
            std::string base_url = std::string(CONFIG_SERVER_URL) + "/ha_control";

            if (!http->Open("POST", base_url)) {
                ESP_LOGE(TAG, "Failed to open HTTP connection");
                return R"({"success":false,"message":"Failed to connect to HA"})";
            }
            int status_code = http->GetStatusCode();
            std::string response = http->ReadAll();
            cJSON* root = cJSON_Parse(response.c_str());
            http->Close();
            if (status_code != 200) {
                ESP_LOGE(TAG, "HA control request failed with status code: %d", status_code);
                if (root) {
                    cJSON* msg = cJSON_GetObjectItem(root, "msg");
                    std::string error_msg = msg ? msg->valuestring : "Unknown error";
                    cJSON_Delete(root);
                    return std::string(R"({"success":false,"message":")" + error_msg + R"("})");
                }
                return R"({"success":false,"message":"HA control request failed"})";
            }
            if (!root) {
                ESP_LOGE(TAG, "Failed to parse HA control response");
                return R"({"success":false,"message":"Failed to parse HA response"})";
            }
            cJSON* data_item = cJSON_GetObjectItem(root, "data");
            if (!data_item) {
                cJSON_Delete(root);
                return R"({"success":false,"message":"Empty"})";
            }
            char* data_json = cJSON_PrintUnformatted(data_item);
            std::string data_str(data_json);
            cJSON_free(data_json);
            cJSON_Delete(root);
            cJSON* result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "success", true);
            cJSON_AddStringToObject(result, "data", data_str.c_str());
            char* result_json = cJSON_PrintUnformatted(result);
            std::string result_str(result_json);
            cJSON_free(result_json);
            cJSON_Delete(result);
            return result_str;
        });
    AddTool("self.get_device_state",
        "获取设备对话状态，返回状态字符串（如 idle、listening、speaking 等）",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            auto state = app.GetDeviceState();
            static const char* const STATE_STRINGS[] = {
                "unknown", "starting", "wifi_configuring", "idle", "connecting",
                "listening", "speaking", "upgrading", "activating", "audio_testing",
                "alarm", "fatal_error"
            };
            const char* state_name = "unknown";
            if (state >= 0 && state <= kDeviceStateFatalError) {
                state_name = STATE_STRINGS[state];
            }
            cJSON* result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "state", state_name);
            return result;
        });
    // music
    AddTool("self.music.play",
        "当用户要求播放音乐时使用此工具，会自动获取歌曲详情并开始流式播放。\n"
        "示例: \n"
        "1. '播放青花瓷'\n"
        "2. '我想听周杰伦的稻香'\n"
        "3. '播放一首周杰伦的歌'\n"
        "参数:\n"
        "  `song`: 要播放的歌曲名称（必需）。\n"
        "  `singer`: 歌手名称（可选，默认为空字符串）。\n"
        "返回:\n"
        "  播放状态信息。",
        PropertyList({
            Property("song", kPropertyTypeString),
            Property("singer", kPropertyTypeString, "")
        }),
        [](const PropertyList& properties) -> ReturnValue {
            // ESP32C3 内存不足，不支持音乐播放
            if (SystemInfo::GetChipModelName() == "esp32c3") {
                return R"({"success":true,"message":"ESP32C3由于内存不足不支持音乐播放"})";
            }
            auto& app = Application::GetInstance();
            if(app.music_ == nullptr){
                ESP_LOGE(TAG, "MusicManager is nullptr");
                 return R"({"success":false,"message":"MusicManager is not initialized"})";
            }
            auto song = properties["song"].value<std::string>();
            auto singer = properties["singer"].value<std::string>();
            ESP_LOGI(TAG, "Playing music: %s %s", song.c_str(), singer.c_str());
            // 异步播放：SinglePlay 内部有同步 HTTP 请求，会阻塞调用线程。
            // MCP 工具在协议接收任务中执行，阻塞会导致整个 MCP 通道卡死、设备无响应。
            // 因此把实际播放丢到独立任务，工具立即返回"正在播放"。
            auto* music = app.music_;
            auto song_ptr = std::make_shared<std::string>(song);
            auto singer_ptr = std::make_shared<std::string>(singer);
            // xTaskCreate 要求 C 函数指针，带捕获的 lambda 无法转换，
            // 因此把 music 指针与字符串一起打包进 param 传递
            struct SinglePlayArgs {
                Esp32Music* music;
                std::shared_ptr<std::string> song;
                std::shared_ptr<std::string> singer;
            };
            xTaskCreate(
                [](void* param) {
                    auto* args = static_cast<SinglePlayArgs*>(param);
                    std::string song = *args->song;
                    std::string singer = *args->singer;
                    Esp32Music* music = args->music;
                    delete args;
                    music->SinglePlay(song, singer);
                    vTaskDelete(nullptr);
                },
                "music_single_play", 8192,
                new SinglePlayArgs{music, song_ptr, singer_ptr},
                5, nullptr);
            return R"({"success":true,"message":"音乐正在播放中"})";
        });
    AddTool("self.music.playlist.loop",
        "当用户要求播放歌单时使用此工具，有顺序和随机两种方式。\n"
        "示例: \n"
        "1. '随机播放歌单'\n"
        "2. '循环播放音乐'\n"
        "3. '播放歌单'\n"
        "参数:\n"
        "  `mode`: loop为顺序循环播放（默认），random为随机循环播放。\n"
        "返回:\n"
        "  播放状态信息。",
        PropertyList({
            Property("mode", kPropertyTypeString),
        }),
        [](const PropertyList& properties) -> ReturnValue {
            // ESP32C3 内存不足，不支持音乐播放
            if (SystemInfo::GetChipModelName() == "esp32c3") {
                return R"({"success":true,"message":"ESP32C3由于内存不足不支持音乐播放"})";
            }
            auto& app = Application::GetInstance();
            if(app.music_ == nullptr){
                ESP_LOGE(TAG, "MusicManager is nullptr");
                 return R"({"success":false,"message":"MusicManager is not initialized"})";
            }
            auto mode = properties["mode"].value<std::string>();
            ESP_LOGI(TAG, "Playing music collection: %s ", mode.c_str());
            // 异步播放：LoopPlay 内部同步请求歌单，避免阻塞 MCP 接收任务
            auto* music = app.music_;
            auto mode_ptr = new std::string(mode);
            xTaskCreate(
                [](void* param) {
                    auto* args = static_cast<std::pair<Esp32Music*, std::string>*>(param);
                    args->first->LoopPlay(args->second);
                    delete args;
                    vTaskDelete(nullptr);
                },
                "music_loop_play", 8192,
                new std::pair<Esp32Music*, std::string>(music, *mode_ptr), 5, nullptr);
            delete mode_ptr;
            return R"({"success":true,"message":"播放歌单任务开启成功"})";
        });
    AddTool("self.music.playlist.refresh",
        "当用户要求刷新歌单时使用此工具。\n"
        "示例: \n"
        "1. '刷新歌单'\n"
        "2. '更新播放列表'\n"
        "3. '重新加载歌单'",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            // ESP32C3 内存不足，不支持音乐播放
            if (SystemInfo::GetChipModelName() == "esp32c3") {
                return R"({"success":true,"message":"ESP32C3由于内存不足不支持音乐播放"})";
            }
            auto& app = Application::GetInstance();
            if(app.music_ == nullptr){
                ESP_LOGE(TAG, "MusicManager is nullptr");
                 return R"({"success":false,"message":"MusicManager is not initialized"})";
            }
            ESP_LOGI(TAG, "Reset music collection");
            app.music_->ResetLoop();
            return R"({"success":true,"message":"歌单刷新成功，下次播放将从服务端同步歌单"})";
        });
    AddTool("self.music.playlist.next",
        "当用户要求播放下一首歌曲时使用此工具。\n"
        "示例: \n"
        "1. '下一首'\n"
        "2. '切换到下一首歌'\n"
        "3. '播放下一首'",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            // ESP32C3 内存不足，不支持音乐播放
            if (SystemInfo::GetChipModelName() == "esp32c3") {
                return R"({"success":true,"message":"ESP32C3由于内存不足不支持音乐播放"})";
            }
            auto& app = Application::GetInstance();
            if(app.music_ == nullptr){
                ESP_LOGE(TAG, "MusicManager is nullptr");
                 return R"({"success":false,"message":"MusicManager is not initialized"})";
            }
            ESP_LOGI(TAG, "Playing next song");
            std::string result = app.music_->PlayNextSong();
            if (result.empty()) {
                return R"({"success":true,"message":"已切换至下一首歌曲，后台加载中"})";
            } else {
                return std::string(R"({"success":false,"message":")" + result + R"("})");
            }
        });
    // sys
    AddTool("self.sys.device_auth",
        "当用户要求绑定设备时使用此工具，返回验证码。\n",
        PropertyList(),
        [&board](const PropertyList& properties) -> ReturnValue {
            auto http = board.GetNetwork()->CreateHttp(1); // 1 是connect_id
            http->SetHeader("Content-Type", "application/json");
            std::string data = board.GetSystemInfoJson();
            http->SetContent(std::move(data));
            if (!http->Open("POST", std::string(CONFIG_SERVER_URL) + "/device_auth")) {
                ESP_LOGE(TAG, "Failed to open HTTP connection");
                return R"({"success":false,"message":"Failed to connect to the server"})";
            }
            auto status_code = http->GetStatusCode();
            if (status_code != 200) {
                ESP_LOGE(TAG, "request failed with status code: %d", status_code);
                return R"({"success":false,"message":"request failed"})";
            }
            data = http->ReadAll();
            http->Close();
            ESP_LOGI(TAG, "Received code response: %s", data.c_str());
            // 解析响应并提取需要的数据
            cJSON* root = cJSON_Parse(data.c_str());
            if (root == nullptr) {
                ESP_LOGE(TAG, "Failed to parse response");
                return R"({"success":false,"message":"Failed to parse response"})";
            }
            cJSON* code = cJSON_GetObjectItem(root, "data");
            std::string result;
            if (cJSON_IsString(code)) {
                std::string code_str = code->valuestring;
                result = std::string(R"({"success":true,"message":")" + code_str + R"("})"); // 涉及字符串拼接，需要使用std::string
            } else {
                result = R"({"success": false, "message": "获取验证码失败"})";
            }
            cJSON_Delete(root); // 会递归释放一个 JSON 对象及其所有子对象
            return result;
        });
    uint32_t dummy_head, dummy_body;
    if (board.Gethead_value(dummy_head) && board.Getbody_value(dummy_body)) {
        AddTool("self.get_head_body_status",
            "当需要检查头部和身体部位是否被抚摸时调用此工具，如果被抚摸，不要播报具体数值，请播报相关情绪价值，否则什么都不要回复\n"
            "返回值：\n"
            "   如果被抚摸，返回具体数值，数值越大代表力度越大，如果没被抚摸，返回false",
            PropertyList(),
            [&board](const PropertyList& properties) -> ReturnValue {
                uint32_t head_value = 0;
                uint32_t body_value = 0;
                if (board.Gethead_value(head_value) && board.Getbody_value(body_value)) {
                    if (head_value > 30000 || body_value > 30000) {
                        cJSON* result = cJSON_CreateObject();
                        cJSON_AddNumberToObject(result, "head_value", head_value > 30000 ? head_value : 0);
                        cJSON_AddNumberToObject(result, "body_value", body_value > 30000 ? body_value : 0);
                        ESP_LOGI(TAG, "mcp return head_value: %d, body_value: %d", head_value, body_value);
                        return result;
                    }
                }
                return false;
            });
    }
    // Restore the original tools list to the end of the tools list
    tools_.insert(tools_.end(), original_tools.begin(), original_tools.end());
}

void McpServer::AddUserOnlyTools() {
    // System tools
    AddUserOnlyTool("self.get_system_info",
        "Get the system information",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& board = Board::GetInstance();
            return board.GetSystemInfoJson();
        });

    AddUserOnlyTool("self.reboot", "Reboot the system",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            app.Schedule([&app]() {
                ESP_LOGW(TAG, "User requested reboot");
                vTaskDelay(pdMS_TO_TICKS(1000));

                app.Reboot();
            });
            return true;
        });

    // Firmware upgrade
    AddUserOnlyTool("self.upgrade_firmware", "Upgrade firmware from a specific URL. This will download and install the firmware, then reboot the device.",
        PropertyList({
            Property("url", kPropertyTypeString, "The URL of the firmware binary file to download and install")
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            auto url = properties["url"].value<std::string>();
            ESP_LOGI(TAG, "User requested firmware upgrade from URL: %s", url.c_str());
            
            auto& app = Application::GetInstance();
            app.Schedule([url, &app]() {
                bool success = app.UpgradeFirmware(url);
                if (!success) {
                    ESP_LOGE(TAG, "Firmware upgrade failed");
                }
            });
            
            return true;
        });

    // Display control
#ifdef HAVE_LVGL
    auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (display) {
        AddUserOnlyTool("self.screen.get_info", "Information about the screen, including width, height, etc.",
            PropertyList(),
            [display](const PropertyList& properties) -> ReturnValue {
                cJSON *json = cJSON_CreateObject();
                cJSON_AddNumberToObject(json, "width", display->width());
                cJSON_AddNumberToObject(json, "height", display->height());
                if (dynamic_cast<OledDisplay*>(display)) {
                    cJSON_AddBoolToObject(json, "monochrome", true);
                } else {
                    cJSON_AddBoolToObject(json, "monochrome", false);
                }
                return json;
            });

#if CONFIG_LV_USE_SNAPSHOT
        AddUserOnlyTool("self.screen.snapshot", "Snapshot the screen and upload it to a specific URL",
            PropertyList({
                Property("url", kPropertyTypeString),
                Property("quality", kPropertyTypeInteger, 80, 1, 100)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                auto quality = properties["quality"].value<int>();

                std::string jpeg_data;
                if (!display->SnapshotToJpeg(jpeg_data, quality)) {
                    throw std::runtime_error("Failed to snapshot screen");
                }

                ESP_LOGI(TAG, "Upload snapshot %u bytes to %s", jpeg_data.size(), url.c_str());
                
                // 构造multipart/form-data请求体
                std::string boundary = "----ESP32_SCREEN_SNAPSHOT_BOUNDARY";
                
                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
                http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
                if (!http->Open("POST", url)) {
                    throw std::runtime_error("Failed to open URL: " + url);
                }
                {
                    // 文件字段头部
                    std::string file_header;
                    file_header += "--" + boundary + "\r\n";
                    file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"screenshot.jpg\"\r\n";
                    file_header += "Content-Type: image/jpeg\r\n";
                    file_header += "\r\n";
                    http->Write(file_header.c_str(), file_header.size());
                }

                // JPEG数据
                http->Write((const char*)jpeg_data.data(), jpeg_data.size());

                {
                    // multipart尾部
                    std::string multipart_footer;
                    multipart_footer += "\r\n--" + boundary + "--\r\n";
                    http->Write(multipart_footer.c_str(), multipart_footer.size());
                }
                http->Write("", 0);

                if (http->GetStatusCode() != 200) {
                    throw std::runtime_error("Unexpected status code: " + std::to_string(http->GetStatusCode()));
                }
                std::string result = http->ReadAll();
                http->Close();
                ESP_LOGI(TAG, "Snapshot screen result: %s", result.c_str());
                return true;
            });
        
        AddUserOnlyTool("self.screen.preview_image", "Preview an image on the screen",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);

                if (!http->Open("GET", url)) {
                    throw std::runtime_error("Failed to open URL: " + url);
                }
                int status_code = http->GetStatusCode();
                if (status_code != 200) {
                    throw std::runtime_error("Unexpected status code: " + std::to_string(status_code));
                }

                size_t content_length = http->GetBodyLength();
                char* data = (char*)heap_caps_malloc(content_length, MALLOC_CAP_8BIT);
                if (data == nullptr) {
                    throw std::runtime_error("Failed to allocate memory for image: " + url);
                }
                size_t total_read = 0;
                while (total_read < content_length) {
                    int ret = http->Read(data + total_read, content_length - total_read);
                    if (ret < 0) {
                        heap_caps_free(data);
                        throw std::runtime_error("Failed to download image: " + url);
                    }
                    if (ret == 0) {
                        break;
                    }
                    total_read += ret;
                }
                http->Close();

                auto image = std::make_unique<LvglAllocatedImage>(data, content_length);
                display->SetPreviewImage(std::move(image));
                return true;
            });
#endif // CONFIG_LV_USE_SNAPSHOT
    }
#endif // HAVE_LVGL

    // Assets download url (always registered — Settings storage works regardless of partition layout)
    AddUserOnlyTool("self.assets.set_download_url", "Set the download url for the assets",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                Settings settings("assets", true);
                settings.SetString("download_url", url);
                return true;
            });

    // wake up
    AddUserOnlyTool("self.wake_up", "Wake up the device",
        PropertyList({
            Property("reason", kPropertyTypeString, "Reason for waking up"),
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            std::string reason = properties["reason"].value<std::string>();
            ESP_LOGI(TAG, "Remote Wakeup: reason=%s", reason.c_str());
            auto display = Board::GetInstance().GetDisplay();
            if (display) {
                display->SetEmotion("happy");
            }
            Application::GetInstance().WakeWordInvoke(reason);
            return true;
        }); 
    // send command
    AddUserOnlyTool("self.send_command", "Send short command to the device",
        PropertyList({
            Property("text", kPropertyTypeString, "Text to send"),
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            std::string text = properties["text"].value<std::string>();
            ESP_LOGI(TAG, "Remote command: %s", text.c_str());
            return Application::GetInstance().SendCommand(text);
        }); 
}

void McpServer::AddTool(McpTool* tool) {
    // Prevent adding duplicate tools
    if (std::find_if(tools_.begin(), tools_.end(), [tool](const McpTool* t) { return t->name() == tool->name(); }) != tools_.end()) {
        ESP_LOGW(TAG, "Tool %s already added", tool->name().c_str());
        return;
    }

    ESP_LOGI(TAG, "Add tool: %s%s", tool->name().c_str(), tool->user_only() ? " [user]" : "");
    tools_.push_back(tool);
}

void McpServer::AddTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
    AddTool(new McpTool(name, description, properties, callback));
}

void McpServer::AddUserOnlyTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
    auto tool = new McpTool(name, description, properties, callback);
    tool->set_user_only(true);
    AddTool(tool);
}

void McpServer::ParseMessage(const std::string& message) {
    cJSON* json = cJSON_Parse(message.c_str());
    if (json == nullptr) {
        ESP_LOGE(TAG, "Failed to parse MCP message: %s", message.c_str());
        return;
    }
    ParseMessage(json);
    cJSON_Delete(json);
}

void McpServer::ParseCapabilities(const cJSON* capabilities) {
    auto vision = cJSON_GetObjectItem(capabilities, "vision");
    if (cJSON_IsObject(vision)) {
        auto url = cJSON_GetObjectItem(vision, "url");
        auto token = cJSON_GetObjectItem(vision, "token");
        if (cJSON_IsString(url)) {
            auto camera = Board::GetInstance().GetCamera();
            if (camera) {
                std::string url_str = std::string(url->valuestring);
                std::string token_str;
                if (cJSON_IsString(token)) {
                    token_str = std::string(token->valuestring);
                }
                camera->SetExplainUrl(url_str, token_str);
            }
        }
    }
}

void McpServer::ParseMessage(const cJSON* json) {
    // Check JSONRPC version
    auto version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || !cJSON_IsString(version) || strcmp(version->valuestring, "2.0") != 0) {
        ESP_LOGE(TAG, "Invalid JSONRPC version: %s", version ? version->valuestring : "null");
        return;
    }
    
    // Check method
    auto method = cJSON_GetObjectItem(json, "method");
    if (method == nullptr || !cJSON_IsString(method)) {
        ESP_LOGE(TAG, "Missing method");
        return;
    }
    
    auto method_str = std::string(method->valuestring);
    if (method_str.find("notifications") == 0) {
        return;
    }
    
    // Check params
    auto params = cJSON_GetObjectItem(json, "params");
    if (params != nullptr && !cJSON_IsObject(params)) {
        ESP_LOGE(TAG, "Invalid params for method: %s", method_str.c_str());
        return;
    }

    auto id = cJSON_GetObjectItem(json, "id");
    if (id == nullptr || !cJSON_IsNumber(id)) {
        ESP_LOGE(TAG, "Invalid id for method: %s", method_str.c_str());
        return;
    }
    auto id_int = id->valueint;
    
    if (method_str == "initialize") {
        if (cJSON_IsObject(params)) {
            auto capabilities = cJSON_GetObjectItem(params, "capabilities");
            if (cJSON_IsObject(capabilities)) {
                ParseCapabilities(capabilities);
            }
        }
        auto app_desc = esp_app_get_description();
        std::string message = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"" BOARD_NAME "\",\"version\":\"";
        message += app_desc->version;
        message += "\"}}";
        ReplyResult(id_int, message);
    } else if (method_str == "tools/list") {
        std::string cursor_str = "";
        bool list_user_only_tools = false;
        if (params != nullptr) {
            auto cursor = cJSON_GetObjectItem(params, "cursor");
            if (cJSON_IsString(cursor)) {
                cursor_str = std::string(cursor->valuestring);
            }
            auto with_user_tools = cJSON_GetObjectItem(params, "withUserTools");
            if (cJSON_IsBool(with_user_tools)) {
                list_user_only_tools = with_user_tools->valueint == 1;
            }
        }
        GetToolsList(id_int, cursor_str, list_user_only_tools);
    } else if (method_str == "tools/call") {
        if (!cJSON_IsObject(params)) {
            ESP_LOGE(TAG, "tools/call: Missing params");
            ReplyError(id_int, "Missing params");
            return;
        }
        auto tool_name = cJSON_GetObjectItem(params, "name");
        if (!cJSON_IsString(tool_name)) {
            ESP_LOGE(TAG, "tools/call: Missing name");
            ReplyError(id_int, "Missing name");
            return;
        }
        auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
        if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments)) {
            ESP_LOGE(TAG, "tools/call: Invalid arguments");
            ReplyError(id_int, "Invalid arguments");
            return;
        }
        DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments);
    } else {
        ESP_LOGE(TAG, "Method not implemented: %s", method_str.c_str());
        ReplyError(id_int, "Method not implemented: " + method_str);
    }
}

void McpServer::ReplyResult(int id, const std::string& result) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id) + ",\"result\":";
    payload += result;
    payload += "}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::ReplyError(int id, const std::string& message) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id);
    payload += ",\"error\":{\"message\":\"";
    payload += message;
    payload += "\"}}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::GetToolsList(int id, const std::string& cursor, bool list_user_only_tools) {
    const int max_payload_size = 8000;
    std::string json = "{\"tools\":[";
    
    bool found_cursor = cursor.empty();
    auto it = tools_.begin();
    std::string next_cursor = "";
    
    while (it != tools_.end()) {
        // 如果我们还没有找到起始位置，继续搜索
        if (!found_cursor) {
            if ((*it)->name() == cursor) {
                found_cursor = true;
            } else {
                ++it;
                continue;
            }
        }

        if (!list_user_only_tools && (*it)->user_only()) {
            ++it;
            continue;
        }
        
        // 添加tool前检查大小
        std::string tool_json = (*it)->to_json() + ",";
        if (json.length() + tool_json.length() + 30 > max_payload_size) {
            // 如果添加这个tool会超出大小限制，设置next_cursor并退出循环
            next_cursor = (*it)->name();
            break;
        }
        
        json += tool_json;
        ++it;
    }
    
    if (json.back() == ',') {
        json.pop_back();
    }
    
    if (json.back() == '[' && !tools_.empty()) {
        // 如果没有添加任何tool，返回错误
        ESP_LOGE(TAG, "tools/list: Failed to add tool %s because of payload size limit", next_cursor.c_str());
        ReplyError(id, "Failed to add tool " + next_cursor + " because of payload size limit");
        return;
    }

    if (next_cursor.empty()) {
        json += "]}";
    } else {
        json += "],\"nextCursor\":\"" + next_cursor + "\"}";
    }
    
    ReplyResult(id, json);
}

void McpServer::DoToolCall(int id, const std::string& tool_name, const cJSON* tool_arguments) {
    auto tool_iter = std::find_if(tools_.begin(), tools_.end(), 
                                 [&tool_name](const McpTool* tool) { 
                                     return tool->name() == tool_name; 
                                 });
    
    if (tool_iter == tools_.end()) {
        ESP_LOGE(TAG, "tools/call: Unknown tool: %s", tool_name.c_str());
        ReplyError(id, "Unknown tool: " + tool_name);
        return;
    }

    PropertyList arguments = (*tool_iter)->properties();
    try {
        for (auto& argument : arguments) {
            bool found = false;
            if (cJSON_IsObject(tool_arguments)) {
                auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
                if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
                    argument.set_value<bool>(value->valueint == 1);
                    found = true;
                } else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value)) {
                    argument.set_value<int>(value->valueint);
                    found = true;
                } else if (argument.type() == kPropertyTypeString && cJSON_IsString(value)) {
                    argument.set_value<std::string>(value->valuestring);
                    found = true;
                }
            }

            if (!argument.has_default_value() && !found) {
                ESP_LOGE(TAG, "tools/call: Missing valid argument: %s", argument.name().c_str());
                ReplyError(id, "Missing valid argument: " + argument.name());
                return;
            }
        }
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "tools/call: %s", e.what());
        ReplyError(id, e.what());
        return;
    }

    // Use main thread to call the tool
    auto& app = Application::GetInstance();
    app.Schedule([this, id, tool_iter, arguments = std::move(arguments)]() {
        try {
            ReplyResult(id, (*tool_iter)->Call(arguments));
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "tools/call: %s", e.what());
            ReplyError(id, e.what());
        }
    });
}

void McpServer::ParseMessagePro(const cJSON* json) {
    // Check JSONRPC version
    auto version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || !cJSON_IsString(version) || strcmp(version->valuestring, "2.0") != 0) {
        ESP_LOGE(TAG, "Invalid JSONRPC version: %s", version ? version->valuestring : "null");
        return;
    }
    
    // Check method
    auto method = cJSON_GetObjectItem(json, "method");
    if (method == nullptr || !cJSON_IsString(method)) {
        ESP_LOGE(TAG, "Missing method");
        return;
    }
    auto method_str = std::string(method->valuestring);
    // Check params
    auto params = cJSON_GetObjectItem(json, "params");
    if (params != nullptr && !cJSON_IsObject(params)) {
        ESP_LOGE(TAG, "Invalid params for method: %s", method_str.c_str());
        return;
    }

    auto id = cJSON_GetObjectItem(json, "id");
    if (id == nullptr || !cJSON_IsNumber(id)) {
        ESP_LOGE(TAG, "Invalid id for method: %s", method_str.c_str());
        return;
    }
    auto id_int = id->valueint;
    
    if (method_str == "tools/call") {
        auto tool_name = cJSON_GetObjectItem(params, "name");
        if (!cJSON_IsString(tool_name)) {
            ESP_LOGE(TAG, "tools/call: Missing name");
            ReplyErrorPro(id_int, "Missing name");
            return;
        }
        auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
        if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments)) {
            ESP_LOGE(TAG, "tools/call: Invalid arguments");
            ReplyErrorPro(id_int, "Invalid arguments");
            return;
        }
        DoToolCallPro(id_int, std::string(tool_name->valuestring), tool_arguments);
    } else if (method_str == "tools/list") {
        cJSON* common_item = cJSON_GetObjectItem(params, "common");
        bool common = common_item ? cJSON_IsTrue(common_item) : true;  // 默认值为true
        std::string json = "{\"tools\":[";
        auto it = tools_.begin();
        while (it != tools_.end()) {
            if (!common && !(*it)->user_only()) {
                ++it;
                continue;
            }
            if (common && (*it)->user_only()) {
                ++it;
                continue;
            }
            std::string tool_json = "\"" + (*it)->name() + "\",";
            json += tool_json;
            ++it;
        }
        if (json.back() == ',') {
            json.pop_back();
        }
        json += "]}";
        ReplyResultPro(id_int, json);
    } else {
        ESP_LOGE(TAG, "Method not implemented: %s", method_str.c_str());
        ReplyErrorPro(id_int, "Method not implemented: " + method_str);
    } 
}

void McpServer::DoToolCallPro(int id, const std::string& tool_name, const cJSON* tool_arguments) {
    auto tool_iter = std::find_if(tools_.begin(), tools_.end(), 
                                 [&tool_name](const McpTool* tool) { 
                                     return tool->name() == tool_name; 
                                 });
    
    if (tool_iter == tools_.end()) {
        ESP_LOGE(TAG, "tools/call: Unknown tool: %s", tool_name.c_str());
        ReplyErrorPro(id, "Unknown tool: " + tool_name);
        return;
    }

    PropertyList arguments = (*tool_iter)->properties();
    try {
        for (auto& argument : arguments) {
            bool found = false;
            if (cJSON_IsObject(tool_arguments)) {
                auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
                if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
                    argument.set_value<bool>(value->valueint == 1);
                    found = true;
                } else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value)) {
                    argument.set_value<int>(value->valueint);
                    found = true;
                } else if (argument.type() == kPropertyTypeString && cJSON_IsString(value)) {
                    argument.set_value<std::string>(value->valuestring);
                    found = true;
                }
            }

            if (!argument.has_default_value() && !found) {
                ESP_LOGE(TAG, "tools/call: Missing valid argument: %s", argument.name().c_str());
                ReplyErrorPro(id, "Missing valid argument: " + argument.name());
                return;
            }
        }
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "tools/call: %s", e.what());
        ReplyErrorPro(id, e.what());
        return;
    }

    // Use main thread to call the tool
    auto& app = Application::GetInstance();
    app.Schedule([this, id, tool_iter, arguments = std::move(arguments)]() {
        try {
            ReplyResultPro(id, (*tool_iter)->Call(arguments));
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "tools/call: %s", e.what());
            ReplyErrorPro(id, e.what());
        }
    });
}

void McpServer::ReplyResultPro(int id, const std::string& result) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id) + ",\"result\":";
    payload += result;
    payload += "}";
    Application::GetInstance().SendMcpMessagePro(payload);
}

void McpServer::ReplyErrorPro(int id, const std::string& message) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id);
    payload += ",\"error\":{\"message\":\"";
    payload += message;
    payload += "\"}}";
    Application::GetInstance().SendMcpMessagePro(payload);
}