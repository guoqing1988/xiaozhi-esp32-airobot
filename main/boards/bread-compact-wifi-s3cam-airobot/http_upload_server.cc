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
#include <memory>
#include <mutex>
#include <strings.h>
#include <unistd.h>   // close(): httpd close_fn 替代默认 close(fd), 必须自己关闭 socket
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

// WebSocket 活动会话(供服务端主动推送状态)。
// 支持多客户端: 用互斥锁保护 fd 登记表; 未启用 CONFIG_HTTPD_WS_SUPPORT 时不编译。
#if CONFIG_HTTPD_WS_SUPPORT
#define MAX_WS_CLIENTS 4
static std::mutex s_ws_mtx;
static httpd_handle_t s_ws_hd = nullptr;
static int s_ws_fds[MAX_WS_CLIENTS];
static int s_ws_count = 0;
#endif

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

// 允许跨域访问: 本地页面/小程序(开发工具)通过 IP 直连本接口时, 由浏览器做 CORS 校验。
// 简单请求(GET/POST multipart)只需 Access-Control-Allow-Origin; 复杂请求(POST JSON/自定义头)
// 会先发 OPTIONS 预检, 故统一加跨域响应头并注册 OPTIONS 预检 handler。
static void SetCors(httpd_req_t* req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, X-Requested-With");
}

// OPTIONS 预检 handler: 各 URI 共用, 返回 204 + 跨域头。
static esp_err_t HandleOptions(httpd_req_t* req) {
    SetCors(req);
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_hdr(req, "Content-Length", "0");
    return httpd_resp_send(req, "", 0);
}

// 判断字符串是否以指定后缀结尾(不区分大小写), 用于 .mp3/.lrc 文件名判断。
static bool HasSuffix(const char* s, const char* ext) {
    size_t sl = strlen(s), el = strlen(ext);
    return sl >= el && strcasecmp(s + sl - el, ext) == 0;
}

// 统一发送 application/json 响应。
static esp_err_t SendJson(httpd_req_t* req, const std::string& body) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body.c_str());
}

// 读取 POST 请求 body 到 buf(并置 \0); 空 body 时已发送 400 并返回 false。
static bool ReadBody(httpd_req_t* req, char* buf, size_t size) {
    int len = httpd_req_recv(req, buf, size - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return false;
    }
    buf[len] = '\0';
    return true;
}

// 构建 "<prefix>-<value>" 形式的下位机命令串, 如 speed-200 / servo-82。
static std::string MakeCmd(const char* prefix, int value) {
    return std::string(prefix) + "-" + std::to_string(value);
}

// 构建三段驾驶命令串 "drive-<cmd>-<speed>"。
static std::string MakeDriveCmd(const char* cmd, int spd) {
    return std::string("drive-") + cmd + "-" + std::to_string(spd);
}

static esp_err_t HandleIndex(httpd_req_t* req) {
    SetCors(req);
    // 页面内容存于独立文件 main/boards/bread-compact-wifi-s3cam-airobot/web/index.html
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, static_cast<size_t>(index_html_end - index_html_start));
}


static esp_err_t HandleUpload(httpd_req_t* req) {
    SetCors(req);
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
    bool is_mp3 = HasSuffix(name, ".mp3");
    bool is_lrc = HasSuffix(name, ".lrc");
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

// 处理歌曲删除等操作(纯函数)。定义在 HandleMusicPost 之后, 故先前置声明。
static std::string MusicActionJson(const char* body);

// 生成 /sdcard/music 下所有 .mp3 歌曲的 JSON 数组字符串(含大小/修改时间)。
// 纯函数(不依赖 httpd_req)，供 HTTP GET /music 与 WebSocket music_list 共用。
static std::string MusicListJson() {
    cJSON* arr = cJSON_CreateArray();
    DIR* dir = opendir(MUSIC_DIR);
    if (dir != nullptr) {
        struct dirent* entry = nullptr;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_type != DT_REG) {
                continue;
            }
            const char* name = entry->d_name;
            if (!HasSuffix(name, ".mp3")) {
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
    return body;
}

// 列出 /sdcard/music 下所有 .mp3 歌曲(附文件大小字节 + 修改时间/上传时刻)。
// 用标准库 dirent.h 扫描目录 + stat() 取大小/时间 + cJSON 构建响应；中文文件名(UTF-8)直接作为字符串返回。
// 注意: 文件修改时间依赖 FATFS 时间戳(FATFS_TIMESTAMP)与设备同步的系统时间；未启用时 mtime 可能为 0。
static esp_err_t HandleMusicList(httpd_req_t* req) {
    SetCors(req);
    std::string body = MusicListJson();
    return SendJson(req, body);
}

// POST /music：JSON body 支持删除歌曲。
//   delete: {"action":"delete","name":"歌曲.mp3"}
// 删除歌曲(.mp3)时自动删除同名 .lrc 歌词; 成功后回调刷新歌曲列表缓存。
static esp_err_t HandleMusicPost(httpd_req_t* req) {
    SetCors(req);
    char buf[1024];
    if (!ReadBody(req, buf, sizeof(buf))) return ESP_OK;
    std::string resp = MusicActionJson(buf);
    return SendJson(req, resp);
}

// 处理歌曲删除等操作(纯函数，不依赖 httpd_req)，供 HTTP POST /music 与 WebSocket music_delete 共用。
// 入参 body 为 JSON 字符串；返回响应 JSON 字符串。
static std::string MusicActionJson(const char* body) {
    cJSON* root = cJSON_Parse(body);
    if (root == nullptr) {
        return "{\"ok\":false,\"error\":\"bad json\"}";
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
            bool is_mp3 = HasSuffix(name, ".mp3");
            bool is_lrc = HasSuffix(name, ".lrc");
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
    return resp;
}

// GET /uno：返回下位机(Arduino)状态 JSON(供前端遥控页面轮询)
static esp_err_t HandleUnoGet(httpd_req_t* req) {
    SetCors(req);
    std::string body = s_uno_api.get_status ? s_uno_api.get_status() : "{}";
    return SendJson(req, body);
}

// POST /uno：JSON body 控制下位机。
//   drive: {"action":"drive","cmd":"forward","speed":200}  进入 web 驾驶(方向+速度)
//   stop:  {"action":"stop"}                                  停止驾驶
//   speed: {"action":"speed","speed":200}                   单独调速度
static esp_err_t HandleUnoPost(httpd_req_t* req) {
    SetCors(req);
    char buf[256];
    if (!ReadBody(req, buf, sizeof(buf))) return ESP_OK;

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
                std::string dcmd = MakeDriveCmd(cmd, spd);
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
                   s_uno_api.send_drive(MakeCmd("speed", spd)) + "\"}";
        } else if (strcmp(action, "servo") == 0) {
            // 舵机(头部)角度控制 0~180
            cJSON* c_degree = cJSON_GetObjectItem(root, "degree");
            int deg = c_degree ? c_degree->valueint : 90;
            if (deg < 0) deg = 0;
            if (deg > 180) deg = 180;
            resp = std::string("{\"ok\":true,\"msg\":\"") +
                   s_uno_api.send_drive(MakeCmd("servo", deg)) + "\"}";
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

    return SendJson(req, resp);
}

#if CONFIG_HTTPD_WS_SUPPORT
// ==================================================================
// WebSocket 控制通道 (/ws)
//   用官方 esp_http_server 内建 WebSocket API(httpd_ws_*): 一条长连接承载
//   uno 控制/状态、音乐列表/删除、闹钟读写等所有 JSON 消息，替代高频 HTTP
//   短连接，避免每请求一连接占满 LWIP socket 池(抢走小智 UDP 音频通道)。
// ==================================================================

// 主动推送状态：由 httpd_ws_send_frame_async 发送(需在 httpd 上下文执行, 故用 httpd_queue_work)。
// 入参字符串需保持存活直至 work 函数执行完(这里用 std::string 挂在 arg 上, 由 work 释放)。
// 入参随 httpd_queue_work 挂到 arg 上, 由 work 释放; hd/fd 用登记时的快照, 避免 work 执行时读全局被并发修改。
struct WsPushArg { std::string data; httpd_handle_t hd; int fd; };
static void WsPushWork(void* arg) {
    std::unique_ptr<WsPushArg> a(static_cast<WsPushArg*>(arg));
    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = reinterpret_cast<uint8_t*>(&a->data[0]);
    frame.len = a->data.size();
    // 该 API 实际为同步发送(内部直接 sess->send_fn), 且是按 (handle, fd) 发送的唯一接口,
    // 故经 httpd_queue_work 调度到 httpd 上下文后用它; payload 在发送完成后才释放, 安全。
    httpd_ws_send_frame_async(a->hd, a->fd, &frame);
}

// 向已连接的所有 WS 客户端广播一条文本消息(板级任意任务可调用)。
// 在锁内取 (handle, fd) 快照后再在锁外排队, 避免 work 里读共享会话表的数据竞争; 并支持多客户端。
static void WsPush(const std::string& data) {
    httpd_handle_t hd; int fds[MAX_WS_CLIENTS]; int n;
    {
        std::lock_guard<std::mutex> lk(s_ws_mtx);
        hd = s_ws_hd; n = s_ws_count;
        for (int i = 0; i < n; ++i) fds[i] = s_ws_fds[i];
    }
    if (hd == nullptr || n == 0) return;
    for (int i = 0; i < n; ++i) {
        auto* arg = new WsPushArg{data, hd, fds[i]};
        if (httpd_queue_work(hd, WsPushWork, arg) != ESP_OK) delete arg;
    }
}

// 板级状态变化时由回调触发：把当前 uno 状态 JSON 推给前端(替代 1500ms 轮询)。
static void WsPushStatus() {
    if (s_uno_api.get_status) WsPush(s_uno_api.get_status());
}

// 对外接口：板级在状态变化时调用，把当前 uno 状态推给已连接的 web 前端(无连接则空操作)。
void WebNotifyUnoStatus() { WsPushStatus(); }

// 处理一条来自前端的 JSON 消息(action)并返回响应 JSON 字符串。
static std::string WsHandleMessage(const char* body) {
    cJSON* root = cJSON_Parse(body);
    if (root == nullptr) return "{\"ok\":false,\"error\":\"bad json\"}";
    cJSON* c_action = cJSON_GetObjectItem(root, "action");
    const char* action = (c_action && c_action->valuestring) ? c_action->valuestring : "";
    // 前端请求可带可选 id, 服务端原样透传到响应, 便于前端精确匹配请求-回执; 无 id 则不带(如状态推送)。
    cJSON* c_id = cJSON_GetObjectItem(root, "id");
    int req_id = (c_id && c_id->valuestring) ? atoi(c_id->valuestring) : (c_id ? c_id->valueint : -9999);
    std::string resp = "{\"ok\":false,\"error\":\"unknown action\"}";

    if (strcmp(action, "uno_status") == 0) {
        if (s_uno_api.get_status) resp = s_uno_api.get_status();
    } else if (strcmp(action, "uno_drive") == 0) {
        cJSON* c_cmd = cJSON_GetObjectItem(root, "cmd");
        cJSON* c_speed = cJSON_GetObjectItem(root, "speed");
        const char* cmd = (c_cmd && c_cmd->valuestring) ? c_cmd->valuestring : "";
        int spd = c_speed ? c_speed->valueint : 200;
        if (spd < 70) spd = 70;
        if (spd > 255) spd = 255;
        if (cmd[0] == '\0') { resp = "{\"ok\":false,\"error\":\"missing cmd\"}"; }
        else if (s_uno_api.send_drive) {
            std::string dcmd = MakeDriveCmd(cmd, spd);
            resp = std::string("{\"ok\":true,\"msg\":\"") + s_uno_api.send_drive(dcmd) + "\"}";
        }
    } else if (strcmp(action, "uno_stop") == 0) {
        if (s_uno_api.send_drive) resp = std::string("{\"ok\":true,\"msg\":\"") + s_uno_api.send_drive("drive-stop") + "\"}";
    } else if (strcmp(action, "uno_speed") == 0) {
        cJSON* c_speed = cJSON_GetObjectItem(root, "speed");
        int spd = c_speed ? c_speed->valueint : 200;
        if (spd < 70) spd = 70;
        if (spd > 255) spd = 255;
        if (s_uno_api.send_drive) resp = std::string("{\"ok\":true,\"msg\":\"") +
            s_uno_api.send_drive(MakeCmd("speed", spd)) + "\"}";
    } else if (strcmp(action, "uno_servo") == 0) {
        cJSON* c_degree = cJSON_GetObjectItem(root, "degree");
        int deg = c_degree ? c_degree->valueint : 90;
        if (deg < 0) deg = 0;
        if (deg > 180) deg = 180;
        if (s_uno_api.send_drive) resp = std::string("{\"ok\":true,\"msg\":\"") +
            s_uno_api.send_drive(MakeCmd("servo", deg)) + "\"}";
    } else if (strcmp(action, "uno_servo_home") == 0) {
        if (s_uno_api.send_drive) resp = std::string("{\"ok\":true,\"msg\":\"") + s_uno_api.send_drive("servo-home") + "\"}";
    } else if (strcmp(action, "uno_servo_home_set") == 0) {
        cJSON* c_value = cJSON_GetObjectItem(root, "value");
        int val = c_value ? c_value->valueint : 82;
        if (val < 0) val = 0;
        if (val > 180) val = 180;
        resp = s_uno_api.set_servo_home ? s_uno_api.set_servo_home(val)
                                        : std::string("{\"ok\":false,\"error\":\"unavailable\"}");
    } else if (strcmp(action, "uno_servo_home_get") == 0) {
        resp = s_uno_api.get_servo_home ? s_uno_api.get_servo_home() : std::string("{\"value\":82}");
    } else if (strcmp(action, "music_list") == 0) {
        resp = MusicListJson();
    } else if (strcmp(action, "music_delete") == 0) {
        cJSON* del = cJSON_CreateObject();
        cJSON_AddStringToObject(del, "action", "delete");
        cJSON* c_name = cJSON_GetObjectItem(root, "name");
        if (c_name && c_name->valuestring) cJSON_AddStringToObject(del, "name", c_name->valuestring);
        char* dstr = cJSON_PrintUnformatted(del);
        cJSON_Delete(del);
        if (dstr) { resp = MusicActionJson(dstr); free(dstr); }
    } else if (strcmp(action, "alarm_list") == 0) {
        if (s_alarm_api.get_alarms_json) resp = s_alarm_api.get_alarms_json();
    } else if (strcmp(action, "alarm_add") == 0) {
        cJSON* c_type = cJSON_GetObjectItem(root, "type");
        cJSON* c_val = cJSON_GetObjectItem(root, "value");
        cJSON* c_label = cJSON_GetObjectItem(root, "label");
        cJSON* c_song = cJSON_GetObjectItem(root, "song");
        const char* ty = (c_type && c_type->valuestring) ? c_type->valuestring : "relative";
        int val = c_val ? c_val->valueint : 0;
        const char* lb = (c_label && c_label->valuestring) ? c_label->valuestring : "";
        const char* sg = (c_song && c_song->valuestring) ? c_song->valuestring : "";
        if (s_alarm_api.add_alarm) resp = std::string("{\"id\":") + std::to_string(s_alarm_api.add_alarm(ty, val, lb, sg)) + "}";
    } else if (strcmp(action, "alarm_remove") == 0) {
        cJSON* c_id = cJSON_GetObjectItem(root, "id");
        int id = c_id ? c_id->valueint : -1;
        if (s_alarm_api.remove_alarm) resp = s_alarm_api.remove_alarm(id) ? "{\"ok\":true}" : "{\"ok\":false}";
    }
    cJSON_Delete(root);
    // 若请求带 id, 将 id 注入到响应 JSON 中, 便于前端精确匹配请求-回执。
    // 注意: 数组类响应(music_list/alarm_list)不能直接 AddNumberToObject —— cJSON 会把 id
    // 当作数组元素追加到末尾(得到 [...,id]), 既污染数据又丢失顶层 id 字段, 前端无法匹配回执。
    // 故数组响应应包装成对象 {"data": <数组>, "id": <id>}; 对象响应则在顶层加 id 字段。
    if (req_id > -9999) {
        cJSON* rj = cJSON_Parse(resp.c_str());
        if (rj) {
            char* s = nullptr;
            if (cJSON_IsArray(rj)) {
                cJSON* wrapper = cJSON_CreateObject();
                if (wrapper != nullptr) {
                    cJSON_AddItemToObject(wrapper, "data", rj);  // rj 转移给 wrapper
                    cJSON_AddNumberToObject(wrapper, "id", req_id);
                    s = cJSON_PrintUnformatted(wrapper);
                    cJSON_Delete(wrapper);  // rj 已转移所有权, 不再单独删除
                } else {
                    cJSON_Delete(rj);  // 分配失败: 放弃注入, 保持原数组响应
                }
            } else {
                cJSON_AddNumberToObject(rj, "id", req_id);
                s = cJSON_PrintUnformatted(rj);
                cJSON_Delete(rj);
            }
            if (s) { resp = s; free(s); }
        }
    }
    return resp;
}

// WebSocket handler(官方推荐写法, 参考 IDF ws_echo_server 示例)。
// 通知板级当前 WS 连接数(仅在数量变化时通知, 避免每个 HTTP 短连接关闭都切一次 WiFi 模式)。
static void NotifyWsClientCount(int count) {
    static int s_last_count = -1;
    if (s_last_count == count) return;
    s_last_count = count;
    if (s_uno_api.on_client_change) s_uno_api.on_client_change(count);
}

// 从 WS 会话登记表移除 fd 并通知板级(供 CLOSE 帧与 httpd close_fn 共用)。
static void WsUnregisterClient(int fd) {
    int count;
    {
        std::lock_guard<std::mutex> lk(s_ws_mtx);
        for (int i = 0; i < s_ws_count; ++i) {
            if (s_ws_fds[i] == fd) { s_ws_fds[i] = s_ws_fds[--s_ws_count]; break; }
        }
        if (s_ws_count == 0) s_ws_hd = nullptr;
        count = s_ws_count;
    }
    NotifyWsClientCount(count);   // 锁外通知, 避免回调里再取锁造成嵌套
}

// httpd 会话关闭回调: 兜底 CLOSE 帧丢失的情况(浏览器崩溃/网络中断/会话超时)。
// 若不清理, fd 会残留在登记表里: 状态推送发向死连接, 且 web_control_active_ 永远为真
// (WiFi 停在性能模式不再省电)。非 WS 会话的 fd 不在表中, 查找不到即空操作。
//
// ⚠ 必须自己 close(sockfd): IDF 的 httpd_sess_delete() 是
//     `if (close_fn) close_fn(hd, fd); else close(fd);`
//   —— close_fn 是"替代"默认关闭动作, 不是"通知"。漏掉 close() 会导致每个 HTTP
//   请求泄漏一个 fd, 累积到 CONFIG_LWIP_MAX_SOCKETS 后新连接全部失败,
//   表现为 web 页面与小智 AI 一起"失联"。
static void OnWsSessionClosed(httpd_handle_t hd, int sockfd) {
    (void)hd;
    WsUnregisterClient(sockfd);
    close(sockfd);
}

// httpd 会对每个到达的 WS 帧调用本 handler, 处理一帧后返回 ESP_OK; 收到 CLOSE 帧时清空会话。
static esp_err_t HandleWs(httpd_req_t* req) {
    // 登记会话(供服务端主动推送状态): 支持多客户端, 用互斥锁保护 fd 登记表。
    int fd = httpd_req_to_sockfd(req);
    int client_count;
    {
        std::lock_guard<std::mutex> lk(s_ws_mtx);
        if (s_ws_count == 0) s_ws_hd = req->handle;  // handle 来自同一 server, 记一次即可
        int j = 0;
        for (; j < s_ws_count; ++j) if (s_ws_fds[j] == fd) break;
        if (j == s_ws_count && s_ws_count < MAX_WS_CLIENTS) s_ws_fds[s_ws_count++] = fd;
        client_count = s_ws_count;
    }
    // 连接数变化通知板级(锁外调用: 回调里可能再取其它锁, 避免嵌套死锁)
    NotifyWsClientCount(client_count);
    httpd_ws_frame_t frame = {};
    memset(&frame, 0, sizeof(frame));
    frame.type = HTTPD_WS_TYPE_TEXT;
    // 先探长度(读帧头)
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) return ret;
    // CLOSE 帧: 浏览器断开, 从登记表移除该 fd 并回一个 CLOSE 确认
    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        WsUnregisterClient(fd);
        httpd_ws_frame_t out = {};
        out.type = HTTPD_WS_TYPE_CLOSE;
        httpd_ws_send_frame(req, &out);
        return ESP_OK;
    }
    // PING 帧: 按 RFC6455 须回 PONG(浏览器/代理空闲心跳), 否则连接会被判失活断开
    if (frame.type == HTTPD_WS_TYPE_PING) {
        httpd_ws_frame_t pong = {};
        pong.type = HTTPD_WS_TYPE_PONG;
        pong.len = frame.len;
        if (frame.len > 0) {  // ping 带的数据原样随 pong 返回
            std::string pb(frame.len, '\0');
            frame.payload = reinterpret_cast<uint8_t*>(&pb[0]);
            if (httpd_ws_recv_frame(req, &frame, frame.len) == ESP_OK) {
                pong.payload = frame.payload;
                httpd_ws_send_frame(req, &pong);
            }
        } else {
            httpd_ws_send_frame(req, &pong);
        }
        return ESP_OK;
    }
    // PONG 帧(客户端回的心跳): 忽略不处理
    if (frame.type == HTTPD_WS_TYPE_PONG) return ESP_OK;
    if (frame.len) {
        std::string buf(frame.len, '\0');
        frame.payload = reinterpret_cast<uint8_t*>(&buf[0]);
        ret = httpd_ws_recv_frame(req, &frame, frame.len);
        if (ret != ESP_OK) return ret;
        std::string resp = WsHandleMessage(buf.c_str());
        httpd_ws_frame_t out = {};
        out.type = HTTPD_WS_TYPE_TEXT;
        out.payload = reinterpret_cast<uint8_t*>(&resp[0]);
        out.len = resp.size();
        httpd_ws_send_frame(req, &out);
    }
    return ESP_OK;
}

#else  // !CONFIG_HTTPD_WS_SUPPORT
// 未启用 WebSocket 时, 状态推送为空操作(板级调用 WebNotifyUnoStatus 安全)
void WebNotifyUnoStatus() {}
#endif

// GET /alarm?action=list：返回闹钟 JSON 数组(供网页/外部读取)
static esp_err_t HandleAlarmGet(httpd_req_t* req) {
    SetCors(req);
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
    return SendJson(req, body);
}

// POST /alarm：JSON body 支持 add / remove。
//   add:    {"action":"add","type":"relative|absolute","value":<秒>,"label":"..."}
//   remove: {"action":"remove","id":<编号>}
static esp_err_t HandleAlarmPost(httpd_req_t* req) {
    SetCors(req);
    char buf[1024];
    if (!ReadBody(req, buf, sizeof(buf))) return ESP_OK;

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

    return SendJson(req, resp);
}

static void StartHttpServer() {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.stack_size = 8192;  // 上传写 SD 卡需要较大栈(FATFS)，默认 4096 会栈溢出导致重启
    cfg.task_priority = 6;  // 低于音频输入任务(prio 8)，避免上传/访问时抢占 AI 音频
    cfg.max_uri_handlers = 20;  // CORS OPTIONS 预检也占用 handler 槽位, 调大预留
#if CONFIG_HTTPD_WS_SUPPORT
    // WS 异常断开时清理登记表 + 恢复 WiFi 省电(见 OnWsSessionClosed 注释)
    cfg.close_fn = OnWsSessionClosed;
#endif
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
    // CORS 预检: 各 HTTP URI 的 OPTIONS 都回 204 + 跨域头(本地页面/小程序经 IP 跨域访问)。
    httpd_uri_t options_uris[] = {
        { .uri = "/",       .method = HTTP_OPTIONS, .handler = HandleOptions, .user_ctx = nullptr },
        { .uri = "/upload", .method = HTTP_OPTIONS, .handler = HandleOptions, .user_ctx = nullptr },
        { .uri = "/music",  .method = HTTP_OPTIONS, .handler = HandleOptions, .user_ctx = nullptr },
        { .uri = "/uno",    .method = HTTP_OPTIONS, .handler = HandleOptions, .user_ctx = nullptr },
        { .uri = "/alarm",  .method = HTTP_OPTIONS, .handler = HandleOptions, .user_ctx = nullptr },
    };
    for (size_t i = 0; i < sizeof(options_uris) / sizeof(options_uris[0]); ++i) {
        if (httpd_register_uri_handler(server, &options_uris[i]) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register CORS OPTIONS handler for %s", options_uris[i].uri);
            return;
        }
    }
    // WebSocket 控制/状态通道(官方 httpd_ws_* API, 需启用 CONFIG_HTTPD_WS_SUPPORT)
#if CONFIG_HTTPD_WS_SUPPORT
    httpd_uri_t ws_uri = {
        .uri = "/ws", .method = HTTP_GET, .handler = HandleWs, .user_ctx = nullptr, .is_websocket = true,
    };
    if (httpd_register_uri_handler(server, &ws_uri) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /ws handler");
        return;
    }
#endif
    // 板级状态变化时通过 WebSocket 主动推送给前端(见 WebNotifyUnoStatus, 由板级直接调用)

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
