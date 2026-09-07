#include "http_upload_server.h"

#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#define TAG "HttpUpload"

#define MUSIC_DIR "/sdcard/music"

// 上传成功回调（在 StartUploadServer 时注入），用于刷新上层歌曲列表缓存
static std::function<void()> s_on_uploaded;
// 等待 WiFi 就绪的轮询定时器
static esp_timer_handle_t s_wifi_timer = nullptr;
// 闹钟管理回调(由板级 SetAlarmWebApi 注入)
static AlarmWebApi s_alarm_api;
// 机器人控制回调(由板级 SetUnoWebApi 注入)
static UnoWebApi s_uno_api;

void SetAlarmWebApi(const AlarmWebApi& api) { s_alarm_api = api; }
void SetUnoWebApi(const UnoWebApi& api) { s_uno_api = api; }

// URL 解码（%XX -> 字符，+ -> 空格），用于文件名
static void UrlDecode(char* out, size_t out_size, const char* in) {
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 1 < out_size; ++i) {
        if (in[i] == '%' && in[i + 1] != '\0' && in[i + 2] != '\0') {
            char hex[3] = {in[i + 1], in[i + 2], '\0'};
            out[o++] = static_cast<char>(strtol(hex, nullptr, 16));
            i += 2;
        } else if (in[i] == '+') {
            out[o++] = ' ';
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
}

// 文件名安全化：去除路径分隔符，防止路径穿越；仅保留基础文件名
static void SanitizeName(char* name) {
    for (char* p = name; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') {
            *p = '_';
        }
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        name[0] = '\0';
    }
}

// 嵌入的 web 页面(index.html, 由 main/CMakeLists.txt 的 EMBED_FILES 嵌入)
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static esp_err_t HandleIndex(httpd_req_t* req) {
    // 页面内容存于独立文件 main/boards/bread-compact-wifi-s3cam-airobot/web/index.html
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, static_cast<size_t>(index_html_end - index_html_start));
}


static esp_err_t HandleUpload(httpd_req_t* req) {
    // httpd_query_key_value 期望纯 query 字符串（不含路径和 '?'），需从 uri 中提取
    const char* q = strchr(req->uri, '?');
    if (q == nullptr || q[1] == '\0') {
        ESP_LOGE(TAG, "Upload: no query string, uri=%s", req->uri);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing 'name' query parameter");
        return ESP_OK;  // 响应已通过 send_err 发送，返回 OK 避免 httpd 直接关闭 socket
    }
    q++;  // 跳过 '?'

    char name[256] = {};
    if (httpd_query_key_value(q, "name", name, sizeof(name)) != ESP_OK || name[0] == '\0') {
        ESP_LOGE(TAG, "Upload: missing 'name' param, query=%s", q);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing 'name' query parameter");
        return ESP_OK;  // 响应已通过 send_err 发送，返回 OK 避免 httpd 直接关闭 socket
    }
    // 是否覆盖同名文件（默认覆盖；overwrite=0 时不覆盖，已存在则返回 409）
    bool overwrite = true;
    {
        char ov[8] = {};
        if (httpd_query_key_value(q, "overwrite", ov, sizeof(ov)) == ESP_OK) {
            overwrite = (strcmp(ov, "0") != 0);
        }
    }
    UrlDecode(name, sizeof(name), name);
    SanitizeName(name);
    size_t len = strlen(name);
    bool is_mp3 = len >= 4 && strcasecmp(name + len - 4, ".mp3") == 0;
    bool is_lrc = len >= 4 && strcasecmp(name + len - 4, ".lrc") == 0;
    if (!is_mp3 && !is_lrc) {
        ESP_LOGE(TAG, "Upload: rejected non-music file name='%s'", name);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "only .mp3 / .lrc files are allowed");
        return ESP_OK;  // 响应已通过 send_err 发送，返回 OK 避免 httpd 直接关闭 socket
    }

    char path[320];
    snprintf(path, sizeof(path), "%s/%s", MUSIC_DIR, name);

    if (!overwrite) {
        FILE* exist = fopen(path, "rb");
        if (exist != nullptr) {
            fclose(exist);
            ESP_LOGE(TAG, "Upload: %s already exists, overwrite disabled", path);
            httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "file already exists, overwrite disabled");
            return ESP_OK;  // 响应已通过 send_err 发送，返回 OK 避免 httpd 直接关闭 socket
        }
    }
    FILE* f = fopen(path, "wb");
    if (f == nullptr) {
        ESP_LOGE(TAG, "Upload: cannot create %s", path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot create file on SD card");
        return ESP_OK;  // 响应已通过 send_err 发送，返回 OK 避免 httpd 直接关闭 socket
    }

    // 接收缓冲放堆上，减小 httpd 任务栈压力（FATFS 写 SD 本身也需要栈）
    char* buf = static_cast<char*>(malloc(4096));
    if (buf == nullptr) {
        fclose(f);
        ESP_LOGE(TAG, "Upload: malloc failed");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;  // 响应已通过 send_err 发送，返回 OK 避免 httpd 直接关闭 socket
    }
    int total = 0;
    int ret;
    while ((ret = httpd_req_recv(req, buf, 4096)) > 0) {
        if (fwrite(buf, 1, static_cast<size_t>(ret), f) != static_cast<size_t>(ret)) {
            free(buf);
            fclose(f);
            remove(path);
            ESP_LOGE(TAG, "Upload: write failed at %d bytes", total);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write to SD card failed");
            return ESP_OK;  // 响应已通过 send_err 发送，返回 OK 避免 httpd 直接关闭 socket
        }
        total += ret;
    }
    free(buf);
    fclose(f);

    if (ret < 0) {
        remove(path);
        ESP_LOGE(TAG, "Upload: recv interrupted ret=%d after %d bytes", ret, total);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "receive interrupted");
        return ESP_OK;  // 响应已通过 send_err 发送，返回 OK 避免 httpd 直接关闭 socket
    }

    httpd_resp_sendstr(req, "OK");
    if (s_on_uploaded) {
        s_on_uploaded();  // 通知上层刷新歌曲列表缓存
    }
    return ESP_OK;
}

// 列出 /sdcard/music 下所有 .mp3 歌曲(附文件大小字节 + 修改时间/上传时刻)。
// 用标准库 dirent.h 扫描目录 + stat() 取大小/时间 + cJSON 构建响应；中文文件名(UTF-8)直接作为字符串返回。
// 注意: 文件修改时间依赖 FATFS 时间戳(FATFS_TIMESTAMP)与设备同步的系统时间；未启用时 mtime 可能为 0。
static esp_err_t HandleMusicList(httpd_req_t* req) {
    (void)req;
    cJSON* arr = cJSON_CreateArray();
    DIR* dir = opendir(MUSIC_DIR);
    if (dir != nullptr) {
        struct dirent* entry = nullptr;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_type != DT_REG) {
                continue;
            }
            const char* name = entry->d_name;
            size_t len = strlen(name);
            if (len < 4 || strcasecmp(name + len - 4, ".mp3") != 0) {
                continue;  // 只列出歌曲，.lrc 作为同名附属不单独显示
            }
            char path[320];
            snprintf(path, sizeof(path), "%s/%s", MUSIC_DIR, name);
            // 用 stat() 一次性取文件大小与修改时间(上传时刻)，避免再单独 fopen
            struct stat st = {};
            long size = 0;
            long long mtime = 0;
            if (stat(path, &st) == 0) {
                size = static_cast<long>(st.st_size);
                mtime = static_cast<long long>(st.st_mtime);
            }
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "name", name);
            cJSON_AddNumberToObject(item, "size", static_cast<double>(size));
            cJSON_AddNumberToObject(item, "mtime", static_cast<double>(mtime));
            cJSON_AddItemToArray(arr, item);
        }
        closedir(dir);
    }
    char* out = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    std::string body = out ? out : "[]";
    free(out);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body.c_str());
}

// POST /music：JSON body 支持删除歌曲。
//   delete: {"action":"delete","name":"歌曲.mp3"}
// 删除歌曲(.mp3)时自动删除同名 .lrc 歌词; 成功后回调刷新歌曲列表缓存。
static esp_err_t HandleMusicPost(httpd_req_t* req) {
    char buf[1024];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    buf[len] = '\0';

    cJSON* root = cJSON_Parse(buf);
    if (root == nullptr) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_OK;
    }
    cJSON* c_action = cJSON_GetObjectItem(root, "action");
    const char* action = (c_action && c_action->valuestring) ? c_action->valuestring : "";
    std::string resp = "{\"ok\":false,\"error\":\"unknown action\"}";

    if (strcmp(action, "delete") == 0) {
        cJSON* c_name = cJSON_GetObjectItem(root, "name");
        if (c_name && c_name->valuestring && c_name->valuestring[0] != '\0') {
            char name[256] = {};
            snprintf(name, sizeof(name), "%s", c_name->valuestring);
            SanitizeName(name);
            size_t nlen = strlen(name);
            bool is_mp3 = nlen >= 4 && strcasecmp(name + nlen - 4, ".mp3") == 0;
            bool is_lrc = nlen >= 4 && strcasecmp(name + nlen - 4, ".lrc") == 0;
            if (is_mp3 || is_lrc) {
                char path[320];
                snprintf(path, sizeof(path), "%s/%s", MUSIC_DIR, name);
                if (remove(path) == 0) {
                    if (is_mp3) {
                        // 删除歌曲时连带删除同名歌词(如存在)
                        std::string base_name(name, nlen - 4);  // 去掉 .mp3 后缀
                        char lrc[320];
                        snprintf(lrc, sizeof(lrc), "%s/%s.lrc", MUSIC_DIR, base_name.c_str());
                        remove(lrc);
                    }
                    resp = "{\"ok\":true}";
                    if (s_on_uploaded) {
                        s_on_uploaded();  // 通知刷新歌曲列表缓存
                    }
                } else {
                    resp = "{\"ok\":false,\"error\":\"file not found\"}";
                }
            } else {
                resp = "{\"ok\":false,\"error\":\"only .mp3/.lrc allowed\"}";
            }
        } else {
            resp = "{\"ok\":false,\"error\":\"missing name\"}";
        }
    }
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp.c_str());
}

// GET /uno：返回下位机(Arduino)状态 JSON(供前端遥控页面轮询)
static esp_err_t HandleUnoGet(httpd_req_t* req) {
    std::string body = s_uno_api.get_status ? s_uno_api.get_status() : "{}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body.c_str());
}

// POST /uno：JSON body 控制下位机。
//   drive: {"action":"drive","cmd":"forward","speed":200}  进入 web 驾驶(方向+速度)
//   stop:  {"action":"stop"}                                  停止驾驶
//   speed: {"action":"speed","speed":200}                   单独调速度
static esp_err_t HandleUnoPost(httpd_req_t* req) {
    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    buf[len] = '\0';

    cJSON* root = cJSON_Parse(buf);
    if (root == nullptr) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_OK;
    }
    cJSON* c_action = cJSON_GetObjectItem(root, "action");
    const char* action = (c_action && c_action->valuestring) ? c_action->valuestring : "";

    std::string resp = "{\"ok\":false,\"error\":\"unknown action\"}";
    if (s_uno_api.send_drive) {
        if (strcmp(action, "drive") == 0) {
            cJSON* c_cmd = cJSON_GetObjectItem(root, "cmd");
            cJSON* c_speed = cJSON_GetObjectItem(root, "speed");
            const char* cmd = (c_cmd && c_cmd->valuestring) ? c_cmd->valuestring : "";
            int spd = c_speed ? c_speed->valueint : 200;
            if (spd < 70) spd = 70;
            if (spd > 255) spd = 255;
            if (cmd[0] == '\0') {
                resp = "{\"ok\":false,\"error\":\"missing cmd\"}";
            } else {
                std::string dcmd = std::string("drive-") + cmd + "-" + std::to_string(spd);
                resp = std::string("{\"ok\":true,\"msg\":\"") + s_uno_api.send_drive(dcmd) + "\"}";
            }
        } else if (strcmp(action, "stop") == 0) {
            resp = std::string("{\"ok\":true,\"msg\":\"") + s_uno_api.send_drive("drive-stop") + "\"}";
        } else if (strcmp(action, "speed") == 0) {
            cJSON* c_speed = cJSON_GetObjectItem(root, "speed");
            int spd = c_speed ? c_speed->valueint : 200;
            if (spd < 70) spd = 70;
            if (spd > 255) spd = 255;
            resp = std::string("{\"ok\":true,\"msg\":\"") +
                   s_uno_api.send_drive(std::string("speed-") + std::to_string(spd)) + "\"}";
        } else if (strcmp(action, "servo") == 0) {
            // 舵机(头部)角度控制 0~180
            cJSON* c_degree = cJSON_GetObjectItem(root, "degree");
            int deg = c_degree ? c_degree->valueint : 90;
            if (deg < 0) deg = 0;
            if (deg > 180) deg = 180;
            resp = std::string("{\"ok\":true,\"msg\":\"") +
                   s_uno_api.send_drive(std::string("servo-") + std::to_string(deg)) + "\"}";
        } else if (strcmp(action, "servo-home") == 0) {
            // 头部舵机回正(回到当前回正角度)
            resp = std::string("{\"ok\":true,\"msg\":\"") + s_uno_api.send_drive("servo-home") + "\"}";
        } else if (strcmp(action, "servo-home-set") == 0) {
            // 设置回正角度(0-180), 存储到 NVS
            cJSON* c_value = cJSON_GetObjectItem(root, "value");
            int val = c_value ? c_value->valueint : 82;
            if (val < 0) val = 0;
            if (val > 180) val = 180;
            resp = s_uno_api.set_servo_home ? s_uno_api.set_servo_home(val)
                                            : std::string("{\"ok\":false,\"error\":\"unavailable\"}");
        } else if (strcmp(action, "servo-home-get") == 0) {
            // 读取已保存的回正角度
            resp = s_uno_api.get_servo_home ? s_uno_api.get_servo_home()
                                            : std::string("{\"value\":82}");
        }
    }
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp.c_str());
}

// GET /alarm?action=list：返回闹钟 JSON 数组(供网页/外部读取)
static esp_err_t HandleAlarmGet(httpd_req_t* req) {
    const char* q = strchr(req->uri, '?');
    char action[16] = {};
    if (q != nullptr) {
        q++;  // 跳过 '?'
        httpd_query_key_value(q, "action", action, sizeof(action));
    }
    std::string body;
    if (strcmp(action, "list") == 0 && s_alarm_api.get_alarms_json) {
        body = s_alarm_api.get_alarms_json();
    } else {
        body = "{\"error\":\"unknown action\"}";
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body.c_str());
}

// POST /alarm：JSON body 支持 add / remove。
//   add:    {"action":"add","type":"relative|absolute","value":<秒>,"label":"..."}
//   remove: {"action":"remove","id":<编号>}
static esp_err_t HandleAlarmPost(httpd_req_t* req) {
    char buf[1024];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;  // 响应已通过 send_err 发送, 返回 OK 避免 httpd 直接关闭 socket
    }
    buf[len] = '\0';

    cJSON* root = cJSON_Parse(buf);
    if (root == nullptr) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_OK;
    }
    cJSON* c_action = cJSON_GetObjectItem(root, "action");
    const char* action = (c_action && c_action->valuestring) ? c_action->valuestring : "";

    std::string resp;
    if (strcmp(action, "add") == 0 && s_alarm_api.add_alarm) {
        cJSON* c_type = cJSON_GetObjectItem(root, "type");
        const char* ty = (c_type && c_type->valuestring) ? c_type->valuestring : "relative";
        cJSON* c_val = cJSON_GetObjectItem(root, "value");
        int val = c_val ? c_val->valueint : 0;
        cJSON* c_label = cJSON_GetObjectItem(root, "label");
        const char* lb = (c_label && c_label->valuestring) ? c_label->valuestring : "";
        cJSON* c_song = cJSON_GetObjectItem(root, "song");
        const char* sg = (c_song && c_song->valuestring) ? c_song->valuestring : "";
        int id = s_alarm_api.add_alarm(ty, val, lb, sg);
        resp = std::string("{\"id\":") + std::to_string(id) + "}";
    } else if (strcmp(action, "remove") == 0 && s_alarm_api.remove_alarm) {
        cJSON* c_id = cJSON_GetObjectItem(root, "id");
        int id = c_id ? c_id->valueint : -1;
        bool ok = s_alarm_api.remove_alarm(id);
        resp = ok ? "{\"ok\":true}" : "{\"ok\":false}";
    } else {
        resp = "{\"error\":\"unknown action\"}";
    }
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp.c_str());
}

static void StartHttpServer() {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.stack_size = 8192;  // 上传写 SD 卡需要较大栈(FATFS)，默认 4096 会栈溢出导致重启
    cfg.task_priority = 6;  // 低于音频输入任务(prio 8)，避免上传/访问时抢占 AI 音频
    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start http server (port 80 may be busy)");
        return;
    }

    httpd_uri_t index_uri = {
        .uri = "/", .method = HTTP_GET, .handler = HandleIndex, .user_ctx = nullptr,
    };
    httpd_uri_t upload_uri = {
        .uri = "/upload", .method = HTTP_POST, .handler = HandleUpload, .user_ctx = nullptr,
    };
    httpd_uri_t music_get_uri = {
        .uri = "/music", .method = HTTP_GET, .handler = HandleMusicList, .user_ctx = nullptr,
    };
    httpd_uri_t music_post_uri = {
        .uri = "/music", .method = HTTP_POST, .handler = HandleMusicPost, .user_ctx = nullptr,
    };
    httpd_uri_t uno_get_uri = {
        .uri = "/uno", .method = HTTP_GET, .handler = HandleUnoGet, .user_ctx = nullptr,
    };
    httpd_uri_t uno_post_uri = {
        .uri = "/uno", .method = HTTP_POST, .handler = HandleUnoPost, .user_ctx = nullptr,
    };
    httpd_uri_t alarm_get_uri = {
        .uri = "/alarm", .method = HTTP_GET, .handler = HandleAlarmGet, .user_ctx = nullptr,
    };
    httpd_uri_t alarm_post_uri = {
        .uri = "/alarm", .method = HTTP_POST, .handler = HandleAlarmPost, .user_ctx = nullptr,
    };
    if (httpd_register_uri_handler(server, &index_uri) != ESP_OK ||
        httpd_register_uri_handler(server, &upload_uri) != ESP_OK ||
        httpd_register_uri_handler(server, &music_get_uri) != ESP_OK ||
        httpd_register_uri_handler(server, &music_post_uri) != ESP_OK ||
        httpd_register_uri_handler(server, &uno_get_uri) != ESP_OK ||
        httpd_register_uri_handler(server, &uno_post_uri) != ESP_OK ||
        httpd_register_uri_handler(server, &alarm_get_uri) != ESP_OK ||
        httpd_register_uri_handler(server, &alarm_post_uri) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register uri handlers");
        return;
    }

    ESP_LOGI(TAG, "Upload server started: http://<device-ip>/ (upload MP3 to %s)", MUSIC_DIR);
}

// WiFi STA 是否已拿到 IP（lwIP 栈就绪后才可创建 socket）
static bool WifiIpReady() {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == nullptr) {
        return false;
    }
    esp_netif_ip_info_t ip = {};
    if (esp_netif_get_ip_info(netif, &ip) != ESP_OK) {
        return false;
    }
    return ip.ip.addr != 0;
}

// 轮询到 WiFi IP 就绪后真正启动 http server（esp_timer 回调运行在任务上下文，可安全调用 httpd_start）
static void OnWifiReadyTimer(void* arg) {
    if (!WifiIpReady()) {
        return;
    }
    if (s_wifi_timer != nullptr) {
        esp_timer_stop(s_wifi_timer);
        esp_timer_delete(s_wifi_timer);
        s_wifi_timer = nullptr;
    }
    ESP_LOGI(TAG, "WiFi IP ready, starting upload server");
    StartHttpServer();
}

void StartUploadServer(std::function<void()> on_uploaded) {
    s_on_uploaded = std::move(on_uploaded);
    if (WifiIpReady()) {
        StartHttpServer();
        return;
    }
    // WiFi 未就绪（板子构造函数阶段，lwIP 栈还未启动）：轮询等待，避免在 tcpip 栈初始化前
    // 创建 socket 导致 assert(tcpip_send_msg_wait_sem Invalid mbox) 重启循环。
    esp_timer_create_args_t args = {
        .callback = OnWifiReadyTimer,
        .arg = nullptr,
        .name = "upload_wifi_wait",
    };
    if (esp_timer_create(&args, &s_wifi_timer) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create wifi wait timer");
        return;
    }
    esp_timer_start_periodic(s_wifi_timer, 1000000);  // 每秒检查一次
}
