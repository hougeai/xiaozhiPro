#include "alarm.h"
#include <ctime>
#include <new>
#include <utility>
#include <cJSON.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "board.h"
#include "system_info.h"
#define TAG "AlarmManager"

// 闹钟云同步的 HTTP 超时（毫秒）。服务端异常时必须尽快返回，避免长时间占用调用方任务。
#define ALARM_HTTP_TIMEOUT_MS 5000

std::string FormatTimestamp(time_t timestamp) {
    struct tm* local_time = localtime(&timestamp);
    char time_str[80];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", local_time);
    return std::string(time_str);
}


AlarmManager::AlarmManager(){
    ESP_LOGI(TAG, "AlarmManager init");
    ring_flag_ = false;
    ring_name_ = "alarm";
    timer_ = nullptr; // 初始为空
    server_url_ = std::string(CONFIG_SERVER_URL) + "/device_alarm";
    max_alarms_ = 10;

    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            auto* alarm_manager = static_cast<AlarmManager*>(arg);
            alarm_manager->OnAlarm();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "alarm_timer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_));
    // 注意：首次云端同步（HTTPS/TLS/RSA 硬件）不再在构造时启动，
    // 改由外部在系统启动完成、main 任务空闲后再显式调用 StartReloadTask()，
    // 避免与唤醒词初始化等并发占用 RSA 硬件导致握手异常。
}

void AlarmManager::StartReloadTask() {
    if (init_started_) {
        return;
    }
    init_started_ = true;

    auto task = [](void* arg) {
        auto* self = static_cast<AlarmManager*>(arg);
        // 网络不可用会由 ReloadAlarms 内部的 HTTP 超时（ALARM_HTTP_TIMEOUT_MS）
        // 兜底失败并回退本地闹钟，这里无需自行探测网络状态。
        self->ReloadAlarms();
        vTaskDelete(nullptr);
    };
    if (xTaskCreate(task, "alarm_init", 8192, this, 1, nullptr) != pdPASS) {
        // 任务创建失败则退化为同步加载，保证闹钟至少能初始化
        ESP_LOGW(TAG, "Failed to create alarm init task, fall back to sync ReloadAlarms");
        init_started_ = false;
        ReloadAlarms();
    }
}

AlarmManager::~AlarmManager(){
    if(timer_){
        esp_timer_stop(timer_);
        esp_timer_delete(timer_);
        timer_ = nullptr;
    }
}

std::string AlarmManager::ReloadAlarms() {
    // 1.锁外拉取云端闹钟（网络操作不持锁），拿到解析结果后再统一写入
    std::vector<Alarm> cloud_alarms;
    std::vector<std::pair<Alarm, std::string>> pending;
    std::string sync_result = SyncAlarmFromCloud(cloud_alarms, pending);

    // 2.持锁重建闹钟列表
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Settings settings_("alarm_clock", true); // 闹钟设置
        alarms_.clear();
        if (sync_result.empty()) {
            // 云端拉取成功：用云端数据覆盖本地设置
            for(int i = 0; i < max_alarms_; i++){
                settings_.SetString("alarm_" + std::to_string(i), "");
                settings_.SetInt("alarm_time_" + std::to_string(i), 0);
                settings_.SetInt("alarm_rpt_" + std::to_string(i), 0);
                settings_.SetInt("alarm_itv_" + std::to_string(i), 0);
            }
            for (const auto& alarm : cloud_alarms) {
                alarms_.push_back(alarm);
                settings_.SetString("alarm_" + std::to_string(alarm.id), alarm.name);
                settings_.SetInt("alarm_time_" + std::to_string(alarm.id), alarm.time);
                settings_.SetInt("alarm_rpt_" + std::to_string(alarm.id), alarm.repeat);
                settings_.SetInt("alarm_itv_" + std::to_string(alarm.id), alarm.interval);
                ESP_LOGI(TAG, "Loaded alarm from cloud: %s %s", alarm.name.c_str(), FormatTimestamp(alarm.time).c_str());
            }
        } else {
            // 云端失败：回退到本地设置，保持原有数据不被清空
            ESP_LOGW(TAG, "Failed to sync alarms from cloud: %s, loading from local settings", sync_result.c_str());
            for(int i = 0; i < max_alarms_; i++){
                std::string alarm_name = settings_.GetString("alarm_" + std::to_string(i));
                if(alarm_name != ""){
                    Alarm alarm{};
                    alarm.id = i; // 闹钟的索引
                    alarm.name = alarm_name;
                    alarm.time = settings_.GetInt("alarm_time_" + std::to_string(i));
                    alarm.repeat = settings_.GetInt("alarm_rpt_" + std::to_string(i));
                    alarm.interval = settings_.GetInt("alarm_itv_" + std::to_string(i));
                    // 检查本地存储的闹钟时间是否已过期，如果是重复闹钟则顺延到未来
                    time_t now = time(NULL);
                    if (alarm.time <= now) {
                        if (alarm.repeat > 0 && alarm.interval > 0) {
                            int periods = (now - alarm.time) / alarm.interval + 1;
                            alarm.time += periods * alarm.interval;
                            // 更新到NVS
                            settings_.SetInt("alarm_time_" + std::to_string(i), alarm.time);
                            ESP_LOGI(TAG, "Alarm %s expired, adjusted to future: %s", alarm.name.c_str(), FormatTimestamp(alarm.time).c_str());
                        } else {
                            ESP_LOGI(TAG, "Alarm %s expired and not repeatable, skipping", alarm.name.c_str());
                            continue;
                        }
                    }
                    alarms_.push_back(alarm);
                    ESP_LOGI(TAG, "Alarm %d %s add again at %s", alarm.id, alarm.name.c_str(), FormatTimestamp(alarm.time).c_str());
                }
            }
        }
    }

    time_t now = time(NULL);
    // 清除过期的闹钟（内部自行加锁）
    ClearOverdueAlarm(now);
    // 调度最近的闹钟
    ScheduleNextAlarm(now);
    // 锁外异步回写被顺延过的云端闹钟
    SyncAlarmsToCloudAsync(std::move(pending));

    return sync_result;
}

void AlarmManager::CreateAndStartTimer(int delay) {
    if (timer_ == nullptr) {
        ESP_LOGW(TAG, "Timer not created, skip start");
        return;
    }
    if (delay <= 0) {
        ESP_LOGW(TAG, "Invalid timer interval: %d sec, skip start", delay);
        return;
    }
    esp_timer_stop(timer_); // 先停止之前的任务
    ESP_LOGI(TAG, "Start timer, trigger in %d sec", delay);
    // 用 64 位运算避免 delay 较大时乘法溢出
    esp_err_t err = esp_timer_start_once(timer_, (uint64_t)delay * 1000000ULL);
    if (err != ESP_OK) {
        // 启动失败不应让整机 abort，记录后由后续调度重试
        ESP_LOGE(TAG, "Failed to start alarm timer: %s", esp_err_to_name(err));
    }
}

void AlarmManager::OnAlarm(){
    time_t now = time(NULL);
    // 持锁查找到期闹钟并生成提醒文本（ClearOverdueAlarm 可能删除元素，因此不能持有裸指针跨锁使用）
    std::string ring_text;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for(const auto& alarm : alarms_){
            if(alarm.time <= now){
                ring_text = alarm.name + " " + FormatTimestamp(alarm.time);
                found = true;
                break;
            }
        }
        if (found) {
            ring_name_ = ring_text;
        }
    }
    if (!found) {
        ESP_LOGI(TAG, "OnAlarm triggered but no valid alarm due");
        ring_flag_ = false;
        return;
    }
    // 有闹钟要提醒
    ESP_LOGI(TAG, "=----ring----=");
    ring_flag_ = true;
    ESP_LOGI(TAG, "Alarm: %s is ringing", ring_text.c_str());

    // 清除过期的闹钟
    ClearOverdueAlarm(now);
    // 调度下一个闹钟
    ScheduleNextAlarm(now);
}

void AlarmManager::ScheduleNextAlarm(time_t now){
    std::lock_guard<std::mutex> lock(mutex_);
    ScheduleNextAlarmLocked(now);
}

void AlarmManager::ScheduleNextAlarmLocked(time_t now){
    Alarm *next_alarm = GetProximateAlarm(now);
    if (next_alarm != nullptr){
        ESP_LOGI(TAG, "Next Alarm %d %s set at %s repeat %d interval %d", next_alarm->id, next_alarm->name.c_str(), FormatTimestamp(next_alarm->time).c_str(), next_alarm->repeat, next_alarm->interval);
        CreateAndStartTimer(next_alarm->time - now);
    } else {
        ESP_LOGI(TAG, "no alarm now");
    }
}

Alarm * AlarmManager::GetProximateAlarm(time_t now){
    Alarm *current_alarm_ = nullptr;
    for(auto& alarm : alarms_){
        if(alarm.time > now && (current_alarm_ == nullptr || alarm.time < current_alarm_->time)){
            current_alarm_ = &alarm; // 获取当前时间以后第一个发生的时钟句柄
        }
    }
    return current_alarm_;
}

void AlarmManager::ClearOverdueAlarm(time_t now){
    // 本函数可能运行在 esp_timer 任务上（OnAlarm 触发），该任务栈很小且为全系统共享，
    // 因此锁内只做内存与 NVS 操作，网络同步收集后在锁外交给独立任务执行。
    std::vector<std::pair<Alarm, std::string>> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Settings settings_("alarm_clock", true); // 闹钟设置(硬盘存储)
        // 如果有repeat，则更新 alarm_first 和 settings，否则删除
        for (auto it = alarms_.begin(); it != alarms_.end();){
            // 未到期的
            if (it->time > now){
                ++it;
                continue;
            }
            // 到期处理：在 0-9 范围内寻找
            bool found = false;
            for (int i = 0; i < max_alarms_; i++){
                if(settings_.GetString("alarm_" + std::to_string(i)) == it->name && settings_.GetInt("alarm_time_" + std::to_string(i)) == it->time){
                    found = true;
                    it->repeat--; // 次数减去1
                    if (it->repeat > 0) {
                        it->time += it->interval;
                        settings_.SetInt("alarm_time_" + std::to_string(i), it->time);
                        settings_.SetInt("alarm_rpt_" + std::to_string(i), it->repeat);
                        ESP_LOGI(TAG, "Alarm %s will repeat %d, queue cloud update", it->name.c_str(), it->repeat);
                        pending.emplace_back(*it, "update");
                        ++it;
                    } else {
                        settings_.SetString("alarm_" + std::to_string(i), ""); // 名称置为空
                        settings_.SetInt("alarm_time_" + std::to_string(i), 0);
                        settings_.SetInt("alarm_rpt_" + std::to_string(i), 0);
                        settings_.SetInt("alarm_itv_" + std::to_string(i), 0);
                        ESP_LOGI(TAG, "Alarm %s is overdue, repeat %d, queue cloud delete", it->name.c_str(), it->repeat);
                        pending.emplace_back(*it, "delete");
                        it = alarms_.erase(it); // 删除过期的闹钟, 此时it指向下一个元素
                    }
                    break; // 命中，跳出for循环
                }
            }
            if (!found) {
                ESP_LOGI(TAG, "Alarm %s not found in settings, skipping", it->name.c_str());
                ++it;
            }
        }
        // 如果所有闹钟都过期了，停止定时器
        if (alarms_.empty() && timer_) {
            esp_timer_stop(timer_);
            ESP_LOGI(TAG, "All alarms overdue, timer stopped");
        }
    }
    // 锁外异步同步，定时器回调可以立即返回
    SyncAlarmsToCloudAsync(std::move(pending));
}

void AlarmManager::SyncAlarmsToCloudAsync(std::vector<std::pair<Alarm, std::string>> pending) {
    if (pending.empty()) {
        return;
    }
    struct SyncContext {
        AlarmManager* manager;
        std::vector<std::pair<Alarm, std::string>> items;
    };
    auto* ctx = new (std::nothrow) SyncContext{this, std::move(pending)};
    if (ctx == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate alarm sync context");
        return;
    }
    auto task = [](void* arg) {
        auto* ctx = static_cast<SyncContext*>(arg);
        for (const auto& item : ctx->items) {
            std::string result = ctx->manager->SyncAlarmToCloud(item.first, item.second);
            ESP_LOGI(TAG, "Async sync alarm %s (%s): %s", item.first.name.c_str(), item.second.c_str(),
                     result.empty() ? "success" : result.c_str());
        }
        delete ctx;
        vTaskDelete(nullptr);
    };
    if (xTaskCreate(task, "alarm_cloud_sync", 12288, ctx, 1, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create alarm cloud sync task");
        delete ctx;
    }
}

std::string AlarmManager::SetAlarm(int delay, int hour, int minute, int repeat, int interval, std::string name){
    // 开始设置新闹钟
    Alarm alarm{}; // 一个新的闹钟，全部初始化为0，Alarm alarm则不会初始化，会拿到一个随机值
    // 计算 time
    time_t now = time(NULL);
    std::string time_str = FormatTimestamp(now);
    ESP_LOGI(TAG, "当前时间戳: %ld (%s)", (long)now, time_str.c_str());
    if ((hour >= 0 && hour < 24) || (minute >= 0 && minute < 60)) {
        // 计算距离最近的指定hour和minute的开始时间
        struct tm* target_time = localtime(&now);
        // 设置目标时间的小时和分钟
        target_time->tm_hour = (hour >= 0 && hour < 24) ? hour : 0;
        target_time->tm_min = (minute >= 0 && minute < 60) ? minute : 0;
        target_time->tm_sec = 0;  // 秒数设为0
        // 转换为time_t
        time_t alarm_time = mktime(target_time);
        // 如果计算出的时间已过，则添加一天
        if (alarm_time < now) {
            alarm_time += 24 * 60 * 60;  // 添加一天的秒数
        }
        alarm.time = alarm_time;
    } else {
        alarm.time = now + delay;
    }
    alarm.name = name;
    alarm.repeat = repeat;
    alarm.interval = interval;

    // 1.持锁占位：立刻写入槽位名称，避免释放锁做网络时被并发的 SetAlarm 抢占同一 id
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Settings settings_("alarm_clock", true); // 闹钟设置
        bool slot_found = false;
        for (int i = 0; i < max_alarms_; i++) {
            if(settings_.GetString("alarm_" + std::to_string(i)) == ""){
                alarm.id = i; // 将索引赋值给闹钟的id
                slot_found = true;
                break;
            }
        }
        if (!slot_found) {
            ESP_LOGW(TAG, "No free alarm slot available");
            return "No free alarm slot available";
        }
        settings_.SetString("alarm_" + std::to_string(alarm.id), alarm.name);
    }

    // 2.锁外同步到云端，失败则释放占位并返回（网络操作绝不能持锁，否则会拖住定时器等其它任务）
    std::string result = SyncAlarmToCloud(alarm, "add");
    if (!result.empty()) {
        ESP_LOGI(TAG, "Alarm %s sync to cloud: %s", alarm.name.c_str(), result.c_str());
        std::lock_guard<std::mutex> lock(mutex_);
        Settings settings_("alarm_clock", true);
        settings_.SetString("alarm_" + std::to_string(alarm.id), "");
        return result;
    }

    // 3.重新持锁保存到nvs并调度定时器
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Settings settings_("alarm_clock", true); // 闹钟设置
        settings_.SetInt("alarm_time_" + std::to_string(alarm.id), alarm.time);
        settings_.SetInt("alarm_rpt_" + std::to_string(alarm.id), repeat);
        settings_.SetInt("alarm_itv_" + std::to_string(alarm.id), interval);
        alarms_.push_back(alarm); // 全部赋值后再push_back
        ESP_LOGI(TAG, "Alarm %d %s set at %s repeat %d interval %d", alarm.id, alarm.name.c_str(), FormatTimestamp(alarm.time).c_str(), alarm.repeat, alarm.interval);
        // 获取下一个要启动的闹钟
        ScheduleNextAlarmLocked(now);
    }
    return ""; // 返回空字符串表示成功
}

std::string AlarmManager::DelAlarmById(int id){
    if (id < 0) {
        ESP_LOGI(TAG, "id should be >= 0");
        return "id should be >= 0";
    }

    // 1.持锁找到目标闹钟并拷贝出来（此时不修改列表，云端失败时无需回滚）
    Alarm target{};
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for(const auto& alarm : alarms_){
            if(alarm.id == id){
                target = alarm;
                found = true;
                break;
            }
        }
    }
    
    if(!found){
        ESP_LOGW(TAG, "Alarm %d not found", id);
        return "Alarm not found";
    }
    
    // 2.锁外同步到云端
    std::string result = SyncAlarmToCloud(target, "delete");
    if (!result.empty()) {
        return result;
    }

    // 3.重新持锁删除本地记录并调度定时器
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Settings settings_("alarm_clock", true); // 闹钟设置
        for(auto it = alarms_.begin(); it != alarms_.end(); ++it){
            if(it->id == id){
                ESP_LOGI(TAG, "Alarm %d %s deleted", it->id, it->name.c_str());
                alarms_.erase(it);
                break;
            }
        }
        // 从设置中删除指定名称的闹钟
        settings_.SetString("alarm_" + std::to_string(id), "");
        settings_.SetInt("alarm_time_" + std::to_string(id), 0);
        settings_.SetInt("alarm_rpt_" + std::to_string(id), 0);
        settings_.SetInt("alarm_itv_" + std::to_string(id), 0);

        ESP_LOGI(TAG, "Alarm %d deleted from nvs", id);

        // 重新设置下一个闹钟
        ScheduleNextAlarmLocked(time(NULL));
    }
    return "";
}

std::string AlarmManager::QueryAllAlarm(){
    std::lock_guard<std::mutex> lock(mutex_);
    std::string result = R"([)";
    bool first = true;
    for(auto& alarm : alarms_){
        if (!first) {
            result += R"(,)";
        }
        first = false;
        result += R"({)";
        result += R"("id":)" + std::to_string(alarm.id) + R"(,)";
        result += R"("name":")" + alarm.name + R"(",)";
        result += R"("time":")" + FormatTimestamp(alarm.time) + R"(",)";
        result += R"("repeat":)" + std::to_string(alarm.repeat) + R"(,)";
        result += R"("interval":)" + std::to_string(alarm.interval);
        result += R"(})";
    }
    result += R"(])";
    return result;
}

void AlarmManager::ClearRing(){
    ESP_LOGI("Alarm", "clear");
    ring_flag_ = false;
};

std::string AlarmManager::CleanUpAlarm() {
    // 清空所有闹钟：先清除云端的（锁外，避免网络阻塞持锁），再清除本地的
    std::string result = ClearCloudAlarms();
    if (!result.empty()) {
        return result;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    Settings settings_("alarm_clock", true); // 闹钟设置
    alarms_.clear();
    // 清空所有设置
    for(int i = 0; i < max_alarms_; i++){
        settings_.SetString("alarm_" + std::to_string(i), "");
        settings_.SetInt("alarm_time_" + std::to_string(i), 0);
        settings_.SetInt("alarm_rpt_" + std::to_string(i), 0);
        settings_.SetInt("alarm_itv_" + std::to_string(i), 0);
    }
    // 停止当前定时器
    if (timer_) {
        esp_timer_stop(timer_);
    }
    ESP_LOGI(TAG, "All alarms cleaned up");
    return "";
}

std::string AlarmManager::SyncAlarmToCloud(const Alarm& alarm, const std::string& action) {
    // 创建HTTP客户端
    auto http = Board::GetInstance().GetNetwork()->CreateHttp();
    
    http->SetTimeout(ALARM_HTTP_TIMEOUT_MS); // 服务端异常时快速失败
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("Device-ID", SystemInfo::GetMacAddress().c_str());
    
    // 构造JSON数据
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "action", action.c_str()); // "add", "delete", "update", "clear"

    cJSON* alarm_obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(alarm_obj, "nvs_id", alarm.id);
    cJSON_AddNumberToObject(alarm_obj, "time", alarm.time);
    cJSON_AddNumberToObject(alarm_obj, "repeat", alarm.repeat);
    cJSON_AddNumberToObject(alarm_obj, "interval", alarm.interval);
    cJSON_AddStringToObject(alarm_obj, "name", alarm.name.c_str());
    cJSON_AddItemToObject(root, "alarm", alarm_obj);

    char* json_str = cJSON_PrintUnformatted(root);
    std::string data(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    
    http->SetContent(std::move(data));
    
    // 发送请求
    if (!http->Open("POST", server_url_.c_str())) {
        ESP_LOGE(TAG, "Failed to connect to alarm sync server");
        http->Close();
        return "Failed to connect to alarm sync server";
    }
    
    int status_code = http->GetStatusCode().value_or(0);
    std::string response = http->ReadAll();
    http->Close();
    if (status_code != 200) {
        cJSON* root = cJSON_Parse(response.c_str());
        if (root) {
            cJSON* msg = cJSON_GetObjectItem(root, "msg");
            std::string error_msg = msg ? msg->valuestring : "Unknown error";
            cJSON_Delete(root);
            return error_msg;  // 返回具体错误信息，如 "Device not found"
        }
        return "Failed to sync alarms to cloud";
    }
    ESP_LOGI(TAG, "Successfully %s alarm to cloud", action.c_str());
    return "";
}

std::string AlarmManager::ClearCloudAlarms() {
    // 创建HTTP客户端
    auto http = Board::GetInstance().GetNetwork()->CreateHttp();
    
    http->SetTimeout(ALARM_HTTP_TIMEOUT_MS); // 服务端异常时快速失败
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("Device-ID", SystemInfo::GetMacAddress().c_str());
    
    // 构造JSON数据
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "action", "clear");

    char* json_str = cJSON_PrintUnformatted(root);
    std::string data(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    
    http->SetContent(std::move(data));
    
    // 发送请求
    if (!http->Open("POST", server_url_.c_str())) {
        ESP_LOGE(TAG, "Failed to connect to alarm sync server");
        http->Close();
        return "Failed to connect to alarm sync server";
    }
    
    int status_code = http->GetStatusCode().value_or(0);
    std::string response = http->ReadAll();
    http->Close();
    if (status_code != 200) {
        cJSON* root = cJSON_Parse(response.c_str());
        if (root) {
            cJSON* msg = cJSON_GetObjectItem(root, "msg");
            std::string error_msg = msg ? msg->valuestring : "Unknown error";
            cJSON_Delete(root);
            return error_msg;  // 返回具体错误信息，如 "Device not found"
        }
        return "Failed to sync alarms to cloud";
    }
    ESP_LOGI(TAG, "Successfully cleared alarms on cloud");
    return "";
}

std::string AlarmManager::SyncAlarmFromCloud(std::vector<Alarm>& out_alarms,
                                             std::vector<std::pair<Alarm, std::string>>& out_pending) {
    out_alarms.clear();
    out_pending.clear();

    // 创建HTTP客户端
    auto http = Board::GetInstance().GetNetwork()->CreateHttp();

    http->SetTimeout(ALARM_HTTP_TIMEOUT_MS); // 服务端异常时快速失败
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("Device-ID", SystemInfo::GetMacAddress().c_str());

    // 发送GET请求
    if (!http->Open("GET", server_url_.c_str())) {
        ESP_LOGE(TAG, "Failed to connect to alarm sync server");
        http->Close();
        return "Failed to connect to alarm sync server";
    }

    int status_code = http->GetStatusCode().value_or(0);
    std::string response = http->ReadAll();
    http->Close();
    cJSON* root = cJSON_Parse(response.c_str());
    if (status_code != 200) {
        ESP_LOGE(TAG, "Failed to fetch alarms from cloud, status code: %d", status_code);
        if (root) {
            cJSON *msg = cJSON_GetObjectItem(root, "msg");
            std::string error_msg = cJSON_IsString(msg) ? msg->valuestring : "Unknown error";
            cJSON_Delete(root);
            return error_msg; //返回具体错误信息
        }
        return "Failed to fetch alarms from cloud";
    }
    if (!root) {
        ESP_LOGE(TAG, "Failed to get alarm sync response");
        return "Failed to get alarm sync response";
    }
    cJSON* alarms_array = cJSON_GetObjectItem(root, "data");
    if (!alarms_array || !cJSON_IsArray(alarms_array)) {
        ESP_LOGW(TAG, "No alarms in response or invalid format");
        cJSON_Delete(root);
        return "";
    }
    int alarm_count = cJSON_GetArraySize(alarms_array);
    for (int i = 0; i < alarm_count; i++) {
        cJSON* alarm_json = cJSON_GetArrayItem(alarms_array, i);
        if (!alarm_json) continue;
        cJSON* id_item = cJSON_GetObjectItem(alarm_json, "nvs_id");
        cJSON* time_item = cJSON_GetObjectItem(alarm_json, "time");
        cJSON* repeat_item = cJSON_GetObjectItem(alarm_json, "repeat");
        cJSON* interval_item = cJSON_GetObjectItem(alarm_json, "interval");
        cJSON* name_item = cJSON_GetObjectItem(alarm_json, "name");
        // 校验字段，避免服务端返回异常数据时空指针解引用
        if (!cJSON_IsNumber(id_item) || !cJSON_IsNumber(time_item) ||
            !cJSON_IsNumber(repeat_item) || !cJSON_IsNumber(interval_item) ||
            !cJSON_IsString(name_item) || name_item->valuestring == nullptr) {
            ESP_LOGW(TAG, "Invalid alarm entry at index %d, skipping", i);
            continue;
        }
        Alarm alarm{};
        alarm.id = id_item->valueint;
        alarm.time = time_item->valueint;
        alarm.repeat = repeat_item->valueint;
        alarm.interval = interval_item->valueint;
        alarm.name = std::string(name_item->valuestring);
        // 槽位越界会写坏 NVS 键空间，直接跳过
        if (alarm.id < 0 || alarm.id >= max_alarms_) {
            ESP_LOGW(TAG, "Alarm nvs_id %d out of range, skipping", alarm.id);
            continue;
        }
        // 检查云端下发的闹钟时间是否已过期，如果是重复闹钟则顺延到未来
        time_t now = time(NULL);
        bool time_adjusted = false;
        if (alarm.time <= now) {
            if (alarm.repeat > 0 && alarm.interval > 0) {
                int periods = (now - alarm.time) / alarm.interval + 1;
                alarm.time += periods * alarm.interval;
                time_adjusted = true;
                ESP_LOGI(TAG, "Alarm %s expired, adjusted to future: %s", alarm.name.c_str(), FormatTimestamp(alarm.time).c_str());
            } else {
                ESP_LOGI(TAG, "Alarm %s expired and not repeatable, skipping", alarm.name.c_str());
                continue;
            }
        }
        out_alarms.push_back(alarm);
        // 如果时间被调整过，交由调用方在锁外异步回写云端
        if (time_adjusted) {
            out_pending.emplace_back(alarm, "update");
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Successfully loaded %d alarms from cloud", (int)out_alarms.size());
    return "";
}

std::string AlarmManager::GetRingName() {
    std::lock_guard<std::mutex> lock(mutex_);
    return ring_name_;
}