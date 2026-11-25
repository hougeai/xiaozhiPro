/*
 * MCP Server Implementation
 * Reference: https://modelcontextprotocol.io/specification/2024-11-05
 */

#include "mcp_server.h"
#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_pthread.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <iterator>
#include <optional>

#include "application.h"
#include "board.h"
#include "alarm.h"
#include "lvgl_display.h"
#include "system_info.h"
#include "led/gpio_led.h" 
#include "display.h"
#include "lvgl_image.h"
#include "lvgl_theme.h"
#include "settings.h"

#define TAG "MCP"

McpServer::McpServer() {}

McpServer::~McpServer() = default;

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
            "Provides the real-time information of the device, including the current status of the "
            "audio speaker, screen, battery, network, etc.\n"
            "Use this tool for: \n"
            "1. Answering questions about current condition (e.g. what is the current volume of "
            "the audio speaker?)\n"
            "2. As the first step to control the device (e.g. turn up / down the volume of the "
            "audio speaker, etc.)",
            PropertyList(), [&board](const PropertyList& properties) -> ReturnValue {
                return board.GetDeviceStatusJson();
            });

    AddTool("self.audio_speaker.set_volume",
            "Set the volume of the audio speaker. If the current volume is unknown, you must call "
            "`self.get_device_status` tool first and then call this tool.",
            PropertyList({Property("volume", kPropertyTypeInteger, 0, 100)}),
            [&board](const PropertyList& properties) -> ReturnValue {
                auto codec = board.GetAudioCodec();
                codec->SetOutputVolume(properties["volume"].value<int>());
                return true;
            });

    auto backlight = board.GetBacklight();
    if (backlight) {
        AddTool("self.screen.set_brightness", "Set the brightness of the screen.",
                PropertyList({Property("brightness", kPropertyTypeInteger, 0, 100)}),
                [backlight](const PropertyList& properties) -> ReturnValue {
                    uint8_t brightness =
                        static_cast<uint8_t>(properties["brightness"].value<int>());
                    backlight->SetBrightness(brightness, true);
                    return true;
                });
    }

#ifdef HAVE_LVGL
    auto display = board.GetDisplay();
    if (display && display->GetTheme() != nullptr) {
        AddTool("self.screen.set_theme",
                "Set the theme of the screen. The theme can be `light` or `dark`.",
                PropertyList({Property("theme", kPropertyTypeString)}),
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
                "Always remember you have a camera. If the user asks you to see something, use "
                "this tool to take a photo and then explain it.\n"
                "Args:\n"
                "  `question`: The question that you want to ask about the photo.\n"
                "Return:\n"
                "  A JSON object that provides the photo information.",
                PropertyList({Property("question", kPropertyTypeString)}),
                [camera](const PropertyList& properties) -> ToolResult {
                    // Lower the priority to do the camera capture
                    TaskPriorityReset priority_reset(1);

                    if (!camera->Capture()) {
                        return std::unexpected("Failed to capture photo");
                    }
                    auto question = properties["question"].value<std::string>();
                    auto result = camera->Explain(question);
                    if (!result) {
                        return std::unexpected(std::move(result.error()));
                    }
                    return std::move(*result);
                });
    }
#endif
    
    // led
    // GpioLed is not compiled for CONFIG_IDF_TARGET_ESP32 (gpio_led.cc is
    // removed from SOURCES in CMakeLists.txt), so guard the reference to avoid
    // an undefined-reference link error on ESP32 builds.
#if !defined(CONFIG_IDF_TARGET_ESP32)
    auto led = board.GetManualLed();
    // 项目禁用 RTTI（-fno-rtti），不能用 dynamic_cast；GetManualLed 的所有实现均返回
    // GpioLed（或基类默认的 nullptr），static_cast 即可，下方 if (gpio_led) 已兜底空指针。
    auto gpio_led = static_cast<GpioLed*>(led);
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
#endif  // !defined(CONFIG_IDF_TARGET_ESP32)
    // knowledge base
    AddTool("self.kb.query",
        "知识库检索。用户问题超出认知、不可胡编乱造时调用。异步：立即返回'检索中'并告知用户正在检索，完成后会收到'知识库报告'。参数query为原始问题。",
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
            int status_code = http->GetStatusCode().value_or(0);
            std::string response = http->ReadAll();
            // ESP_LOGI(TAG, "kb.query response: %s", response.c_str());
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
        "仅在收到'知识库报告'时调用，检索最新消息，否则不要调用。",
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
            int status_code = http->GetStatusCode().value_or(0);
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
        "搜索用户个人记忆。当用户提'之前/上次/以前'、具体人名地点等专有名词、要求继续之前讨论、或需结合个人偏好习惯时调用。参数query为检索词，多词用空格分隔。",
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
            int status_code = http->GetStatusCode().value_or(0);
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
        "视觉分析。用户要求看东西或开摄像头时调用，参数question为要问的问题。",
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
            int status_code = http->GetStatusCode().value_or(0);
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
    // agent 统一通道
    AddTool("self.agent.send",
        "给指定的本地Agent发送任务消息。当用户想让某个Agent执行任务时调用，例如：让OpenClaw发飞书消息、让Hermes整理AI资讯、让Claude Code总结代码。"
        "参数agent_id必须传以下英文标识之一，禁止传中文或其他内容：openclaw、hermes、claude、codex、qwen、opencode、qoder、minimax、kimi、codebuddy、deepseek、pi。"
        "message为要发送的消息内容。",
        PropertyList({
            Property("agent_id", kPropertyTypeString),
            Property("message", kPropertyTypeString)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto http = Board::GetInstance().GetNetwork()->CreateHttp(1);
            http->SetHeader("Content-Type", "application/json");
            http->SetHeader("Device-ID", SystemInfo::GetMacAddress().c_str());
            http->SetTimeout(10000); // 设置超时时间
            // 构造请求的JSON数据
            std::string agent_id = properties["agent_id"].value<std::string>();
            std::string message = properties["message"].value<std::string>();
            cJSON* json_data = cJSON_CreateObject();
            cJSON_AddStringToObject(json_data, "message", message.c_str());
            char* json_string = cJSON_PrintUnformatted(json_data);
            std::string data(json_string);
            cJSON_free(json_string);
            cJSON_Delete(json_data);
            http->SetContent(std::move(data));
            std::string base_url = std::string(CONFIG_SERVER_URL) + "/agent_send?agent_id=" + agent_id;

            if (!http->Open("POST", base_url)) {
                ESP_LOGE(TAG, "Failed to open HTTP connection");
                return R"({"success":false,"message":"Failed to connect to agent"})";
            }
            int status_code = http->GetStatusCode().value_or(0);
            std::string response = http->ReadAll();
            cJSON* root = cJSON_Parse(response.c_str());
            http->Close();
            if (status_code != 200) {
                ESP_LOGE(TAG, "Agent request failed with status code: %d", status_code);
                if (root) {
                    cJSON* msg = cJSON_GetObjectItem(root, "msg");
                    std::string error_msg = msg ? msg->valuestring : "Unknown error";
                    ESP_LOGE(TAG, "Agent error message: %s", error_msg.c_str());
                    cJSON_Delete(root);
                    return std::string(R"({"success":false,"message":")" + error_msg + R"("})");
                }
                return R"({"success":false,"message":"Agent request failed"})";
            }

            if (!root) {
                ESP_LOGE(TAG, "Failed to parse Agent response");
                return R"({"success":false,"message":"Failed to parse Agent response"})";
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
    AddTool("self.agent.retrieve",
        "检索指定Agent的最新回复。当收到Agent报告类唤醒（如'openclaw报告'、'hermes报告'等），或用户询问某个Agent的执行结果时调用。"
        "参数agent_id必须传以下英文标识之一，禁止传中文或其他内容：openclaw、hermes、claude、codex、qwen、opencode、qoder、minimax、kimi、codebuddy、deepseek、pi。",
        PropertyList({
            Property("agent_id", kPropertyTypeString)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto http = Board::GetInstance().GetNetwork()->CreateHttp(1);
            http->SetHeader("Content-Type", "application/json");
            http->SetHeader("Device-ID", SystemInfo::GetMacAddress().c_str());
            http->SetTimeout(10000); // 设置超时时间
            std::string agent_id = properties["agent_id"].value<std::string>();
            std::string base_url = std::string(CONFIG_SERVER_URL) + "/agent_retrieve?agent_id=" + agent_id;

            if (!http->Open("GET", base_url)) {
                ESP_LOGE(TAG, "Failed to open HTTP connection");
                return R"({"success":false,"message":"Failed to connect to agent"})";
            }
            int status_code = http->GetStatusCode().value_or(0);
            std::string response = http->ReadAll();
            cJSON* root = cJSON_Parse(response.c_str());
            http->Close();
            if (status_code != 200) {
                ESP_LOGE(TAG, "Agent request failed with status code: %d", status_code);
                if (root) {
                    cJSON* msg = cJSON_GetObjectItem(root, "msg");
                    std::string error_msg = msg ? msg->valuestring : "Unknown error";
                    ESP_LOGE(TAG, "Agent error message: %s", error_msg.c_str());
                    cJSON_Delete(root);
                    return std::string(R"({"success":false,"message":")" + error_msg + R"("})");
                }
                return R"({"success":false,"message":"Agent request failed"})";
            }

            if (!root) {
                ESP_LOGE(TAG, "Failed to parse Agent response");
                return R"({"success":false,"message":"Failed to parse Agent response"})";
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
        "获取Home Assistant(HA)传感器列表，返回按设备分组的 entity_id、name、state、unit。",
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
            int status_code = http->GetStatusCode().value_or(0);
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
        "获取HA可控设备列表，返回 entity_id、name、state、domain、services，用于后续 self.ha.control。",
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
            int status_code = http->GetStatusCode().value_or(0);
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
        "控制HA设备。参数entity_id为设备实体，service为服务(turn_on等)，fields为额外参数JSON。",
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
            int status_code = http->GetStatusCode().value_or(0);
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
    // alarm+music (disabled on ESP32-C3: insufficient RAM)
#if !defined(CONFIG_IDF_TARGET_ESP32C3)
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
    // music
    AddTool("self.music.list",
        "查看上传的音乐列表时使用此工具，返回该用户已上传的歌曲清单。\n"
        "返回:\n"
        "  用户上传的音乐列表（每行一首，格式为 歌手-歌名）。",
        PropertyList(),
        [&board](const PropertyList& properties) -> ReturnValue {
            auto http = Board::GetInstance().GetNetwork()->CreateHttp(1);
            http->SetHeader("Content-Type", "application/json");
            http->SetHeader("Device-ID", SystemInfo::GetMacAddress().c_str());
            http->SetTimeout(5000); // 设置超时时间
            std::string base_url = std::string(CONFIG_SERVER_URL) + "/device_music_list";

            if (!http->Open("GET", base_url)) {
                ESP_LOGE(TAG, "Failed to connect to music list API");
                return R"({"success":false,"message":"Failed to connect to the server"})";
            }

            int status_code = http->GetStatusCode().value_or(0);
            std::string response = http->ReadAll();
            cJSON* root = cJSON_Parse(response.c_str());
            http->Close();
            if (status_code != 200 || root == nullptr) {
                std::string msg = "Failed to get music list";
                if (root) {
                    cJSON* m = cJSON_GetObjectItem(root, "msg");
                    if (cJSON_IsString(m)) msg = m->valuestring;
                    cJSON_Delete(root);
                }
                ESP_LOGE(TAG, "Get music list failed: %s", msg.c_str());
                return std::string(R"({"success":false,"message":")") + msg + R"("})";
            }

            cJSON* data_item = cJSON_GetObjectItem(root, "data");
            if (!data_item || !cJSON_IsString(data_item)) {
                cJSON_Delete(root);
                return R"({"success":false,"message":"Empty"})";
            }
            std::string data_str = data_item->valuestring;
            cJSON_Delete(root);
            // 直接把服务端返回的列表字符串透传给调用方
            cJSON* result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "data", data_str.c_str());
            cJSON_AddBoolToObject(result, "success", true);
            char* result_json = cJSON_PrintUnformatted(result);
            std::string result_str(result_json);
            cJSON_free(result_json);
            cJSON_Delete(result);
            return result_str;
        });
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
            // 同步调用 SinglePlay 并回传其返回信息（含未找到歌曲等错误）。
            // SinglePlay 在拿到音频 URL 并成功起播后才返回空串，HTTP 失败/无匹配时会立即返回错误字符串，同步等待代价仅为一次 HTTP 请求（数百毫秒）
            std::string result = app.music_->SinglePlay(song, singer);
            if (result.empty()) {
                return R"({"success":true,"message":"音乐正在播放中"})";
            }
            return std::string("{\"success\":false,\"message\":\"") + result + "\"}";
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
            // 同步调用 LoopPlay 并回传其返回信息（含歌单为空等错误），
            // 否则 LLM 永远收到"成功"而实际播放失败。
            std::string result = app.music_->LoopPlay(mode);
            // LoopPlay 成功时返回空字符串，失败时返回中文错误描述
            if (result.empty()) {
                return R"({"success":true,"message":"播放歌单任务开启成功"})";
            }
            return std::string("{\"success\":false,\"message\":\"") + result + "\"}";
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
#endif // !CONFIG_IDF_TARGET_ESP32C3
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
            int status_code = http->GetStatusCode().value_or(0);
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
    tools_.insert(tools_.end(), std::make_move_iterator(original_tools.begin()),
                  std::make_move_iterator(original_tools.end()));
}

void McpServer::AddUserOnlyTools() {
    // System tools
    AddUserOnlyTool("self.get_system_info", "Get the system information", PropertyList(),
                    [this](const PropertyList& properties) -> ReturnValue {
                        auto& board = Board::GetInstance();
                        return board.GetSystemInfoJson();
                    });

    AddUserOnlyTool("self.reboot", "Reboot the system", PropertyList(),
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
    AddUserOnlyTool(
        "self.upgrade_firmware",
        "Upgrade firmware from a specific URL. This will download and install the firmware, then "
        "reboot the device.",
        PropertyList({Property("url", kPropertyTypeString,
                               "The URL of the firmware binary file to download and install")}),
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
    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr && display->SupportsGuiOperations()) {
        AddUserOnlyTool("self.screen.get_info",
                        "Information about the screen, including width, height, etc.",
                        PropertyList(), [display](const PropertyList& properties) -> ReturnValue {
                            cJSON* json = cJSON_CreateObject();
                            cJSON_AddNumberToObject(json, "width", display->width());
                            cJSON_AddNumberToObject(json, "height", display->height());
                            cJSON_AddBoolToObject(json, "monochrome", display->IsMonochrome());
                            return json;
                        });

#if CONFIG_LV_USE_SNAPSHOT
        AddUserOnlyTool(
            "self.screen.snapshot", "Snapshot the screen and upload it to a specific URL",
            PropertyList({Property("url", kPropertyTypeString),
                          Property("quality", kPropertyTypeInteger, 80, 1, 100)}),
            [display](const PropertyList& properties) -> ToolResult {
                auto url = properties["url"].value<std::string>();
                auto quality = properties["quality"].value<int>();

                std::string jpeg_data;
                if (!display->SnapshotToJpeg(jpeg_data, quality)) {
                    return std::unexpected("Failed to snapshot screen");
                }

                ESP_LOGI(TAG, "Upload snapshot %u bytes to %s", jpeg_data.size(), url.c_str());

                // 构造multipart/form-data请求体
                std::string boundary = "----ESP32_SCREEN_SNAPSHOT_BOUNDARY";

                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
                http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
                if (auto opened = http->Open("POST", url); !opened) {
                    return std::unexpected("Failed to open URL: " + url + " (" +
                                           opened.error().ToString() + ")");
                }
                auto write_or_fail = [&http](const char* data,
                                             size_t size) -> std::optional<std::string> {
                    if (auto written = http->Write(data, size); !written) {
                        return written.error().ToString();
                    }
                    return std::nullopt;
                };
                {
                    // 文件字段头部
                    std::string file_header;
                    file_header += "--" + boundary + "\r\n";
                    file_header +=
                        "Content-Disposition: form-data; name=\"file\"; "
                        "filename=\"screenshot.jpg\"\r\n";
                    file_header += "Content-Type: image/jpeg\r\n";
                    file_header += "\r\n";
                    if (auto error = write_or_fail(file_header.c_str(), file_header.size());
                        error) {
                        http->Close();
                        return std::unexpected("Failed to upload screenshot: " + *error);
                    }
                }

                // JPEG数据
                if (auto error = write_or_fail((const char*)jpeg_data.data(), jpeg_data.size());
                    error) {
                    http->Close();
                    return std::unexpected("Failed to upload screenshot: " + *error);
                }

                {
                    // multipart尾部
                    std::string multipart_footer;
                    multipart_footer += "\r\n--" + boundary + "--\r\n";
                    if (auto error =
                            write_or_fail(multipart_footer.c_str(), multipart_footer.size());
                        error) {
                        http->Close();
                        return std::unexpected("Failed to upload screenshot: " + *error);
                    }
                }
                if (auto error = write_or_fail("", 0); error) {
                    http->Close();
                    return std::unexpected("Failed to upload screenshot: " + *error);
                }

                auto upload_status = http->GetStatusCode();
                if (!upload_status) {
                    return std::unexpected(upload_status.error().ToString());
                }
                if (*upload_status != 200) {
                    return std::unexpected("Unexpected status code: " +
                                           std::to_string(*upload_status));
                }
                std::string result = http->ReadAll();
                http->Close();
                ESP_LOGI(TAG, "Snapshot screen result: %s", result.c_str());
                return true;
            });

        AddUserOnlyTool(
            "self.screen.preview_image", "Preview an image on the screen",
            PropertyList({Property("url", kPropertyTypeString)}),
            [display](const PropertyList& properties) -> ToolResult {
                auto url = properties["url"].value<std::string>();
                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);

                if (auto opened = http->Open("GET", url); !opened) {
                    return std::unexpected("Failed to open URL: " + url + " (" +
                                           opened.error().ToString() + ")");
                }
                auto status_code = http->GetStatusCode();
                if (!status_code) {
                    return std::unexpected(status_code.error().ToString());
                }
                if (*status_code != 200) {
                    return std::unexpected("Unexpected status code: " +
                                           std::to_string(*status_code));
                }

                size_t content_length = http->GetBodyLength();
                using BufferPtr = std::unique_ptr<char, decltype(&heap_caps_free)>;
                BufferPtr data(
                    static_cast<char*>(heap_caps_malloc(content_length, MALLOC_CAP_8BIT)),
                    heap_caps_free);
                if (data == nullptr) {
                    return std::unexpected("Failed to allocate memory for image: " + url);
                }
                size_t total_read = 0;
                while (total_read < content_length) {
                    auto ret = http->Read(data.get() + total_read, content_length - total_read);
                    if (!ret) {
                        return std::unexpected("Failed to download image: " + url);
                    }
                    if (*ret == 0) {
                        break;
                    }
                    total_read += *ret;
                }
                http->Close();

                auto image = std::make_unique<LvglAllocatedImage>(data.release(), total_read);
                if (!image->IsValid()) {
                    return std::unexpected("Downloaded image is invalid: " + url);
                }
                display->SetPreviewImage(std::move(image));
                return true;
            });
#endif  // CONFIG_LV_USE_SNAPSHOT
    }
#endif  // HAVE_LVGL

    // Assets download url (always registered — Settings storage works regardless of partition
    // layout)
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

void McpServer::LogToolInventory() const {
    size_t user_only = 0;
    for (const auto& tool : tools_) {
        if (tool->user_only()) {
            ++user_only;
        }
    }
    ESP_LOGI(TAG, "%u tools registered: %u visible to the model, %u user-only",
             (unsigned)tools_.size(), (unsigned)(tools_.size() - user_only), (unsigned)user_only);
}

void McpServer::AddTool(std::unique_ptr<McpTool> tool) {
    // Prevent adding duplicate tools
    if (std::find_if(tools_.begin(), tools_.end(), [&tool](const auto& existing) {
            return existing->name() == tool->name();
        }) != tools_.end()) {
        ESP_LOGW(TAG, "Tool %s already added", tool->name().c_str());
        return;
    }

    ESP_LOGI(TAG, "Add tool: %s%s", tool->name().c_str(), tool->user_only() ? " [user]" : "");
    tools_.push_back(std::move(tool));
}

void McpServer::AddTool(const std::string& name, const std::string& description,
                        const PropertyList& properties, ToolCallback callback) {
    AddTool(std::make_unique<McpTool>(name, description, properties, std::move(callback)));
}

void McpServer::AddUserOnlyTool(const std::string& name, const std::string& description,
                                const PropertyList& properties, ToolCallback callback) {
    auto tool = std::make_unique<McpTool>(name, description, properties, std::move(callback));
    tool->set_user_only(true);
    AddTool(std::move(tool));
}

void McpServer::ParseMessage(const std::string& message, ResponseSender response_sender) {
    CJsonUniquePtr json(cJSON_Parse(message.c_str()));
    if (json == nullptr) {
        ESP_LOGE(TAG, "Failed to parse MCP message: %s", message.c_str());
        return;
    }
    ParseMessage(json.get(), std::move(response_sender));
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

void McpServer::ParseMessage(const cJSON* json, ResponseSender response_sender) {
    // Check JSONRPC version
    auto version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || !cJSON_IsString(version) ||
        strcmp(version->valuestring, "2.0") != 0) {
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
        std::string message =
            "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{"
            "\"name\":\"" BOARD_NAME "\",\"version\":\"";
        message += app_desc->version;
        message += "\"}}";
        ReplyResult(id_int, message, response_sender);
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
        GetToolsList(id_int, cursor_str, list_user_only_tools, response_sender);
    } else if (method_str == "tools/call") {
        if (!cJSON_IsObject(params)) {
            ESP_LOGE(TAG, "tools/call: Missing params");
            ReplyError(id_int, -32602, "Missing params", response_sender);
            return;
        }
        auto tool_name = cJSON_GetObjectItem(params, "name");
        if (!cJSON_IsString(tool_name)) {
            ESP_LOGE(TAG, "tools/call: Missing name");
            ReplyError(id_int, -32602, "Missing tool name", response_sender);
            return;
        }
        auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
        if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments)) {
            ESP_LOGE(TAG, "tools/call: Invalid arguments");
            ReplyError(id_int, -32602, "Invalid arguments: expected object", response_sender);
            return;
        }
        DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments,
                   std::move(response_sender));
    } else {
        ESP_LOGE(TAG, "Method not implemented: %s", method_str.c_str());
        ReplyError(id_int, -32601, "Method not implemented: " + method_str, response_sender);
    }
}

void McpServer::SendResponse(const std::string& payload, const ResponseSender& response_sender) {
    if (response_sender) {
        response_sender(payload);
    } else {
        Application::GetInstance().SendMcpMessage(payload);
    }
}

void McpServer::ReplyResult(int id, const std::string& result,
                            const ResponseSender& response_sender) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id) + ",\"result\":";
    payload += result;
    payload += "}";
    SendResponse(payload, response_sender);
}

void McpServer::ReplyError(int id, int code, const std::string& message,
                           const ResponseSender& response_sender) {
    CJsonUniquePtr json_message(cJSON_CreateString(message.c_str()));
    if (json_message == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate MCP error message");
        return;
    }
    CJsonStringUniquePtr encoded_message(cJSON_PrintUnformatted(json_message.get()));
    if (encoded_message == nullptr) {
        ESP_LOGE(TAG, "Failed to serialize MCP error message");
        return;
    }

    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id);
    payload += ",\"error\":{\"code\":";
    payload += std::to_string(code);
    payload += ",\"message\":";
    payload += encoded_message.get();
    payload += "}}";
    SendResponse(payload, response_sender);
}

void McpServer::ReplyError(int id, const std::string& message,
                           const ResponseSender& response_sender) {
    ReplyError(id, -32603, message, response_sender);
}

void McpServer::GetToolsList(int id, const std::string& cursor, bool list_user_only_tools,
                             const ResponseSender& response_sender) {
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
        ESP_LOGE(TAG, "tools/list: Failed to add tool %s because of payload size limit",
                 next_cursor.c_str());
        ReplyError(id, "Failed to add tool " + next_cursor + " because of payload size limit",
                   response_sender);
        return;
    }

    if (next_cursor.empty()) {
        json += "]}";
    } else {
        json += "],\"nextCursor\":\"" + next_cursor + "\"}";
    }

    ReplyResult(id, json, response_sender);
}

void McpServer::DoToolCall(int id, const std::string& tool_name, const cJSON* tool_arguments,
                           ResponseSender response_sender) {
    auto tool_iter = std::find_if(tools_.begin(), tools_.end(), [&tool_name](const auto& tool) {
        return tool->name() == tool_name;
    });

    if (tool_iter == tools_.end()) {
        ESP_LOGE(TAG, "tools/call: Unknown tool: %s", tool_name.c_str());
        ReplyError(id, -32602, "Unknown tool: " + tool_name, response_sender);
        return;
    }

    McpTool* tool = tool_iter->get();
    PropertyList arguments = tool->properties();
    for (auto& argument : arguments) {
        bool found = false;
        std::expected<void, std::string> validation;
        if (cJSON_IsObject(tool_arguments)) {
            auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
            if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
                validation = argument.set_value<bool>(value->valueint == 1);
                found = true;
            } else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value)) {
                validation = argument.set_value<int>(value->valueint);
                found = true;
            } else if (argument.type() == kPropertyTypeString && cJSON_IsString(value)) {
                validation = argument.set_value<std::string>(value->valuestring);
                found = true;
            } else if (value != nullptr) {
                ESP_LOGE(TAG, "tools/call: Invalid type for argument: %s", argument.name().c_str());
                ReplyError(id, -32602, "Invalid type for argument: " + argument.name(),
                           response_sender);
                return;
            }
        }

        if (found && !validation) {
            ESP_LOGE(TAG, "tools/call: %s", validation.error().c_str());
            ReplyError(id, -32602, validation.error(), response_sender);
            return;
        }

        if (!argument.has_default_value() && !found) {
            ESP_LOGE(TAG, "tools/call: Missing required argument: %s", argument.name().c_str());
            ReplyError(id, -32602, "Missing required argument: " + argument.name(),
                       response_sender);
            return;
        }
    }

    // Use main thread to call the tool
    auto& app = Application::GetInstance();
    app.Schedule([this, id, tool, arguments = std::move(arguments),
                  response_sender = std::move(response_sender)]() {
        auto result = tool->Call(arguments);
        if (!result) {
            ESP_LOGE(TAG, "tools/call: %s", result.error().c_str());
            ReplyError(id, result.error(), response_sender);
            return;
        }
        ReplyResult(id, *result, response_sender);
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
    auto tool_iter = std::find_if(tools_.begin(), tools_.end(), [&tool_name](const auto& tool) {
        return tool->name() == tool_name;
    });
    
    if (tool_iter == tools_.end()) {
        ESP_LOGE(TAG, "tools/call: Unknown tool: %s", tool_name.c_str());
        ReplyErrorPro(id, "Unknown tool: " + tool_name);
        return;
    }

    PropertyList arguments = (*tool_iter)->properties();
    // 项目禁用 C++ 异常（-fno-exceptions），参数校验与工具调用不使用 try/catch
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

    // Use main thread to call the tool
    auto& app = Application::GetInstance();
    app.Schedule([this, id, tool_iter, arguments = std::move(arguments)]() {
        // Call 返回 std::expected<std::string, std::string>（main 新工具契约）
        auto result = (*tool_iter)->Call(arguments);
        if (result) {
            ReplyResultPro(id, *result);
        } else {
            ReplyErrorPro(id, result.error());
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