# 设计规格：TF 卡照片相册（网页查看拍过的照片）

- 日期：2026-09-17
- 板级：`main/boards/bread-compact-wifi-s3cam-airobot/`
- 状态：设计已获用户批准，待编写实现计划
- 上游依赖：同日的「日志崩溃留存 + 内存优化 + 网页拍照」改动（已实现，含 `local_photo.h/.cc` 与 `Esp32Camera::EncodeCurrentFrameToJpeg()`）

## 1. 背景与目标

本板已有「网页拍照」：点网页上的 📷 按钮 → `Esp32Camera::Capture()` 抓帧 → `EncodeCurrentFrameToJpeg()` 编码 → JPEG 存 PSRAM 常驻缓冲 → `GET /photo.jpg` 在页面显示（只保留**当次**一张，重启即失）。

用户需求：**有 TF 卡时把拍的照片存到卡上，并在网页里翻看历史照片**。

目标：

1. 网页按钮拍的照片落盘到 TF 卡，形成可翻看的历史相册；
2. AI 拍照（语音 `self.camera.take_photo`）**也**落盘，但放独立目录、且可开关；
3. 存储量自动滚动上限，永不塞满卡；
4. 不引入新分区、新依赖、新常驻内存。

## 2. 已确认的需求决策

| # | 决策点 | 结论 |
|---|---|---|
| 1 | 保留策略 | **D**：自动滚动保留最近 N 张 **且** 网页可手动删除/清空 |
| 2 | 哪些照片存卡 | **C**：网页拍照存卡；AI 拍照**也存**，但**独立目录 + 独立开关** |
| 3 | 保留数量 | 每个目录各 **100** 张（合计最多 200 张 ≈ 10MB） |
| 4 | AI 存卡开关 | **默认开**；网页开关 **+** AI 语音工具都能控；写入 NVS 断电保留 |
| 5 | 文件名 | 时间已同步 → `20260917_153002.jpg`；未同步 → `P_0001.jpg`（扫目录取最大编号+1，不覆盖） |
| 6 | 相册位置 | **A**：拍照按钮与当次照片留在「🎮 机器人控制」；新增独立 Tab「📷 照片」放相册 |

## 3. 架构与组件

### 3.1 存储布局

```
/sdcard/photos/       ← 网页按钮拍照（kind=web）
/sdcard/photos_ai/    ← AI 拍照（kind=ai）
```

- 文件名规则（决策 5 的落地）：

```cpp
// 伪代码：时间已同步（time() > 2020-01-01）则用时间戳，否则用递增序号兜底
if (time_synced) snprintf(name, sizeof(name), "%04d%02d%02d_%02d%02d%02d.jpg", ...);
else             snprintf(name, sizeof(name), "P_%04d.jpg", NextSequence(dir));  // 扫目录 max+1
```

- 滚动清理：**写卡成功后**扫描同目录，超出 100 张的按 `mtime` 最旧优先删除。
  - 必须「成功后」：写失败时清理会白删旧照片。
  - 清理失败（删除失败）只影响数量上限，不影响本次拍照结果，不上报错误。

### 3.2 新增板级文件：`photo_store.h/.cc`

职责单一：**只管 TF 卡上的照片仓库**（目录、命名、写、列、删、清理、开关），不碰相机、不碰 HTTP、不打 ESP_LOG（失败用返回值，符合本板 UART0 共享约定）。

```cpp
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

enum class PhotoKind { kWeb, kAi };

// 开机调用一次：确保两个目录存在。任一步失败返回 false（相册不可用，其余功能照常）。
bool PhotoStoreInit();

// 把整块 JPEG 存进对应目录，并在成功后滚动清理。
// 失败返回 false（调用方据此在前端提示"仅显示，未存卡"）。
bool PhotoStoreSave(PhotoKind kind, const uint8_t *jpeg, size_t len);

// 列表 JSON（name/size/mtime，按 mtime 倒序），供 GET /photos 直接返回。
std::string PhotoStoreListJson(PhotoKind kind);

// 删除单张（name 会做安全化，拒绝路径分隔符与 ".."）。成功返回 true。
bool PhotoStoreDelete(PhotoKind kind, const char *name);

// 清空某目录（只删 .jpg；返回删除成功的张数）。
int PhotoStoreClear(PhotoKind kind);

// AI 拍照存卡开关（NVS 持久化，键 "photo/ai_save"，默认开）。
void PhotoStoreSetAiSave(bool on);
bool PhotoStoreGetAiSave();

// 目录内当前张数（供前端显示 "n/100"）。
int PhotoStoreCount(PhotoKind kind);
```

复用现有实现，不自己写新解析/工具：`dirent.h` 扫描 + `stat()` 取 size/mtime + `cJSON` 建数组（照搬 `MusicListJson()` 的写法）、`SanitizeName()` 同款文件名净化思路、NVS 用 `Settings`（板级已用）。

### 3.3 共享改动：`Esp32Camera` 增加 JPEG 观察者（用户已批准）

问题：AI 拍照的 JPEG 是**流式编码**（`Explain()` 的编码线程把 `index==0` 的完整 JPEG 入队后直接上传），没有落缓冲；而 AI 拍照工具注册在**核心文件** `main/mcp_server.cc:102`，板级拦不住。

方案（纯增量，默认空 → 其它 15 个共用 `esp32_camera.cc` 的板子行为零变化）：

```cpp
// esp32_camera.h
void SetJpegObserver(std::function<void(const uint8_t *jpeg, size_t len)> cb);
```
```cpp
// esp32_camera.cc：在 Explain() 的编码回调 index==0 分支里，入队之后调用
if (jpeg_observer_) jpeg_observer_(static_cast<const uint8_t*>(data), len);
```

- 线程：观察者在**编码线程**运行，回调内**同步写卡**（`fwrite` 直接从 `data` 读，零额外缓冲）。
  - 为什么不能只记下指针稍后写：`index==0` 的 `data` 只在该次回调期间有效（编码器内部缓冲会被复用），回调返回后不得再用，因此必须在回调内写完（或自己 memcpy 一份）。
  - 代价：写卡期间占用编码线程 100~300ms，但队列里已投递完整 JPEG，**上传线程不受阻**；只是 `Explain()` 末尾的 `encoder_thread_.join()` 要等它，AI 响应尾延 100~300ms（可接受）。
  - 回退（若实测发现与 LCD 共用 SPI 总线导致刷屏卡顿）：改成先写进 `local_photo` 已分配的 128KB PSRAM 缓冲，再由 httpd 任务写卡。
- 回调必须可重入安全：用板级 `std::mutex` 保护写卡（与 `local_photo` 的写卡共用一把锁）。
- 清理：板级析构时 `SetJpegObserver(nullptr)`（或注入空 function）避免悬垂。

### 3.4 HTTP 接口（`http_upload_server.cc` / `.h`）

复用现有工具：`SetCors`、`SendJson`、`ReadBody`、`cJSON`。

| 方法 | URI | 说明 |
|---|---|---|
| GET | `/photos?kind=web\|ai` | 列表：`{"ok":true,"kind":"web","count":37,"limit":100,"items":[{"name":"..","size":..,"mtime":..}]}` |
| GET | `/photos/file?kind=web&name=xxx.jpg` | 单张 JPEG，`Content-Type: image/jpeg` + `Cache-Control: no-store`；文件名非法/不存在 → 404 |
| POST | `/photos` | body：`{"action":"delete","kind":"web","name":".."}` / `{"action":"clear","kind":"web"}` / `{"action":"ai_save","on":1}` / `{"action":"status"}` |
| POST | `/photo/take` | 现有接口，响应增加 `"saved":0/1,"file":"..","count":n` |

实现要点：

- 解析 kind 用一个小工具函数（`web`/缺省 → kWeb，`ai` → kAi），未知值返回 400。
- `name` 必须经安全化（拒绝 `/`、`\`、`..`），并**只接受 `.jpg` 后缀**，防路径穿越读任意文件。
- `GET /photos/file` 用 `fopen`/`fread` **分块**发送（4KB 栈缓冲 + `httpd_resp_send_chunk`），不把 60KB 一次性读进内存；发送完 `httpd_resp_send_chunk(req, NULL, 0)`。
- 路由表 `max_uri_handlers` 从 **20 提到 24**（新增 `/photos` GET/POST + `/photos/file` GET + `/photos` OPTIONS 预检）。
- 板级调用：`http_upload_server.cc` 与 `photo_store.cc` 同属本板目录（由 `main/CMakeLists.txt` 的 `file(GLOB boards/<BOARD_DIR>/*.cc)` 一并编译），直接 `#include "photo_store.h"` 调用即可，**不需要**通过 `http_upload_server.h` 注入 Api（KISS）。`PhotoStoreInit()` 在 `InitializeUploadServer()` 里调用（与 `LocalPhotoInit()` 同一处）。

### 3.5 前端（`web/index.html`）

- 新增 Tab「📷 照片」（`showTab` 注册项 + 容器 div），与现有音乐/机器人控制 Tab 同级。
- 相册：
  - 默认加载**最近 12 张**，滚到底（`IntersectionObserver`）自动加载下一批 12 张；底部同时给一个「加载更多（剩余 n 张）」按钮（两者共用同一个 `loadMore()`，按钮作为不能触发滚动/不支持 Observer 时的兼底）；
  - 网格用 `<img src="/photos/file?kind=web&name=..">`；点击放大（overlay，铺满视口，再点关闭）；
  - 每张右上角 `×` → `POST /photos {action:delete}` → 本地移除该 DOM；
  - 顶部工具行：kind 切换（网页/AI）、`n/100` 张数、占用（求和 size，前端算）、「AI 拍照也存卡」开关（`action:ai_save`）、「清空」（`action:clear`，**二次确认**）。
- 日志轮询：进入「📷 照片」Tab 时暂停（复用现有 `logPollIntervalMs()` 的「离开面板即停」机制）。
- 机器人控制面板的拍照结果：在照片下方加一行状态 —— 成功存卡显示「已存卡：`文件名`（n/100）」，否则「仅显示（未插卡或写卡失败）」。
- 无 TF 卡时：照片 Tab 隐藏（页面初始化时按 `/photos` 的返回判断；失败即隐藏）。

## 4. 数据流

```
网页点 📷  →  POST /photo/take
                └─ LocalPhotoCapture()（抓帧+编码，PSRAM）
                     ├─ LocalPhotoData() → GET /photo.jpg（当次显示）
                     └─ PhotoStoreSave(kWeb, jpeg, len) → /sdcard/photos/<name>.jpg → 滚动清理

AI 语音拍照 →  mcp_server.cc: self.camera.take_photo → Esp32Camera::Explain()
                 ├─ 编码线程 index==0 → jpeg_observer_
                 │     └─ PhotoStoreSave(kAi, ...)  ← 受 PhotoStoreGetAiSave() 控制
                 └─ 同一份 JPEG 流式上传云端（不变）

网页看相册 →  GET /photos?kind=web          → PhotoStoreListJson()
              GET /photos/file?kind&name=..  → fopen/fread 分块发送
```

## 5. 关键约束与理由

| 约束 | 理由 |
|---|---|
| **不做缩略图**，列表直接用原图 | ESP32 解码 640×480 JPEG 需 100~300ms + 一块解码缓冲（内部 RAM 很紧，本板 `free sram` 仅 20~25KB）；浏览器缩放即可 |
| 相册**分页懒加载**（12 张/批） | 设备从 TF 卡**逐张**读取并发送（每张 50KB、100~300ms），一次性拉 100 张会把 httpd 任务（单线程，同时跑 WS 日志/上传）卡死 |
| 用 **query 参数**而非 `/photos/<name>` | httpd 的 URI 通配符匹配默认关闭（全项目未启用 `CONFIG_HTTPD_URI_MATCH_WILDCARD`），`/photos/<name>` 需要额外开配置 |
| 清理**只在写卡成功后**做 | 否则写失败还会误删旧照片 |
| 进入照片 Tab **暂停日志轮询** | httpd 单线程：相册拉图与 WS 日志拉取会互相排队 |
| 写卡不新增常驻缓冲 | 网页路径直接用 `local_photo` 的 PSRAM 缓冲；AI 路径在编码回调内**同步写卡**（`fwrite` 直接读编码器缓冲，零拷贝、零额外分配） |
| 相册功能不影响 `no-tfcard` 变体 | 该变体无 httpd、无 SD，`PhotoStore*` 不会被调用；前端按接口失败隐藏 Tab |

## 6. 错误处理

- 所有 `PhotoStore*` 失败一律**返回布尔/空结果**，不打 ESP_LOG（本板日志与 Arduino 指令共用 UART0）。
- 写卡失败：`/photo/take` 仍返回 `ok:true` 但 `saved:0`，前端提示「仅显示」——拍照本身成功即算成功。
- 列表/单张失败：HTTP 404 / 空数组，前端显示「暂无照片」。
- 目录不存在：`PhotoStoreInit()` 创建（`mkdir`）；创建失败 → 相册不可用（Tab 隐藏），拍照仍走 PSRAM。
- 删除失败（文件占用/只读卡）：返回 `ok:false`，前端保留该项并提示。
- 清空只删 `.jpg`，不动目录内其它文件。

## 7. 测试策略（`scripts/tests/`，沿用源码文本断言的既有风格）

新增 `test_airobot_photo_store.py`：

- 目录常量存在且为 `/sdcard/photos`、`/sdcard/photos_ai`；
- 保留上限常量 100，且清理调用出现在**写卡成功之后**（同一函数内 `fwrite`/写成功判断在清理之前）；
- 文件名净化：拒绝 `/`、`\`、`..`；只接受 `.jpg`；
- AI 开关：默认开（`GetAiSave` 的 NVS 读取在无值时返回 true）、NVS 键 `photo/ai_save`；
- 时间未同步兜底：出现 `P_%04d.jpg` 序号分支；
- `photo_store.cc` 内**不出现** `ESP_LOG`（本板约定）。

更新 `test_airobot_web_photo.py`：

- `/photo/take` 响应含 `saved` 字段；
- 新增路由 `/photos`（GET/POST）、`/photos/file`（GET）均已注册；
- `max_uri_handlers` ≥ 24；
- `GET /photos/file` 用 `httpd_resp_send_chunk` 分块发送（不得一次性 `httpd_resp_send` 整个文件）；
- 共享钩子：`esp32_camera.h` 有 `SetJpegObserver`，`esp32_camera.cc` 的 `index == 0` 分支里调用了 `jpeg_observer_`；
- 前端：存在「📷 照片」Tab、`ai_save` 开关、删除/清空调用、进入照片 Tab 时停日志轮询。

## 8. 不做的（YAGNI）

- 缩略图 / 图像处理；
- EXIF 时间戳写入；
- zip 打包下载；
- 改动 AI 上传链路（`Explain()` 的 HTTP 部分保持不动）；
- 新分区表、新依赖、新常驻内存、新任务（写卡在既有线程内完成）；
- 照片同步到云端或小程序端。

## 9. 真机验证要点（需硬件）

1. 插卡 → 网页点 📷 → 页面出图 **且** 日志/状态提示「已存卡：`20260917_xxxxxx.jpg`」；卡上 `/sdcard/photos/` 能看到该文件。
2. 连拍 105 张 → `photos/` 内始终保持 ≤100 张，且最旧的被删、最新的在。
3. 语音「看看这是什么」→ 拍照上传正常（AI 回答正确）**且** `/sdcard/photos_ai/` 新增一张；关掉网页开关后再拍 → 不再新增。
4. 网页相册：滚动加载第二批/第三批正常；点图放大；单张删除后列表与卡同步；清空后列表为空。
5. 撞车测试：网页连点拍照的同时语音拍照 → 不重启，最多一次失败（互斥兜底）。
6. 拔卡后拍照 → 页面出图 + 提示「仅显示（未插卡）」；照片 Tab 隐藏。
7. 断电重启 → 照片仍在（TF 卡持久化）；相册开关状态保持（NVS）。
