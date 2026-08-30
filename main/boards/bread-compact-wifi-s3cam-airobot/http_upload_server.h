#pragma once

#include <functional>

// 启动 WiFi 网页上传服务：浏览器打开 http://<设备IP>/ 即可上传 MP3 到 TF 卡 /sdcard/music。
// on_uploaded：每次成功上传后回调（可选），用于刷新歌曲列表缓存。
void StartUploadServer(std::function<void()> on_uploaded = nullptr);
