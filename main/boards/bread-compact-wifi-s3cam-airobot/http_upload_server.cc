#include "http_upload_server.h"

#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <strings.h>

#define TAG "HttpUpload"

#define MUSIC_DIR "/sdcard/music"

// 上传成功回调（在 StartUploadServer 时注入），用于刷新上层歌曲列表缓存
static std::function<void()> s_on_uploaded;
// 等待 WiFi 就绪的轮询定时器
static esp_timer_handle_t s_wifi_timer = nullptr;

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

static esp_err_t HandleIndex(httpd_req_t* req) {
    const char html[] =
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<title>歌曲上传</title></head><body>"
        "<h2>🎵 上传歌曲到 TF 卡</h2>"
        "<p>上传前建议先用电脑上的转码脚本处理（24000Hz 单声道低码率 MP3，歌词转 UTF-8）<br>"
        "转码: python main/boards/bread-compact-wifi-s3cam-airobot/scripts/mp3_convert_for_esp32s3.py 源目录 输出目录</p>"
        "<input type=\"file\" id=\"f\" accept=\".mp3,.lrc\" multiple>"
        "<button onclick=\"up()\">上传</button> "
        "<label><input type=\"checkbox\" id=\"ov\" checked> 同名文件覆盖</label>"
        "<div style=\"margin:10px 0;width:100%;background:#eee;border-radius:4px;height:22px;overflow:hidden\">"
        "<div id=\"bar\" style=\"width:0%;height:22px;background:#4caf50;text-align:center;color:#fff;line-height:22px;font-size:13px;transition:width .2s\">0%</div></div>"
        "<div id=\"log\"></div>"
        "<script>"
        "function up(){"
        "const files=document.getElementById('f').files;"
        "const log=document.getElementById('log');"
        "const bar=document.getElementById('bar');"
        "if(!files.length){log.innerHTML='<div>请先选择文件</div>';return;}"
        "let i=0;"
        "function next(){"
        "if(i>=files.length){log.innerHTML+='<div><b>全部完成</b></div>';bar.style.width='0%';bar.textContent='0%';return;}"
        "const f=files[i++];"
        "log.innerHTML+='<div>⏳ 上传 '+f.name+' ('+(f.size/1024/1024).toFixed(1)+'MB) ...</div>';"
        "const xhr=new XMLHttpRequest();"
        "xhr.open('POST','/upload?name='+encodeURIComponent(f.name)+'&overwrite='+(document.getElementById('ov').checked?1:0));"
        "xhr.upload.onprogress=function(e){"
        "if(e.lengthComputable){"
        "const p=Math.round(e.loaded/e.total*100);"
        "bar.style.width=p+'%';bar.textContent=p+'%';"
        "}"
        "};"
        "xhr.onload=function(){"
        "if(xhr.status>=200&&xhr.status<300){"
        "log.innerHTML+='<div>✅ 成功 '+f.name+'</div>';"
        "}else if(xhr.status===403){"
        "log.innerHTML+='<div>⚠️ 同名已存在，跳过 '+f.name+'</div>';"
        "}else{"
        "log.innerHTML+='<div>❌ 失败('+xhr.status+') '+f.name+' '+xhr.responseText+'</div>';"
        "}"
        "next();"
        "};"
        "xhr.onerror=function(){"
        "log.innerHTML+='<div>❌ 网络错误 '+f.name+'</div>';"
        "next();"
        "};"
        "xhr.send(f);"
        "}"
        "next();"
        "}"
        "</script></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
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

    ESP_LOGE(TAG, "Upload: receiving '%s' -> %s (overwrite=%d)", name, path, overwrite ? 1 : 0);
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
    char* buf = static_cast<char*>(malloc(2048));
    if (buf == nullptr) {
        fclose(f);
        ESP_LOGE(TAG, "Upload: malloc failed");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;  // 响应已通过 send_err 发送，返回 OK 避免 httpd 直接关闭 socket
    }
    int total = 0;
    int ret;
    int next_log = 256 * 1024;  // 每 256KB 打印一次进度
    int64_t start_us = esp_timer_get_time();
    while ((ret = httpd_req_recv(req, buf, 2048)) > 0) {
        if (fwrite(buf, 1, static_cast<size_t>(ret), f) != static_cast<size_t>(ret)) {
            free(buf);
            fclose(f);
            remove(path);
            ESP_LOGE(TAG, "Upload: write failed at %d bytes", total);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write to SD card failed");
            return ESP_OK;  // 响应已通过 send_err 发送，返回 OK 避免 httpd 直接关闭 socket
        }
        total += ret;
        if (total >= next_log) {
            ESP_LOGE(TAG, "Upload: %s %d KB", name, total / 1024);
            next_log += 256 * 1024;
        }
    }
    free(buf);
    fclose(f);

    if (ret < 0) {
        remove(path);
        ESP_LOGE(TAG, "Upload: recv interrupted ret=%d after %d bytes", ret, total);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "receive interrupted");
        return ESP_OK;  // 响应已通过 send_err 发送，返回 OK 避免 httpd 直接关闭 socket
    }

    int64_t ms = (esp_timer_get_time() - start_us) / 1000;
    ESP_LOGE(TAG, "Upload: done '%s' %d bytes in %lld ms", name, total, static_cast<long long>(ms));
    httpd_resp_sendstr(req, "OK");
    if (s_on_uploaded) {
        s_on_uploaded();  // 通知上层刷新歌曲列表缓存
    }
    return ESP_OK;
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
    if (httpd_register_uri_handler(server, &index_uri) != ESP_OK ||
        httpd_register_uri_handler(server, &upload_uri) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register uri handlers");
        return;
    }

    ESP_LOGE(TAG, "Upload server started: http://<device-ip>/ (upload MP3 to %s)", MUSIC_DIR);
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
    ESP_LOGE(TAG, "WiFi IP ready, starting upload server");
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
