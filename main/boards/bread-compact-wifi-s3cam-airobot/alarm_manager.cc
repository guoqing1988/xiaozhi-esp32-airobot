#include "alarm_manager.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

#ifndef ALARM_FILE_PATH
#define ALARM_FILE_PATH "/sdcard/alarms.json"
#endif
#ifndef ALARM_CHECK_INTERVAL_MS
#define ALARM_CHECK_INTERVAL_MS 1000
#endif

static const char* TAG = "AlarmManager";

// 把单个闹钟条目序列化为 cJSON 对象(供 Save/ListJson 复用, 保证 DRY)
static cJSON* AlarmItemToJson(const AlarmItem& a) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", a.id);
    cJSON_AddStringToObject(o, "type", a.type == kAlarmTypeAbsolute ? "absolute" : "relative");
    cJSON_AddNumberToObject(o, "trigger_sec", a.trigger_sec);
    cJSON_AddStringToObject(o, "label", a.label.c_str());
    cJSON_AddBoolToObject(o, "enabled", a.enabled);
    if (a.type == kAlarmTypeAbsolute) {
        cJSON_AddNumberToObject(o, "last_fired_day", a.last_fired_day);
    }
    return o;
}

AlarmManager::AlarmManager(AudioService& audio_service) : audio_service_(audio_service) {}

AlarmManager::~AlarmManager() {
    Stop();
}

void AlarmManager::SetTriggerCallback(std::function<void(const AlarmItem&)> cb) {
    on_trigger_ = std::move(cb);
}

uint64_t AlarmManager::NowMs() {
    return static_cast<uint64_t>(esp_timer_get_time() / 1000);  // 单调时钟(微秒->毫秒)
}

void AlarmManager::Start() {
    Load();
    if (running_.exchange(true)) {
        return;
    }
    check_thread_ = std::thread(&AlarmManager::CheckLoop, this);
}

void AlarmManager::Stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (check_thread_.joinable()) {
        check_thread_.join();
    }
}

void AlarmManager::CheckLoop() {
    while (running_.load()) {
        std::vector<AlarmItem> due;
        int today = static_cast<int>(time(nullptr) / 86400);  // epoch 天序号
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& a : alarms_) {
                if (a.enabled && IsDue(a, today)) {
                    due.push_back(a);
                    if (a.type == kAlarmTypeRelative) {
                        a.enabled = false;   // 一次性: 到点后自动禁用
                    } else {
                        a.last_fired_day = today;  // 每天重复: 记录本次触发的天序号
                    }
                }
            }
        }
        if (!due.empty()) {
            Save();  // Save 内部加锁, 避免在持锁状态下重复加锁(死锁)
        }
        for (auto& a : due) {
            if (on_trigger_) {
                on_trigger_(a);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(ALARM_CHECK_INTERVAL_MS));
    }
}

bool AlarmManager::IsDue(const AlarmItem& a, int today) const {
    if (!a.enabled) {
        return false;
    }
    if (a.type == kAlarmTypeRelative) {
        // 相对: 从创建时刻起算(esp_timer 单调), 重启后由 Load 重新计时
        uint64_t base = a.base_ms;
        if (base == 0) {
            base = NowMs();  // 异常无基准则视为已过 0 秒
        }
        uint64_t elapsed_sec = (NowMs() - base) / 1000;
        return elapsed_sec >= static_cast<uint64_t>(a.trigger_sec);
    } else {
        // 绝对: 依赖系统时钟(联网后 time() 已同步); 达到当天目标时刻且当天未触发过
        time_t now = time(nullptr);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        int now_sec = tm_now.tm_hour * 3600 + tm_now.tm_min * 60 + tm_now.tm_sec;
        return now_sec >= a.trigger_sec && a.last_fired_day != today;
    }
}

int AlarmManager::NextId() {
    return ++last_id_;
}

int AlarmManager::Add(AlarmType type, int trigger_sec, const std::string& label) {
    AlarmItem a;
    a.type = type;
    a.trigger_sec = trigger_sec;
    a.label = label;
    a.enabled = true;
    if (a.type == kAlarmTypeRelative) {
        a.base_ms = NowMs();  // 相对: 记录创建时刻
    } else {
        // 绝对: 若今天的目标时刻已过, 记今天为"已触发", 避免设置后立即响(次日才触发)
        time_t now = time(nullptr);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        int now_sec = tm_now.tm_hour * 3600 + tm_now.tm_min * 60 + tm_now.tm_sec;
        if (now_sec >= trigger_sec) {
            a.last_fired_day = static_cast<int>(now / 86400);
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        a.id = NextId();
        alarms_.push_back(a);
    }
    Save();
    return a.id;
}

bool AlarmManager::Remove(int id) {
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = alarms_.begin(); it != alarms_.end(); ++it) {
            if (it->id == id) {
                alarms_.erase(it);
                ok = true;
                break;
            }
        }
    }
    if (ok) {
        Save();
    }
    return ok;
}

void AlarmManager::ClearAll() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        alarms_.clear();
    }
    Save();
}

std::string AlarmManager::ListJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    cJSON* arr = cJSON_CreateArray();
    for (const auto& a : alarms_) {
        cJSON_AddItemToArray(arr, AlarmItemToJson(a));
    }
    char* s = cJSON_PrintUnformatted(arr);
    std::string res(s);
    cJSON_free(s);
    cJSON_Delete(arr);
    return res;
}

void AlarmManager::Save() const {
    std::lock_guard<std::mutex> lock(mutex_);
    cJSON* arr = cJSON_CreateArray();
    for (const auto& a : alarms_) {
        cJSON_AddItemToArray(arr, AlarmItemToJson(a));
    }
    char* s = cJSON_PrintUnformatted(arr);
    FILE* f = fopen(ALARM_FILE_PATH, "wb");
    if (f != nullptr) {
        fputs(s, f);
        fclose(f);
    } else {
        ESP_LOGE(TAG, "cannot write %s", ALARM_FILE_PATH);
    }
    cJSON_free(s);
    cJSON_Delete(arr);
}

void AlarmManager::Load() {
    FILE* f = fopen(ALARM_FILE_PATH, "rb");
    if (f == nullptr) {
        return;  // 文件不存在(首次)不视为错误
    }
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    cJSON* arr = cJSON_Parse(buf);
    if (!cJSON_IsArray(arr)) {
        if (arr != nullptr) {
            cJSON_Delete(arr);
        }
        return;
    }
    uint64_t now = NowMs();
    std::lock_guard<std::mutex> lock(mutex_);
    alarms_.clear();
    int max_id = 0;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, arr) {
        AlarmItem a;
        cJSON* id = cJSON_GetObjectItem(item, "id");
        cJSON* ty = cJSON_GetObjectItem(item, "type");
        cJSON* tr = cJSON_GetObjectItem(item, "trigger_sec");
        cJSON* lb = cJSON_GetObjectItem(item, "label");
        cJSON* en = cJSON_GetObjectItem(item, "enabled");
        cJSON* lf = cJSON_GetObjectItem(item, "last_fired_day");
        if (id) {
            a.id = id->valueint;
        }
        if (ty && ty->valuestring && strcmp(ty->valuestring, "absolute") == 0) {
            a.type = kAlarmTypeAbsolute;
        }
        if (tr) {
            a.trigger_sec = tr->valueint;
        }
        if (lb && lb->valuestring) {
            a.label = lb->valuestring;
        }
        if (en) {
            a.enabled = cJSON_IsTrue(en);
        }
        if (lf) {
            a.last_fired_day = lf->valueint;
        }
        if (a.type == kAlarmTypeRelative) {
            a.base_ms = now;  // 相对闹钟重启后重新计时(从当前开机时刻起)
        }
        if (a.id > max_id) {
            max_id = a.id;
        }
        alarms_.push_back(a);
    }
    last_id_ = max_id;
    cJSON_Delete(arr);
}
