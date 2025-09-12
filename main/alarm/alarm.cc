#include "alarm.h"
#include "assets/lang_config.h"
#include "board.h"
#include "display.h"
#include "application.h"
#include <ctime>
#define TAG "AlarmManager"

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

    Settings settings_("alarm_clock", true); // 闹钟设置
    // 从Setting里面读取闹钟列表
    for(int i = 0; i < 10; i++){
        std::string alarm_name = settings_.GetString("alarm_" + std::to_string(i));
        if(alarm_name != ""){
            Alarm alarm;
            alarm.id = i; // 闹钟的索引
            alarm.name = alarm_name;
            alarm.time = settings_.GetInt("alarm_time_" + std::to_string(i));
            alarm.repeat = settings_.GetInt("alarm_rpt_" + std::to_string(i));
            alarm.interval = settings_.GetInt("alarm_itv_" + std::to_string(i));
            alarms_.push_back(alarm);
            ESP_LOGI(TAG, "Alarm %d %s add again at %s", alarm.id, alarm.name.c_str(), FormatTimestamp(alarm.time).c_str());
        }
    }

    time_t now = time(NULL);
    // 清除过期的闹钟
    ClearOverdueAlarm(now);
    // 获取最近的闹钟
    Alarm *current_alarm_ = GetProximateAlarm(now);
    if(current_alarm_ != nullptr){
        CreateAndStartTimer(current_alarm_->time - now);
    }
}

AlarmManager::~AlarmManager(){
    if(timer_){
        esp_timer_stop(timer_);
        esp_timer_delete(timer_);
        timer_ = nullptr;
    }
}

void AlarmManager::CreateAndStartTimer(int delay) {
    if (delay <= 0) {
        ESP_LOGW(TAG, "Invalid timer interval: %d sec, skip start", delay);
        return;
    }
    esp_timer_stop(timer_); // 先停止之前的任务
    ESP_LOGI(TAG, "Start timer, trigger in %d sec", delay);
    ESP_ERROR_CHECK(esp_timer_start_once(timer_, delay * 1000000));
}

void AlarmManager::OnAlarm(){
    time_t now = time(NULL);
    // 获取最接近的闹钟
    Alarm *alarm_first = nullptr;
    for(auto& alarm : alarms_){
        if(alarm.time <= now){
            alarm_first = &alarm;
            break;
        }
    }
    if (alarm_first == nullptr) {
        ESP_LOGI(TAG, "OnAlarm triggered but no valid alarm due");
        ring_flag_ = false;
        return;
    }
    // 有闹钟要提醒
    ESP_LOGI(TAG, "=----ring----=");
    ring_flag_ = true;
    // 构造闹钟提醒文本
    ring_name_ = alarm_first->name + " " + FormatTimestamp(alarm_first->time);
    ESP_LOGI(TAG, "Alarm: %s is ringing", alarm_first->name.c_str());
    
    // 清除过期的闹钟
    ClearOverdueAlarm(now);
    // 调度下一个闹钟
    Alarm *next_alarm_ = GetProximateAlarm(now);
    if (next_alarm_ != nullptr){
        CreateAndStartTimer(next_alarm_->time - now);
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
        for (int i = 0; i < 10; i++){
            if(settings_.GetString("alarm_" + std::to_string(i)) == it->name && settings_.GetInt("alarm_time_" + std::to_string(i)) == it->time){
                found = true;
                it->repeat--; // 次数减去1
                if (it->repeat > 0) {
                    it->time += it->interval;
                    settings_.SetInt("alarm_time_" + std::to_string(i), it->time);
                    settings_.SetInt("alarm_rpt_" + std::to_string(i), it->repeat);
                    ESP_LOGI(TAG, "Alarm %s will repeat %d ", it->name.c_str(), it->repeat);
                    ++it;
                } else {
                    settings_.SetString("alarm_" + std::to_string(i), ""); // 名称置为空
                    settings_.SetInt("alarm_time_" + std::to_string(i), 0);
                    settings_.SetInt("alarm_rpt_" + std::to_string(i), 0);
                    settings_.SetInt("alarm_itv_" + std::to_string(i), 0);
                    ESP_LOGI(TAG, "Alarm %s is overdue, repeat %d", it->name.c_str(), it->repeat);
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

std::string AlarmManager::SetAlarm(int delay, int hour, int minute, int repeat, int interval, std::string name){
    std::lock_guard<std::mutex> lock(mutex_);
    if(alarms_.size() >= 10){
        ESP_LOGI(TAG, "Too many alarms");
        return "Too many alarms";
    }
    Settings settings_("alarm_clock", true); // 闹钟设置
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
    // 从设置里面找到第一个空闲的闹钟, 记录新的闹钟
    for(int i = 0; i < 10; i++){
        if(settings_.GetString("alarm_" + std::to_string(i)) == ""){
            alarm.id = i; // 将索引赋值给闹钟的id
            settings_.SetString("alarm_" + std::to_string(i), alarm.name);
            settings_.SetInt("alarm_time_" + std::to_string(i), alarm.time);
            settings_.SetInt("alarm_rpt_" + std::to_string(i), repeat);
            settings_.SetInt("alarm_itv_" + std::to_string(i), interval);
            break;
        }
    }
    alarms_.push_back(alarm); // 全部赋值后再push_back
    ESP_LOGI(TAG, "Alarm %d %s set at %s repeat %d interval %d", alarm.id, alarm.name.c_str(), FormatTimestamp(alarm.time).c_str(), alarm.repeat, alarm.interval);
    // 打印所有闹钟设置用于调试
    ESP_LOGI(TAG, "=== Alarm Settings Debug Info ===");
    for(int i = 0; i < 10; i++){
        std::string name = settings_.GetString("alarm_" + std::to_string(i));
        int time = settings_.GetInt("alarm_time_" + std::to_string(i));
        int repeat = settings_.GetInt("alarm_rpt_" + std::to_string(i));
        int interval = settings_.GetInt("alarm_itv_" + std::to_string(i));
        if(name != ""){
            ESP_LOGI(TAG, "Alarm %d: name=%s, time=%d, repeat=%d, interval=%d", i, name.c_str(), time, repeat, interval);
        }
    }
    Alarm *alarm_first = GetProximateAlarm(now);
    if (alarm_first != nullptr) {
        ESP_LOGI(TAG, "Next Alarm %d %s set at %s repeat %d interval %d", alarm_first->id, alarm_first->name.c_str(), FormatTimestamp(alarm_first->time).c_str(), alarm_first->repeat, alarm_first->interval);
        CreateAndStartTimer(alarm_first->time - now);
    }
    return ""; // 返回空字符串表示成功
}

std::string AlarmManager::DelAlarm(std::string keyword){
    std::lock_guard<std::mutex> lock(mutex_);
    Settings settings_("alarm_clock", true); // 闹钟设置

    if (keyword.empty()) {
        ESP_LOGI(TAG, "Keyword cannot be empty");
        return "Keyword cannot be empty";
    }

    // 从alarms_中删除包含关键词的闹钟
    bool found = false;
    for(auto it = alarms_.begin(); it != alarms_.end(); ++it){
        if(it->name.find(keyword) != std::string::npos){
            ESP_LOGI(TAG, "Alarm %d %s deleted", it->id, it->name.c_str());
            alarms_.erase(it);
            found = true;
            break;
        }
    }
    
    if(!found){
        ESP_LOGW(TAG, "Alarm %s not found", keyword.c_str());
        return "Alarm not found";
    }
    
    // 从设置中删除指定名称的闹钟
    for(int i = 0; i < 10; i++){
        if(settings_.GetString("alarm_" + std::to_string(i)).find(keyword) != std::string::npos){
            settings_.SetString("alarm_" + std::to_string(i), "");
            settings_.SetInt("alarm_time_" + std::to_string(i), 0);
            settings_.SetInt("alarm_rpt_" + std::to_string(i), 0);
            settings_.SetInt("alarm_itv_" + std::to_string(i), 0);
            break;
        }
    }
    
    ESP_LOGI(TAG, "Alarm %s deleted", keyword.c_str());

    // 重新设置下一个闹钟
    time_t now = time(NULL);
    Alarm *current_alarm_ = GetProximateAlarm(now);
    if (current_alarm_ != nullptr) {
        CreateAndStartTimer(current_alarm_->time - now);
    } else {
        ESP_LOGI(TAG, "No more alarms");
    }
    return "";
}

std::string AlarmManager::DelAlarmById(int id){
    std::lock_guard<std::mutex> lock(mutex_);
    Settings settings_("alarm_clock", true); // 闹钟设置

    if (id < 0) {
        ESP_LOGI(TAG, "id should be >= 0");
        return "id should be >= 0";
    }

    // 从alarms_中删除包含关键词的闹钟
    bool found = false;
    for(auto it = alarms_.begin(); it != alarms_.end(); ++it){
        if(it->id == id){
            ESP_LOGI(TAG, "Alarm %d %s deleted", it->id, it->name.c_str());
            it = alarms_.erase(it); // 返回指向被删除元素后一个元素的迭代器
            found = true;
            break;
        }
    }
    
    if(!found){
        ESP_LOGW(TAG, "Alarm %d not found", id);
        return "Alarm not found";
    }
    
    // 从设置中删除指定名称的闹钟
    settings_.SetString("alarm_" + std::to_string(id), "");
    settings_.SetInt("alarm_time_" + std::to_string(id), 0);
    settings_.SetInt("alarm_rpt_" + std::to_string(id), 0);
    settings_.SetInt("alarm_itv_" + std::to_string(id), 0);
    
    ESP_LOGI(TAG, "Alarm %d deleted from nvs", id);

    // 重新设置下一个闹钟
    time_t now = time(NULL);
    Alarm *current_alarm_ = GetProximateAlarm(now);
    if (current_alarm_ != nullptr) {
        CreateAndStartTimer(current_alarm_->time - now);
    } else {
        ESP_LOGI(TAG, "No more alarms");
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
        result += R"("time":)" + FormatTimestamp(alarm.time) + R"(,)";
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

void AlarmManager::CleanUpAlarm() {
    std::lock_guard<std::mutex> lock(mutex_);
    Settings settings_("alarm_clock", true); // 闹钟设置
    // 清空所有闹钟
    alarms_.clear();
    // 清空所有设置
    for(int i = 0; i < 10; i++){
        settings_.SetString("alarm_" + std::to_string(i), "");
        settings_.SetInt("alarm_time_" + std::to_string(i), 0);
        settings_.SetInt("alarm_rpt_" + std::to_string(i), 0);
        settings_.SetInt("alarm_itv_" + std::to_string(i), 0);
    }
    // 停止当前定时器
    esp_timer_stop(timer_);
    ESP_LOGI(TAG, "All alarms cleaned up");
}