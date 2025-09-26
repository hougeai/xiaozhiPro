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

#include "application.h"
#include "display.h"
#include "oled_display.h"
#include "board.h"
#include "alarm.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "lvgl_display.h"

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
            "Take a photo and explain it. Use this tool after the user asks you to see something.\n"
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
            ESP_LOGI(TAG, "SetAlarm");
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
    // knowledge base
    AddTool("self.kb.query",
        "当用户咨询硅基流动产品相关内容时，可以通过关键词检索知识库获取更多的上下文"
        "Args:"
        "query: str, 搜索的关键词，多个关键词用空格隔开",
        PropertyList({
            Property("query", kPropertyTypeString)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto http = Board::GetInstance().GetNetwork()->CreateHttp(1);
            http->SetHeader("Content-Type", "application/json");
            http->SetHeader("X-API-Key", "sk-Xea7HELObS3gYurLAgC-nepXSZdT6m3CwkV6nYqSxePRzdTa");
            // 构造请求的JSON数据
            cJSON* json_data = cJSON_CreateObject();
            cJSON_AddStringToObject(json_data, "query_text", properties["query"].value<std::string>().c_str());
            cJSON_AddNumberToObject(json_data, "vector_threshold", 0.5);
            cJSON_AddNumberToObject(json_data, "keyword_threshold", 0.3);
            cJSON_AddNumberToObject(json_data, "match_count", 3);
            char* json_string = cJSON_PrintUnformatted(json_data);
            std::string data(json_string);
            cJSON_free(json_string);
            cJSON_Delete(json_data);
            http->SetContent(std::move(data));
            if (!http->Open("GET", "http://106.55.224.164:3008/api/v1/knowledge-bases/kb-00000001/hybrid-search")) {
                ESP_LOGE(TAG, "Failed to open HTTP connection");
                return R"({"success":false,"message":"Failed to connect to knowledge base"})";
            }
            auto status_code = http->GetStatusCode();
            if (status_code != 200) {
                ESP_LOGE(TAG, "Knowledge base request failed with status code: %d", status_code);
                return R"({"success":false,"message":"Knowledge base request failed"})";
            }
            std::string response = http->ReadAll();
            http->Close();
            // 解析响应并提取需要的数据
            cJSON* root = cJSON_Parse(response.c_str());
            if (root == nullptr) {
                ESP_LOGE(TAG, "Failed to parse knowledge base response");
                return R"({"success":false,"message":"Failed to parse knowledge base response"})";
            }
            
            cJSON* data_array = cJSON_GetObjectItem(root, "data");
            if (!cJSON_IsArray(data_array)) {
                cJSON_Delete(root);
                ESP_LOGE(TAG, "Invalid data format in knowledge base response");
                return R"({"success":false,"message":"Invalid data format in knowledge base response"})";
            }
            // 创建只包含content字段的简单数组
            cJSON* content_array = cJSON_CreateArray();
            int array_size = cJSON_GetArraySize(data_array);
            for (int i = 0; i < array_size && i < 3; i++) { // 只取前3个结果
                cJSON* item = cJSON_GetArrayItem(data_array, i);
                if (item) {
                    cJSON* content = cJSON_GetObjectItem(item, "content");
                    if (content) {
                        cJSON_AddItemToArray(content_array, cJSON_CreateString(content->valuestring));
                    }
                }
            }
            cJSON_Delete(root);
            // 构造简化后的响应
            cJSON* result = cJSON_CreateObject();
            cJSON_AddItemToObject(result, "contents", content_array);
            cJSON_AddBoolToObject(result, "success", true);
            char* result_json = cJSON_PrintUnformatted(result);
            std::string result_str(result_json);
            cJSON_free(result_json);
            cJSON_Delete(result);
            return result_str;
        });
    // music
    AddTool("self.online_music.play",
        "当用户要求播放音乐时使用此工具，会自动获取歌曲详情并开始流式播放。\n"
        "参数:\n"
        "  `song_name`: 要播放的歌曲名称（必需）。\n"
        "  `singer`: 歌手名称（可选，默认为空字符串）。\n"
        "返回:\n"
        "  播放状态信息。",
        PropertyList({
            Property("song_name", kPropertyTypeString),
            Property("singer", kPropertyTypeString, "")
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            if(app.music_ == nullptr){
                ESP_LOGE(TAG, "MusicManager is nullptr");
                 return R"({"success":false,"message":"MusicManager is not initialized"})";
            }
            auto song_name = properties["song_name"].value<std::string>();
            auto singer = properties["singer"].value<std::string>();
            ESP_LOGI(TAG, "Playing music: %s %s", song_name.c_str(), singer.c_str());
            
            if (!app.music_->Download(song_name, singer)) {
                return "{\"success\": false, \"message\": \"获取音乐资源失败\"}";
            }
            return "{\"success\": true, \"message\": \"音乐开始播放\"}";
        });
    // sys
    AddTool("self.sys.get_verification_code",
        "当用户要求查询设备验证码时使用此工具，返回验证码。\n",
        PropertyList(),
        [&board](const PropertyList& properties) -> ReturnValue {
            auto http = board.GetNetwork()->CreateHttp(1); // 1 是connect_id
            http->SetHeader("Content-Type", "application/json");
            std::string data = board.GetSystemInfoJson();
            http->SetContent(std::move(data));
            if (!http->Open("POST", std::string(CONFIG_SERVER_URL) + "/get_code")) {
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
            cJSON* code = cJSON_GetObjectItem(root, "code");
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
                auto ota = std::make_unique<Ota>();
                
                bool success = app.UpgradeFirmware(*ota, url);
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

    // Assets download url
    auto& assets = Assets::GetInstance();
    if (assets.partition_valid()) {
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
    }
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
