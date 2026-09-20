# 实时视频流（网页端）实施计划 v2 —— 采用官方标准方案

- 日期：2026-09-20（v2 重写，v1 的自写 WS 二进制方案已废弃）
- 分支：`feature/realtime-video`
- 目标板：`bread-compact-wifi-s3cam-airobot`（ESP32-S3 + 8MB PSRAM + OV 系列 DVP 摄像头 + ST7789 240×320 SPI LCD）
- **实施状态（2026-09-20）**：S1–S8 已全部落地；编译通过（`idf.py build`）+ 全量 367 项单测通过（新增视频流契约测试 27 项）。
  **真机未验证**——上板后需确认：出画、帧率/分辨率角标、取消后堆内存回落、与 ESP-NOW 节点控制共存、切走 Tab 自动断流。

## 0. 为什么换成标准方案（v1 → v2）

v1 打算自己写「WS 二进制帧 + JS 解码 + 自管缓冲」。调研官方后发现**完全没必要**：

浏览器 **原生支持 MJPEG**（`multipart/x-mixed-replace`）—— `<img src="/stream">` 就能显示实时视频，**零 JS 解码代码**。这正是 Espressif 官方所有相机示例的标准做法。

### 官方参考（权威来源）

| 来源 | 内容 |
|---|---|
| `espressif/esp32-camera` **README** | 直接给出完整的 `jpg_stream_httpd_handler()`（MJPEG over HTTP 标准实现） |
| `espressif/esp-iot-solution` `examples/camera/video_stream_server/main/stram_server.c` | 官方示例工程：独立 stream httpd + 拍照端点 |
| `espressif/esp-video-components` `esp_video/examples/simple_video_server/` | V4L2 版本（本板不用，因本板用 esp32-camera 驱动） |
| `espressif/arduino-esp32` `CameraWebServer/app_httpd.cpp` | 同款做法的 Arduino 版（事实标准） |

官方 README 的实现骨架（**照此实现即可，不自造**）：

```c
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

esp_err_t jpg_stream_httpd_handler(httpd_req_t *req) {
    camera_fb_t *fb = NULL;
    // 若请求带 ?x=1 则发单张 JPEG
    ...
    httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    while (true) {
        fb = esp_camera_fb_get();
        if (!fb) { httpd_resp_send_500(req); return ESP_FAIL; }
        if (fb->format == PIXFORMAT_JPEG) {      // ← JPEG 模式：fb->buf 直接发，零编码零拷贝
            fb_len = fb->len;  res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        } else {                                  // 非 JPEG：软件编码
            jpg_chunking_t jchunk = {req, 0};
            res = frame2jpg_cb(fb, 80, jpg_encode_stream, &jchunk) ? ESP_OK : ESP_FAIL;
            httpd_resp_send_chunk(req, NULL, 0);
            fb_len = jchunk.len;
        }
        esp_camera_fb_return(fb);
        // 发送 boundary + part header …
        // 帧率统计：last_frame / fr_start 差值
    }
}
```

## 1. 需求（用户原话拆解）

| # | 需求 | 验收标准 |
|---|------|----------|
| R1 | 「机器人控制」面板加**勾选框**（摇杆上方） | 勾选前无视频区 |
| R2 | 勾选 → 摇杆**上方**出现实时视频 | 实时画面（非静态图） |
| R3 | 取消勾选 → 关闭且**内存都清理掉** | stream httpd / 任务 / 缓冲全部释放，堆回到勾选前 |
| R4 | **低延时** | 端到端尽量 < 300ms，不排队积压 |
| R5 | 视频一角显示**实时帧率 + 分辨率** | 如 `15fps 640x480`，动态更新 |
| R6 | 分辨率默认 = **现在拍照的分辨率** | `FRAMESIZE_VGA` = 640×480 |
| R7 | **最好零拷贝** | JPEG 帧从 `fb->buf` 直接发，不做二次 memcpy |
| R8 | 低内存 / 低 CPU | 不新增常驻内存；不影响音频与 ESP-NOW |
| R9 | **用官方标准方案**（用户新增） | 照官方 README/示例实现，不自造协议/编码器 |
| R10 | **视频时 LCD 可以不预览**（用户新增） | 允许为性能让出 LCD 预览 |

## 2. 关键决策：**仅视频期间按需切换**到 `PIXFORMAT_JPEG`

**用户明确要求**：只有**开启实时视频流时**才启用 JPEG；没开视频时保持现状（RGB565 + LCD 拍照预览一切照旧）。

| 时机 | 相机模式 | LCD 拍照预览 |
|---|---|---|
| 平时（默认） | `PIXFORMAT_RGB565` VGA（**现状不变**） | ✅ 正常 |
| 勾选实时视频 | `PIXFORMAT_JPEG` VGA（`esp_camera_deinit()` + `esp_camera_init()`） | ❌ 让出 |
| 取消勾选 | **切回** `PIXFORMAT_RGB565` | ✅ 恢复 |

切换约 200~300ms，期间相机不可用 → 用互斥 + “切换中”状态挡住并发拍照。

**为什么值得切**（官方 README：*"JPEG mode always gives better frame rates"*）：

OV 系列摄像头模组**自带 JPEG 编码硬件**，`PIXFORMAT_JPEG` 时驱动直接返回 JPEG 帧：

| 指标 | 现在（RGB565 VGA） | 改成 JPEG VGA | 收益 |
|---|---|---|---|
| 单帧大小 | 600 KB | **~40 KB** | 省 ~560KB/帧 |
| 帧缓冲内存 | 600KB × 1 | ~40KB × 2（`fb_count=2` 连续模式） | **省 ~520KB PSRAM** |
| 每帧 CPU 编码 | 软件编码（`esp_new_jpeg`） | **零**（模组硬件出图） | CPU 大幅释放 |
| 传输拷贝 | 编码输出→缓冲→发送 | **fb->buf 直接 chunk 发送** | **零拷贝**（R7 达成） |
| 帧率 | 受限 | 15~25fps @VGA | 更高更稳 |
| LCD 预览 | 有（每帧 600KB malloc+memcpy） | **无**（JPEG 需解码才能显示） | 省 600KB/帧 |

### 代价与注意事项

- **切换必须能回退**：`esp_camera_init` 失败时不能把相机丢在坏状态；`video_stop` 无论当前模式都尝试恢复 RGB565，失败则打 ERROR 并允许用户重试。
- **`fb_count` 必须保持 1**：`esp32_camera.h` 注释明确“fb_count=2 会让 cam_hal 多占 ~30KB DMA **内部** RAM，本板内部 SRAM 扁不住” → 视频也用 `fb_count=1`（JPEG 下为非连续模式，帧率略低但可接受）。
- **配套小改**：`local_photo.cc` 在 JPEG 模式下直接取用 fb（避免再编码一次）；`Esp32Camera::Explain()` 的 JPEG 直通分支已存在，验证即可。

## 3. 架构（v2）

### 3.1 设备侧

```
┌─ 现有 httpd（端口 80）              ┌─ 新增 stream httpd（端口 81，仅视频时启动）
│  /ws   日志/控制/状态（TEXT）        │  GET /stream  → MJPEG multipart（chunked）
│  /photo.jpg /upload /songs …        │                独立任务，长阻塞不拖累控制通道
└────────────────────────────────────┘  └──────────────────────────────────────────
                    ↑                                        ↑
        帧率/分辨率统计经 WS TEXT 推送            esp_camera_fb_get() → fb->buf 直发
                                              （零编码、零拷贝，fb_count=2 连续模式）

[勾选] video_start → ①相机切 JPEG 模式（deinit+init，~300ms）→ ②启 stream httpd（端口 81）→ ③强制 WIFI_PS_NONE → ④统计帧率经 WS 推送
[取消] video_stop  → ①停 stream httpd → ②相机**切回 RGB565**（恢复 LCD 拍照预览）→ ③恢复 WIFI_PS → ④停统计
```

**为什么要独立 httpd 实例**：`/stream` 的 handler 是**长循环**（一直发帧直到客户端断开），若挂在现有端口 80 上会占死 httpd 任务，把 WS 日志/控制/上传全部拖住。官方 `esp-iot-solution` 示例就是**独立 stream server**（含独立 `server_port`），照此实现。

### 3.2 前端（零解码代码）

```html
<!-- 「机器人控制」面板、摇杆上方 -->
<label class="check"><input type="checkbox" id="videoChk" onchange="toggleVideo(this)"> 实时视频（占用 WiFi 带宽，可能影响节点控制）</label>
<div id="videoWrap" style="display:none">
  <img id="videoImg" alt="实时视频">        <!-- src 只在勾选时设置 -->
  <span id="videoStat">--fps --x--</span>   <!-- 角标：帧率/分辨率 -->
</div>
<script>
function toggleVideo(el) {
  const wrap = document.getElementById('videoWrap');
  const img  = document.getElementById('videoImg');
  if (el.checked) { img.src = '/stream'; wrap.style.display = ''; wsSend({action:'video_start'}); }
  else            { img.removeAttribute('src'); wrap.style.display = 'none'; wsSend({action:'video_stop'}); }
}
</script>
```

- 取消勾选 → `removeAttribute('src')` 会让浏览器**主动断开** HTTP 连接 → 设备侧 `httpd_resp_send_chunk` 立即返回错误 → 流循环退出 → 释放。
- 帧率/分辨率**不从 `<img>` 猜**（拿不到），由设备侧统计后走**已有 WS** 推送：`{"video":1,"fps":15,"w":640,"h":480}` → 前端更新角标（R5）。

### 3.3 协议

| 方向 | 通道 | 内容 |
|---|---|---|
| 前端→设备 | WS TEXT | `{"action":"video_start"}` / `{"action":"video_stop"}` |
| 设备→前端 | WS TEXT | `{"video":1,"fps":N,"w":640,"h":480}`（约 1 次/秒；停止后推 `{"video":0}`） |
| 设备→前端 | **HTTP**（端口 81） | `/stream` → `multipart/x-mixed-replace` JPEG 流 |

## 4. 实施步骤

| 步骤 | 内容 | 验证 |
|---|---|---|
| S1 | 相机配置改 `PIXFORMAT_JPEG` + `fb_count=2` + `CAMERA_GRAB_LATEST`，JPEG 尺寸 VGA（不改分辨率，符合 R6） | 编译 + 串口/网页日志确认无报错；拍照功能仍可用 |
| S2 | `local_photo.cc`：JPEG 直通（避免重复编码） | 单测 + 网页拍照实测 |
| S3 | 新增 `local_video_stream.{h,cc}`：照官方 `jpg_stream_httpd_handler` 实现 `/stream`（独立 httpd、端口 81、帧率统计） | 浏览器直接打开 `http://<ip>:81/stream` 出画 |
| S4 | WS 加 `video_start` / `video_stop`（启停 stream httpd + WiFi PS 切换 + 帧率推送） | WS 客户端实测启停与内存回升 |
| S5 | 前端：勾选框 + 视频区 + 角标 + 生命周期清理 | web-access（iPhone 视口）实测 + 截图 |
| S6 | 调优：`jpeg_quality`、帧率上限（默认 12fps）、丢帧/背压 | 实测帧率、延时、内存 |
| S7 | 共存回归：ESP-NOW 节点控制 + 语音对话 + 音频播放时开视频 | 实测通过 |
| S8 | 文档：README 新章节 + 踩坑 + 本计划勾选 | 评审 |

## 5. 风险与对策

| 风险 | 对策 |
|---|---|
| 视频流占死 httpd | **独立 httpd 实例/端口**（官方做法） |
| ESP-NOW 被抢空口 | 默认帧率上限 12fps；页面提示；实测节点控制重传率 |
| 慢客户端导致发送阻塞 | chunk 发送失败即退出循环（handler 返回）→ 释放 |
| LCD 预览消失引发困惑 | README 明确说明；页面勾选框注明 |
| 内存没释放干净（R3） | `video_stop` 停止 httpd（`httpd_stop`）+ 释放统计资源；取消勾选后实测堆差值（目标 <5KB） |
| 与拍照/AI 拍照并发 | 复用 `local_photo` 的互斥思路；切换期间置“切换中”状态，拍照请求直接返回提示而不去抢相机 |
| 切换后相机起不来（init 失败） | `video_stop` 强制回退 RGB565；失败则 ERROR 日志 + 页面提示重试；不影响其他功能（相机不可用则拍照也不可用，但至少状态一致） |
| 相机切 JPEG 后颜色/翻转异常 | `SetHMirror/SetVFlip` 在 JPEG 模式同样生效，实测确认；`jpeg_quality=12` 保持 |

## 6. 测试计划

- **契约单测**：`/stream` handler 注册、`video_start/stop` action 存在、`img.src` 只由勾选框控制、`video_stop` 必调用 `httpd_stop`。
- **端到端（真机）**：勾选→出画→帧率角标更新→取消→画面消失+内存回落；连续开关 10 次不泄漏。
- **共存**：开视频时节点控制响应时间、语音唤醒/对话、音乐播放。
- **长稳**：连续视频 ≥30 分钟不重启、无内存下滑。

## 7. 验收清单

- [ ] R1 勾选框位于摇杆上方
- [ ] R2 勾选出画、取消立即停
- [ ] R3 取消后堆内存回到勾选前（<5KB）
- [ ] R4 端到端延时实测记录
- [ ] R5 角标显示实时帧率与分辨率
- [ ] R6 默认 640×480
- [ ] R7 JPEG 帧从 `fb->buf` 直发（代码评审确认无 memcpy）
- [ ] R8 音频/ESP-NOW 共存测试通过
- [ ] R9 实现照官方 `jpg_stream_httpd_handler` 模式（代码评审确认）
- [ ] R10 LCD 预览让出（在文档与页面上说明）

## 8. 待确认 / 待实测

1. **用户确认**：相机改为 JPEG 模式后**平时拍照也没有 LCD 预览**（不接受则改为"视频时运行时切换模式"，复杂度与风险上升）。
2. VGA JPEG 的实际单帧大小与帧率（决定帧率上限与带宽占用）。
3. 独立 stream httpd 的端口选择（81）是否与现有服务冲突。
4. iPhone Safari 对 `<img src=.../stream>` 长连接的稳定性（MJPEG 在移动端 Safari 上需实测；桌面 Chrome 无问题）。
5. `fb_count=2` 的 PSRAM 占用实测（JPEG 模式约 2×40KB）。
