
#ifndef ALARM_H
#define ALARM_H

#include <string>
#include <vector>
#include <esp_log.h>
#include <esp_timer.h>
#include "time.h"
#include <mutex>
#include "settings.h"
#include <atomic>

struct Alarm {
    int id;          // 提醒ID
    time_t time;        // 开始提醒时间
    int repeat;   // 重复次数，0表示不重复，大于0表示重复次数
    int interval; // 提醒间隔（秒）
    std::string name; // 提醒内容
};

class AlarmManager {
public:
    AlarmManager();
    ~AlarmManager();

    // 设置闹钟
    std::string SetAlarm(int delay, int hour, int minute, int repeat, int interval, std::string name);
    // 删除闹钟
    std::string DelAlarm(std::string keyword);
    std::string DelAlarmById(int id);
    // 查询所有闹钟
    std::string QueryAllAlarm();
    // 清空所有闹钟
    std::string CleanUpAlarm();
    // 清除过时的闹钟
    void ClearOverdueAlarm(time_t now);
    // 获取从现在开始第一个响的闹钟
    Alarm *GetProximateAlarm(time_t now);
    // 创建定时器
    void CreateAndStartTimer(int delay);
    // 闹钟响了的处理函数
    void OnAlarm();
    // 闹钟是不是响了的标志位
    bool IsRing(){ return ring_flag_; };
    // 清除闹钟标志位
    void ClearRing();
    // 获取当前闹钟内容
    std::string GetRingName();
    // 与云端同步闹钟数据
    std::string SyncAlarmToCloud(const std::vector<Alarm>& alarms, const std::string& action);
    std::string SyncAlarmFromCloud();
private:
    std::vector<Alarm> alarms_; // 闹钟列表
    std::mutex mutex_; // 互斥锁
    esp_timer_handle_t timer_; // 定时器

    std::atomic<bool> ring_flag_{false}; 
    std::string ring_name_;
    std::string server_url_;
    int max_alarms_;
};
#endif