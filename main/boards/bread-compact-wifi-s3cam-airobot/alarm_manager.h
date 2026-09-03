#ifndef ALARM_MANAGER_H_
#define ALARM_MANAGER_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio_service.h"

// 闹钟类型: relative = 创建后 N 秒触发(一次性); absolute = 每天 HH:MM 触发(依赖系统时间)
enum AlarmType { kAlarmTypeRelative, kAlarmTypeAbsolute };

struct AlarmItem {
    int         id = 0;
    AlarmType   type = kAlarmTypeRelative;
    // relative: 从创建起经过的秒数; absolute: 一天内的秒数(0~86399)
    int         trigger_sec = 0;
    std::string label;        // 提醒内容(如"喝水")
    bool        enabled = true;
    // relative: 创建时的单调毫秒(esp_timer, 用于相对计时, 内存态, 不落盘)
    uint64_t base_ms = 0;
    // absolute: 上次触发的"天"序号(epoch 天数), 避免设置后立即触发/当天重复触发; 落盘
    int last_fired_day = 0;
};

class AlarmManager {
public:
    explicit AlarmManager(AudioService& audio_service);
    ~AlarmManager();

    // 加载 /sdcard/alarms.json, 启动后台检查线程
    void Start();
    // 停止后台检查线程(析构时自动调用)
    void Stop();

    // 增加闹钟(写入文件), 返回新 id
    int Add(AlarmType type, int trigger_sec, const std::string& label);
    // 清除所有闹钟(供 web "清空" 用)
    void ClearAll();
    // 按 id 删除
    bool Remove(int id);
    // 列出所有闹钟(json 数组字符串), 供 MCP/web 用
    std::string ListJson() const;
    // 启动一次性到点回调(触发播报)
    void SetTriggerCallback(std::function<void(const AlarmItem&)> cb);

private:
    void CheckLoop();                            // 后台周期检查
    void Save() const;                           // 写文件
    void Load();                                 // 读文件
    int  NextId();                               // 自增 id
    bool IsDue(const AlarmItem& a, int today) const;  // 相对/绝对到点判断
    static uint64_t NowMs();                     // 单调毫秒(esp_timer)

    AudioService& audio_service_;
    mutable std::mutex mutex_;
    std::vector<AlarmItem> alarms_;
    std::atomic<bool> running_{false};
    std::thread check_thread_;
    std::function<void(const AlarmItem&)> on_trigger_;
    int last_id_ = 0;
};

#endif  // ALARM_MANAGER_H_
