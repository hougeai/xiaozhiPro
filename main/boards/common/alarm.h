
#ifndef ALARM_H
#define ALARM_H

#include <string>
#include <vector>
#include <utility>
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
    std::string DelAlarmById(int id);
    // 查询所有闹钟
    std::string QueryAllAlarm();
    // 清空所有闹钟
    std::string CleanUpAlarm();
    // 重新加载闹钟
    std::string ReloadAlarms();
    // 在后台任务中异步执行首次 ReloadAlarms（避免构造方被云端同步阻塞）
    void StartReloadTask();
    // 闹钟响了的处理函数（定时器回调）
    void OnAlarm();
    // 闹钟是不是响了的标志位
    bool IsRing(){ return ring_flag_; };
    // 清除闹钟标志位
    void ClearRing();
    // 获取当前闹钟内容
    std::string GetRingName();

private:
    // 清除过时的闹钟（内部自行加锁）
    void ClearOverdueAlarm(time_t now);
    // 获取从现在开始第一个响的闹钟；返回指向 alarms_ 的裸指针，
    // 必须在持锁期间使用，且不得跨越任何会修改 alarms_ 的调用。
    Alarm *GetProximateAlarm(time_t now);
    // 创建定时器（要求调用方已持锁）
    void CreateAndStartTimer(int delay);
    // 与云端同步闹钟数据（阻塞式网络调用，禁止在持锁时调用）
    std::string SyncAlarmToCloud(const Alarm& alarm, const std::string& action);
    std::string ClearCloudAlarms();
    // 仅做网络请求与解析，不修改任何共享状态；结果通过出参返回，由调用方持锁写入。
    // out_pending 为被顺延过、需要回写云端的闹钟。
    std::string SyncAlarmFromCloud(std::vector<Alarm>& out_alarms,
                                   std::vector<std::pair<Alarm, std::string>>& out_pending);
    // 在独立任务中异步同步闹钟到云端（用于定时器上下文，避免阻塞 esp_timer 任务）
    void SyncAlarmsToCloudAsync(std::vector<std::pair<Alarm, std::string>> pending);
    // 调度下一个闹钟；ScheduleNextAlarm 自行加锁，Locked 版本要求调用方已持锁
    void ScheduleNextAlarm(time_t now);
    void ScheduleNextAlarmLocked(time_t now);

    std::vector<Alarm> alarms_; // 闹钟列表
    std::mutex mutex_; // 互斥锁
    esp_timer_handle_t timer_; // 定时器

    std::atomic<bool> ring_flag_{false};
    std::atomic<bool> init_started_{false}; // 防止重复启动后台加载任务
    std::string ring_name_;
    std::string server_url_;
    int max_alarms_;
};
#endif