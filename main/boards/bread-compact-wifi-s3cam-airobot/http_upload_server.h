#pragma once

#include <functional>
#include <string>

// 启动 WiFi 网页上传服务：浏览器打开 http://<设备IP>/ 即可上传 MP3 到 TF 卡 /sdcard/music。
// on_uploaded：每次成功上传后回调（可选），用于刷新歌曲列表缓存。
void StartUploadServer(std::function<void()> on_uploaded = nullptr);

// 闹钟管理回调：由板级注入，供 web 页面读写闹钟。
// get_alarms_json: 返回闹钟 JSON 数组字符串（供列表渲染）。
// add_alarm: 添加闹钟(type, value_sec, label)，返回新闹钟 id。
// remove_alarm: 按 id 删除闹钟，返回是否成功。
struct AlarmWebApi {
    std::function<std::string()> get_alarms_json;
    std::function<int(const std::string& type, int value_sec, const std::string& label)> add_alarm;
    std::function<bool(int id)> remove_alarm;
};

// 注入闹钟管理回调；之后 /alarm REST 接口（GET list / POST add/remove/clear）即可读写闹钟。
void SetAlarmWebApi(const AlarmWebApi& api);
