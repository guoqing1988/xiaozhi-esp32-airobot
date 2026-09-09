#pragma once

#include <functional>
#include <string>

// 启动 WiFi 网页上传服务：浏览器打开 http://<设备IP>/ 即可上传 MP3 到 TF 卡 /sdcard/music。
// on_uploaded：每次成功上传后回调（可选），用于刷新歌曲列表缓存。
void StartUploadServer(std::function<void()> on_uploaded = nullptr);

// 闹钟管理回调：由板级注入，供 web 页面读写闹钟。
// get_alarms_json: 返回闹钟 JSON 数组字符串（供列表渲染）。
// add_alarm: 添加闹钟(type, value_sec, label, song)，song 为指定铃声歌曲名(空=随机)，返回新闹钟 id。
// remove_alarm: 按 id 删除闹钟，返回是否成功。
struct AlarmWebApi {
    std::function<std::string()> get_alarms_json;
    std::function<int(const std::string& type, int value_sec, const std::string& label,
                      const std::string& song)> add_alarm;
    std::function<bool(int id)> remove_alarm;
};

// 注入闹钟管理回调；之后 /alarm REST 接口（GET list / POST add/remove/clear）即可读写闹钟。
void SetAlarmWebApi(const AlarmWebApi& api);

// 机器人(Arduino 下位机)控制回调：由板级注入，供 web 摇杆页面连续控制。
// send_drive: 发送一条驾驶指令(cmd 如 "drive-forward-180"/"drive-stop")，返回描述性文本(成功/失败)。
// get_status: 返回下位机状态 JSON 字符串(供前端查询 mode/action/speed/servo)。
// on_client_change: web 控制页 WS 连接数变化时回调(0=无连接)，板级据此在遥控期间保持
//                   WiFi 性能模式，避免待机态省电导致 WS 帧等 DTIM beacon(几百毫秒延迟)。
struct UnoWebApi {
    std::function<std::string(const std::string& cmd)> send_drive;
    std::function<std::string()> get_status;
    std::function<std::string(int value)> set_servo_home;   // 存 NVS + 下发回正角度, 返回说明
    std::function<std::string()> get_servo_home;            // 返回当前回正角度 JSON
    std::function<void(int count)> on_client_change;        // WS 连接数变化通知(可空)
};

// 注入机器人控制回调；之后 /uno REST 接口（GET status / POST drive/stop/speed）即可控制下位机。
void SetUnoWebApi(const UnoWebApi& api);

// 下位机状态变化时调用：把当前 uno 状态 JSON 经 WebSocket 推送给已连接的 web 前端。
// 由板级在解析到 @busy/@done/@stat(状态变化)时调用；无 WS 连接时为安全的空操作。
void WebNotifyUnoStatus();
