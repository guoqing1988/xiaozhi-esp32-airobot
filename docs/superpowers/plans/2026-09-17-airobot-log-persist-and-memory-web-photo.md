# 崩溃日志留存 + 内部 SRAM 回收 + 网页拍照 实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:executing-plans 或 superpowers:subagent-driven-development 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 让 web 日志面板在设备重启后仍能看到「崩溃前的日志 + 重启原因」，同时回收 web 日志功能占用的内部 SRAM，消除 AI 拍照时的偶发重启；并在此基础上新增网页拍照按钮与照片显示。

**架构：** 三块互相独立的能力，全部围绕 `main/boards/bread-compact-wifi-s3cam-airobot/`
1. **崩溃现场留存**：日志环形缓冲从 `.bss` 迁到 `.noinit` 段（软重启不清零），配三重校验 + 重启分隔行，重启原因取 `esp_reset_reason()`。**零新增内存**（段位平移）。
2. **内部 SRAM 回收**：收缩日志拉取路径的每次分配 + lwIP/WiFi 缓冲下沉到 PSRAM（本板 `config.json`）。不需要动任何共享代码。
3. **网页拍照**：复用 `Esp32Camera::Capture()` 已捕获的帧做 JPEG 编码（新增一个 additive 方法），JPEG 存 PSRAM 常驻缓冲，`GET /photo.jpg` 直接回给浏览器。

**技术栈：** ESP-IDF v6.0.2 / C++17 / FreeRTOS / esp_http_server + WebSocket / LVGL(仅复用) / 原生 HTML+JS。

---

## 范围声明（重要）

**本计划只修改以下文件：**

| 文件 | 性质 |
|---|---|
| `main/boards/bread-compact-wifi-s3cam-airobot/log_capture.h` | 板级私有 |
| `main/boards/bread-compact-wifi-s3cam-airobot/log_capture.cc` | 板级私有 |
| `main/boards/bread-compact-wifi-s3cam-airobot/http_upload_server.h` | 板级私有 |
| `main/boards/bread-compact-wifi-s3cam-airobot/http_upload_server.cc` | 板级私有 |
| `main/boards/bread-compact-wifi-s3cam-airobot/compact_wifi_board_s3cam_airobot.cc` | 板级私有 |
| `main/boards/bread-compact-wifi-s3cam-airobot/web/index.html` | 板级私有（`file(GLOB BOARD_EMBED_FILES)` 自动嵌入） |
| `main/boards/bread-compact-wifi-s3cam-airobot/config.json` | 本板构建配置（`scripts/build.py` 读取） |
| `main/boards/bread-compact-wifi-s3cam-airobot/README.md` | 板级文档 |
| `main/boards/common/esp32_camera.h` / `.cc` | ⚠️ **共享（16 个板共用）——仅阶段 C 需要，且必须用户确认** |
| `scripts/tests/test_airobot_*.py` | 新增测试（遵循项目 `python3 -m unittest discover -s scripts/tests` 约定） |

**明确不做（本次范围外）：**

- ❌ 不改 `partitions/*`（用户要求不动分区）→ 因此 **不用 core dump 方案**
- ❌ 不改 `main/CMakeLists.txt`（`-Wl,--wrap=esp_panic_handler` 也不做）→ 因此 **web 侧拿不到 panic backtrace 地址**，backtrace 仍靠串口抓
- ❌ 不改 `sdkconfig.defaults.esp32s3`（`SPIRAM_MALLOC_ALWAYSINTERNAL` 是全局的，会影响所有 S3 板）
- ❌ 不改 `main/display/lcd_display.cc`（预览降采样属于共享 display 层，收益大但影响面大，另行评估）
- ❌ 不改 `http_upload_server.cc` 里 `cfg.stack_size = 8192`（注释说明 SD 卡上传需要，下调有栈溢出风险）
- ❌ 不新增帧缓冲（`fb_count` 保持 1）：`cam_hal` 每帧需要 30720 字节 DMA **内部** RAM，`fb_count=2` 会多占 ~30KB 内部 RAM，与本次目标冲突

---

## 背景事实（实现时的依据）

| 事实 | 位置 | 影响 |
|---|---|---|
| `SystemInfo::PrintHeapStats()` 打的是 `MALLOC_CAP_INTERNAL` | `main/system_info.cc:151` | 实测空载 free 24.7KB、**历史最低 6.1KB**，拍照窗口出现 |
| `# CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP is not set` | `sdkconfig:2219` | WiFi/lwIP 缓冲只能放内部 RAM，8MB PSRAM 帮不上忙 |
| `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=2048` | `sdkconfig.defaults.esp32s3:8` | ≤2KB 分配全部塞内部 RAM → 堆抖动/碎片 |
| `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` / `LWIP_TCP_WND_DEFAULT` = 5760 | `sdkconfig:3057,3059` | 每条 TCP 连接最多 ~11.5KB 内部 RAM；Kconfig 下限 2440 |
| `CONFIG_LWIP_MAX_SOCKETS=16` | `main/boards/bread-compact-wifi-s3cam-airobot/config.json` | socket 控制块与缓冲池 |
| `esp_log_set_vprintf()` 只接管 `ESP_LOGx` | `log_capture.cc:75` | panic/abort 走 ROM printf 直写 UART0 → **web 面板永远看不到崩溃原因**；重启后静态缓冲清零 → 崩溃前日志也全丢（本计划要修的两点） |
| `cfg.stack_size = 8192`（httpd 任务） | `http_upload_server.cc:842` | web 功能的常驻内部 RAM |
| `MAX_WS_CLIENTS 4` + `static char pull_buf[1024]` | `http_upload_server.cc:37,607` | 每次 `log_pull`：`pull_buf`→`std::string`→`cJSON` strdup→`cJSON_Print` 共 ~3 份 KB 级分配 |
| 前端只要停在「🎮 机器人控制」面板就每秒 `log_pull` | `web/index.html:871-875,982,991` | 日志区收起时也在拉 |
| `file(GLOB boards/<BOARD_DIR>/*.cc *.c)` | `main/CMakeLists.txt:883` | 板级新增源文件**不需要**改核心 CMake |
| `config.fb_count = 1`，`buffer_size 30720` | `compact_wifi_board_s3cam_airobot.cc:200`、启动日志 | 单帧 DMA 缓冲 30KB **内部** RAM |

---

## 文件结构

**修改：**
- `main/boards/bread-compact-wifi-s3cam-airobot/log_capture.h` — 补 `LogCaptureClear()` 声明与持久化语义说明
- `main/boards/bread-compact-wifi-s3cam-airobot/log_capture.cc` — 环形缓冲迁 `.noinit`、三重校验、重启分隔行、`LogCaptureClear()`
- `main/boards/bread-compact-wifi-s3cam-airobot/http_upload_server.h` — 新增 `CameraWebApi` + `SetCameraWebApi()` 声明
- `main/boards/bread-compact-wifi-s3cam-airobot/http_upload_server.cc` — `log_pull` 瘦身、`pull_buf` 512、`MAX_WS_CLIENTS` 2、`log_clear` 动作、`GET /photo.jpg` + `POST /photo/take`
- `main/boards/bread-compact-wifi-s3cam-airobot/compact_wifi_board_s3cam_airobot.cc` — 注入 `CameraWebApi`、`InitializeCameraTools()` 之后注册拍照实现、`InitializeUploadServer()` 前后生命周期
- `main/boards/bread-compact-wifi-s3cam-airobot/web/index.html` — 拉取频率分层 + 拍照按钮 + `<img>` 显示 + 设备缓冲清空按钮
- `main/boards/bread-compact-wifi-s3cam-airobot/config.json` — lwIP/WiFi 内存配置
- `main/boards/bread-compact-wifi-s3cam-airobot/README.md` — 新增踩坑 16、日志章节修订、网页拍照说明
- `main/boards/common/esp32_camera.h` / `.cc` — ⚠️ 仅阶段 C：新增 `EncodeCurrentFrameToJpeg()`（**纯增量，不改任何现有函数**）

**创建：**
- `scripts/tests/test_airobot_log_persist.py` — 静态回归：`.noinit` 段、三重校验、冷启动清空分支、ring 尺寸一致性
- `scripts/tests/test_airobot_web_photo.py` — 静态回归：`/photo.jpg` 路由、`CameraWebApi` 注入、前端按钮与轮询分层

---

# 阶段 A：崩溃现场留存（纯板级，零新增内存）

### 任务 A1：环形缓冲迁移到 `.noinit` 段

**文件：**
- 修改：`main/boards/bread-compact-wifi-s3cam-airobot/log_capture.cc:20-30`（缓冲与状态定义）、`:44-52`（`RingWrite`）、`:75-80`（`LogCaptureInit`）、`:118-132`（`LogCapturePull`）
- 修改：`main/boards/bread-compact-wifi-s3cam-airobot/log_capture.h`

- [ ] **步骤 1：把缓冲与游标打包进 `.noinit` 结构体**

把 `log_capture.cc` 匿名命名空间里的这几行：

```cpp
constexpr size_t kRingSize = 4096;
constexpr size_t kLineMax = 160;

char s_ring[kRingSize];
size_t s_head = 0;     // 下一个写入位置
uint32_t s_total = 0;  // 累计写入字节数(单调递增)，作为拉取序号
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
```

替换为：

```cpp
constexpr size_t kRingSize = 4096;
constexpr size_t kLineMax = 160;

// 环形缓冲放 .noinit 段：软重启(panic/看门狗/esp_restart/OTA)后 DRAM 内容保留，
// 所以"崩溃前的日志"不会被清掉，web 页面重连后按序号拉取仍能看到；
// 上电冷启动时该段内容是随机的，用下面三重校验识别并丢弃（见 LogCaptureInit）。
// 注意：本段为平移，不新增任何内存占用（原来就是 4KB 内部 RAM）。
//
// ⚠ 若实测发现某些复位类型下 .noinit 未保留（例如 bootloader 覆盖），
//   回退方案是把 s_store 换成 RTC_NOINIT_ATTR（RTC slow mem，S3 上 8KB，够放 4KB）。
constexpr uint32_t kRingMagic = 0x4C4F4731u;  // "LOG1"
constexpr uint32_t kRingVersion = 1u;

struct RingStore {
    uint32_t magic;   // == kRingMagic 才算有效
    uint32_t version; // == kRingVersion，避免升级后误读旧布局
    uint32_t size;    // == kRingSize，避免改尺寸后误读
    uint32_t head;    // < kRingSize
    uint32_t total;   // 累计写入字节数(单调递增)，作为拉取序号
    uint32_t boots;   // 软重启计数（仅用于日志展示）
    char ring[kRingSize];
};
RingStore s_store __attribute__((section(".noinit")));

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
```

- [ ] **步骤 2：`RingWrite` 改用 `s_store`**

替换 `RingWrite` 函数体中的字段引用（逻辑不变）：

```cpp
void RingWrite(const char* data, size_t len) {
    if (len == 0) {
        return;
    }
    portENTER_CRITICAL(&s_mux);
    const size_t head = s_store.head;
    const size_t first = (len < kRingSize - head) ? len : kRingSize - head;
    memcpy(s_store.ring + head, data, first);
    if (len > first) {
        memcpy(s_store.ring, data + first, len - first);
    }
    s_store.head = (head + len) % kRingSize;
    s_store.total += static_cast<uint32_t>(len);
    portEXIT_CRITICAL(&s_mux);
}
```

- [ ] **步骤 3：加重启原因文字 + 冷/热启动判定，改写 `LogCaptureInit`**

在 `LogVprintfHook` 之后、`LogCaptureInit` 之前插入：

```cpp
// 复位原因 → 人话。用于"设备重启后告诉用户到底是怎么挂的"：
// PANIC 多为内存不足/空指针/栈溢出，INT_WDT/TASK_WDT 多为阻塞或饿死，BROWNOUT 是供电。
const char* ResetReasonText(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:  return "上电启动(冷启动)";
        case ESP_RST_EXT:      return "外部复位脚";
        case ESP_RST_SW:       return "软件重启(esp_restart/OTA)";
        case ESP_RST_PANIC:    return "PANIC 异常或 abort(多为内存不足/空指针/栈溢出)";
        case ESP_RST_INT_WDT:  return "中断看门狗(临界区过长或中断被饿死)";
        case ESP_RST_TASK_WDT: return "任务看门狗(某任务长时间不让出 CPU)";
        case ESP_RST_WDT:      return "其它看门狗";
        case ESP_RST_DEEPSLEEP:return "深睡眠唤醒";
        case ESP_RST_BROWNOUT: return "欠压重启(供电不足)";
        case ESP_RST_SDIO:     return "SDIO 复位";
        default:               return "未知原因";
    }
}

// 只有这些复位原因才认为"上一轮 RAM 里的日志值得保留"。
// 冷启动/未知一律丢弃，避免把随机 DRAM 当成日志显示。
bool ShouldKeepPreviousLog(esp_reset_reason_t r) {
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
           r == ESP_RST_WDT || r == ESP_RST_SW || r == ESP_RST_BROWNOUT;
}
```

把 `LogCaptureInit` 替换为：

```cpp
void LogCaptureInit() {
    if (s_orig_vprintf != nullptr) {
        return;  // 幂等：重复调用不再二次包装
    }

    const esp_reset_reason_t reason = esp_reset_reason();
    const bool state_ok =
        (s_store.magic == kRingMagic) && (s_store.version == kRingVersion) &&
        (s_store.size == kRingSize) && (s_store.head < kRingSize);

    if (state_ok && ShouldKeepPreviousLog(reason)) {
        // 软重启：保留崩溃前的日志，追加一行醒目的分隔（用 RingWrite，不能走 ESP_LOG 以免递归）
        s_store.boots++;
        char line[kLineMax];
        int n = snprintf(line, sizeof(line),
                         "\n================ 设备重启 #%u：%s ================\n"
                         "（以上为本轮重启前的日志，已从 RAM 中保留）\n\n",
                         static_cast<unsigned>(s_store.boots), ResetReasonText(reason));
        if (n > 0) {
            size_t len = (static_cast<size_t>(n) < sizeof(line)) ? static_cast<size_t>(n)
                                                                : sizeof(line) - 1;
            RingWrite(line, len);
        }
    } else {
        // 冷启动 / 段内容非法：整段重置（magic 最后写，避免半初始化状态被误判为有效）
        portENTER_CRITICAL(&s_mux);
        s_store.magic = 0;
        s_store.version = kRingVersion;
        s_store.size = kRingSize;
        s_store.head = 0;
        s_store.total = 0;
        s_store.boots = 0;
        s_store.magic = kRingMagic;
        portEXIT_CRITICAL(&s_mux);
    }

    s_orig_vprintf = esp_log_set_vprintf(LogVprintfHook);
}

void LogCaptureClear() {
    portENTER_CRITICAL(&s_mux);
    s_store.head = 0;
    s_store.total = 0;
    portEXIT_CRITICAL(&s_mux);
}
```

同时在文件顶部补 include：

```cpp
#include <esp_system.h>  // esp_reset_reason()
```

- [ ] **步骤 4：`LogCapturePull` 改用 `s_store`，`Append` 增加越界防御**

`LogCapturePull` 内全部字段替换（逻辑不变）：

```cpp
    portENTER_CRITICAL(&s_mux);
    const uint32_t total = s_store.total;
    const size_t head = s_store.head;
    ...
    const size_t pos = (head + kRingSize - (total - start)) % kRingSize;
    const size_t first = (len < kRingSize - pos) ? len : kRingSize - pos;
    memcpy(buf, s_store.ring + pos, first);
    if (len > first) {
        memcpy(buf + first, s_store.ring, len - first);
    }
```

- [ ] **步骤 5：`log_capture.h` 补声明与语义说明**

在 `LogCapturePull` 声明之后追加：

```cpp
// 清空设备侧环形缓冲（网页「清空设备缓冲」按钮 / AI 工具用）。
// 注意：清空只影响设备侧历史，不影响浏览器已显示的内容。
void LogCaptureClear();
```

并在文件头部注释里追加一段：

```cpp
// 崩溃现场留存的边界（README 也写了，改代码时保持一致）：
//  * 本模块**只能保留 ESP_LOGx 产生的日志**。panic/abort 的 Guru Meditation、
//    backtrace、`stack overflow in task xxx` 由 IDF panic handler 用 ROM printf
//    直写 UART0，不经过 esp_log_set_vprintf() 的钩子 —— 所以网页永远看不到它们，
//    要拿 backtrace 必须接 USB 串口（且**不需要**打开串口镜像开关）。
//  * 软重启(panic/看门狗/OTA/esp_restart)后环形缓冲内容会保留，网页能看到"崩溃前的日志"；
//    掉电重启(冷启动)则丢弃，避免把随机 RAM 当日志显示。
//  * 重启原因由 esp_reset_reason() 给出，作为分隔行打印在日志流里。
```

- [ ] **步骤 6：编译验证**

```sh
source ~/esp/v6.0.2/esp-idf/export.sh
python3 scripts/build.py bread-compact-wifi-s3cam-airobot --name bread-compact-wifi-s3cam-airobot
```
预期：编译通过；`.noinit` 段被正确分配（如出现 `warning: 's_store' ... .noinit` 之类提示需检查属性写法）。

- [ ] **步骤 7：Commit**

```bash
git add main/boards/bread-compact-wifi-s3cam-airobot/log_capture.cc main/boards/bread-compact-wifi-s3cam-airobot/log_capture.h
git commit -m "feat: 网页日志环形缓冲改为 .noinit 段保留, 重启后仍可查看崩溃前日志与重启原因"
```

### 任务 A2：真机验证持久化（不需要真的崩溃）

**文件：** 无（验证任务）

- [ ] **步骤 1：烧录 + 让日志级别到 INFO**

```sh
python3 scripts/build.py bread-compact-wifi-s3cam-airobot --name bread-compact-wifi-s3cam-airobot -f
```
打开 `http://<设备IP>/`，「🎮 机器人控制」→ 展开「🐞 系统日志」→ 级别选「信息」，等 10 秒看到日志在滚。

- [ ] **步骤 2：触发软重启，确认日志被保留**

对 AI 说「重启设备」，或调用 MCP 工具 `self.reboot`。重启后刷新页面，展开系统日志。
预期：能看到 `================ 设备重启 #1：软件重启(esp_restart/OTA) ================`，且这一行**上方**是本轮重启前的日志。

- [ ] **步骤 3：断电对照**

拔掉 USB/电源再上电，刷新页面。
预期：分隔行消失、缓冲从空开始（冷启动丢弃），说明三重校验有效、没有把随机 RAM 当日志。

- [ ] **步骤 4：记录证据到 README 草稿**

把两次的实际日志片段（尤其是分隔行与 `PANIC`/`INT_WDT` 文案）记下来，任务 D2 用。

---

# 阶段 B：内部 SRAM 回收（纯板级）

### 任务 B1：`log_pull` 路径瘦身

**文件：**
- 修改：`main/boards/bread-compact-wifi-s3cam-airobot/http_upload_server.cc:37`（`MAX_WS_CLIENTS`）、`:67-108`（`SanitizeLogText`）、`:598-626`（`log_pull` 分支）

- [ ] **步骤 1：`MAX_WS_CLIENTS` 4 → 2**

```cpp
// 本板内部 SRAM 紧张：每个 WS 会话都要占用 socket/帧缓冲。
// 实际使用只有一个浏览器标签页，「机器人控制」面板；留 2 个够用（换标签页时旧连接会自行关闭）。
#define MAX_WS_CLIENTS 2
```

- [ ] **步骤 2：`SanitizeLogText` 改成原地处理，消掉一次 KB 级分配**

把 `SanitizeLogText(std::string& s)` 替换为**原地**版本（逻辑完全等价，只是不再构造新 string）。

> ⚠️ **函数名保持 `SanitizeLogText` 不变**：`scripts/tests/test_log_capture.py::TestLogWebApi::test_pull_sanitizes_text`
> 断言的是这个名字，改签名不改名可以不动该测试。

```cpp
// 把日志文本清洗成合法 UTF-8，再交给 cJSON 转义（cJSON 对高位字节原样输出，
// 日志里若混入二进制会让前端 JSON.parse 整批失败）。裸 NUL 也一并替换：
// c_str() 遇到 NUL 会截断，文本会莫名变短。
//
// 原地改写（旧版构造第二个 std::string 再 swap，每次拉取多一次 1KB 内部 RAM 分配，
// 内部 SRAM 只有几十 KB，这点抖动不值得）。
static void SanitizeLogText(char* s, size_t len) {
    size_t w = 0;
    for (size_t i = 0; i < len;) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0 || c < 0x80) {
            s[w++] = (c == 0) ? '?' : static_cast<char>(c);
            ++i;
            continue;
        }
        size_t seq = 0;
        if ((c & 0xE0) == 0xC0) {
            seq = 2;
        } else if ((c & 0xF0) == 0xE0) {
            seq = 3;
        } else if ((c & 0xF8) == 0xF0) {
            seq = 4;
        }
        bool ok = (seq > 0) && (i + seq <= len);
        if (ok) {
            for (size_t k = 1; k < seq; ++k) {
                if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) {
                    ok = false;
                    break;
                }
            }
        }
        if (ok) {
            for (size_t k = 0; k < seq; ++k) {
                s[w++] = s[i + k];
            }
            i += seq;
        } else {
            s[w++] = '?';
            ++i;
        }
    }
    s[w] = '\0';
}
```

- [ ] **步骤 3：`log_pull` 用 512 字节静态缓冲 + 原地清洗 + 就地 NUL 结尾**

把 `log_pull` 分支的实现替换为：

```cpp
    } else if (strcmp(action, "log_pull") == 0) {
        cJSON* c_since = cJSON_GetObjectItem(root, "since");
        uint32_t since = (c_since != nullptr && cJSON_IsNumber(c_since))
                             ? static_cast<uint32_t>(c_since->valuedouble)
                             : 0;
        // 512B 静态缓冲：不占 httpd 栈(该任务 8KB 还要跑 SD 上传)，也尽量落在
        // CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL 的"内部 RAM"阈值以下，减少堆抖动。
        // WS 帧由 httpd 单任务串行处理，不存在并发。
        static char pull_buf[512];
        size_t len = 0;
        uint32_t next_seq = 0;
        LogCapturePull(since, pull_buf, sizeof(pull_buf) - 1, len, next_seq);  // 预留 1B 给 NUL
        pull_buf[len] = '\0';
        SanitizeLogText(pull_buf, len);  // 原地清洗；只会缩短，不会超过原长度
        cJSON* j = cJSON_CreateObject();
        if (j != nullptr) {
            cJSON_AddNumberToObject(j, "seq", static_cast<double>(next_seq));
            cJSON_AddStringToObject(j, "text", pull_buf);
            cJSON_AddNumberToObject(j, "mirror", LogCaptureGetUartMirror() ? 1 : 0);
            cJSON_AddNumberToObject(j, "level", static_cast<int>(esp_log_level_get("*")));
            // 缓冲装满说明可能还有积压，前端据此立即再拉一次追平（首次进入时常见）
            cJSON_AddNumberToObject(j, "more", (len == sizeof(pull_buf) - 1) ? 1 : 0);
            char* s = cJSON_PrintUnformatted(j);
            if (s != nullptr) {
                resp = s;
                free(s);
            }
            cJSON_Delete(j);
        }
    } else if (strcmp(action, "log_clear") == 0) {
        // 网页「清空设备缓冲」：清掉设备侧历史（浏览器画面由前端自己清）
        LogCaptureClear();
        resp = "{\"ok\":true}";
```

> 注意：`SanitizeLogText` 只做缩减不扩张，所以 `pull_buf` 预留的 NUL 位置不会被覆盖；`more` 的判定改回"拉满即可能还有"。

- [ ] **步骤 4：编译验证**

```sh
source ~/esp/v6.0.2/esp-idf/export.sh
python3 scripts/build.py bread-compact-wifi-s3cam-airobot --name bread-compact-wifi-s3cam-airobot
```
预期：编译通过，无 `SanitizeLogText` 未使用告警（旧的 `std::string&` 版本已删除）。

- [ ] **步骤 5：Commit**

```bash
git add main/boards/bread-compact-wifi-s3cam-airobot/http_upload_server.cc
git commit -m "perf: 日志拉取路径原地清洗 + 512B 缓冲 + WS 会话上限 2, 降低内部 SRAM 抖动"
```

### 任务 B2：前端拉取频率分层

**文件：**
- 修改：`main/boards/bread-compact-wifi-s3cam-airobot/web/index.html:869-885`（拉取定时器）、`:786-810`（`<details>` 加 id）、`:982,991`（面板切换）

- [ ] **步骤 1：不改 HTML 结构，用选择器定位折叠区**

> ⚠️ **不要给 `<details>` 加 `id`**：`test_log_capture.py::test_web_log_lives_in_uno_panel` 断言的是
> `'<details class="logbox">' in web`，加属性会让该测试失败。改用 `document.querySelector('details.logbox')` 定位，HTML 一行都不动。

- [ ] **步骤 2：拉取间隔分层（日志区展开 1s；收起 3s；离开面板停）**

把常量与 `startLogPolling` / `stopLogPolling` 替换为：

```js
    const LOG_MAX_CHARS = 40000;   // 前端保留的日志量上限（超出丢最早的）
    const UNO_CMD_MAX_CHARS = 6000;   // 下位机指令记录保留量
    const LOG_POLL_MS = 1000;        // 系统日志展开时：1s（看日志要跟手）
    const LOG_POLL_IDLE_MS = 3000;   // 收起时：3s —— 下位机指令记录与系统日志同源，
                                     // 不能直接停（否则指令回执不刷新），但可以降频。
    let logSeq = 0, logTimer = null, logText = '', logPaused = false, logBusy = false;
    let unoCmdText = '', unoCmdPending = '';   // pending: 跨批次未成行的残余

    function logPollIntervalMs() {
      const d = document.querySelector('details.logbox');
      return (d && d.open) ? LOG_POLL_MS : LOG_POLL_IDLE_MS;
    }
    function startLogPolling() {
      if (logTimer) return;
      logPaused = false; syncLogPauseBtn();
      pullLog();                                  // 进入面板先补一次
      logTimer = setInterval(pullLog, logPollIntervalMs());
    }
    function stopLogPolling() {
      if (logTimer) { clearInterval(logTimer); logTimer = null; }
    }
    // 展开/收起日志区时调整拉取频率（收起时仍要拉：指令记录同源）
    (function () {
      const d = document.querySelector('details.logbox');
      if (!d) return;
      d.addEventListener('toggle', function () {
        if (!logTimer) return;                    // 不在面板上，不用管
        clearInterval(logTimer);
        logTimer = setInterval(pullLog, logPollIntervalMs());
      });
    })();
```

- [ ] **步骤 3：日志区加「清空设备缓冲」按钮**

在「🧹 清空显示」按钮旁边追加：

```html
          <button class="btn" onclick="clearDeviceLog()">🗑 清空设备缓冲</button>
```

并在 `clearLogView()` 定义之后追加：

```js
    // 清空设备侧环形缓冲（浏览器画面单独用「清空显示」清）
    function clearDeviceLog() {
      wsSend({ action: 'log_clear' }).then(function () {
        logSeq = 0; logText = ''; unoCmdText = ''; unoCmdPending = '';
        const lv = document.getElementById('logView'); if (lv) lv.textContent = '';
        const uv = document.getElementById('unoCmdView'); if (uv) uv.textContent = '';
      });
    }
```

- [ ] **步骤 4：语法检查**

```sh
node --check <(sed -n '/<script>/,/<\/script>/p' main/boards/bread-compact-wifi-s3cam-airobot/web/index.html | sed '1d;$d')
```
预期：无输出（语法正确）。

- [ ] **步骤 5：Commit**

```bash
git add main/boards/bread-compact-wifi-s3cam-airobot/web/index.html
git commit -m "perf(web): 日志拉取按面板/折叠状态分层(1s/3s), 新增清空设备缓冲按钮"
```

### 任务 B3：lwIP/WiFi 缓冲下沉到 PSRAM（本板 config.json）

**文件：**
- 修改：`main/boards/bread-compact-wifi-s3cam-airobot/config.json`（两个 build 变体的 `sdkconfig_append`）

- [ ] **步骤 1：给两个变体都加上内存配置**

两个 build（`bread-compact-wifi-s3cam-airobot` 与 `-no-tfcard`）的 `sdkconfig_append` 数组各追加：

```json
                "CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y",
                "CONFIG_LWIP_TCP_SND_BUF_DEFAULT=2920",
                "CONFIG_LWIP_TCP_WND_DEFAULT=2920",
                "CONFIG_LWIP_MAX_SOCKETS=10"
```

- [ ] **步骤 2：重新生成 sdkconfig 并确认生效**

```sh
source ~/esp/v6.0.2/esp-idf/export.sh
python3 scripts/build.py bread-compact-wifi-s3cam-airobot --name bread-compact-wifi-s3cam-airobot
grep -E "SPIRAM_TRY_ALLOCATE_WIFI_LWIP|LWIP_TCP_SND_BUF_DEFAULT|LWIP_TCP_WND_DEFAULT|LWIP_MAX_SOCKETS" sdkconfig
```
预期：`=y` 与三个新数值都出现（旧值 5760/16 不再出现）。

- [ ] **步骤 3：音频回归（TRT_ALLOCATE 影响 WiFi 缓冲位置，必须实测）**

连 AI 对话 5 分钟：唤醒、打断、连续多轮、播放音乐。
预期：无卡顿/断流/唤醒失灵的明显回归。若出现异常，回退 `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`，其余三项保留。

- [ ] **步骤 4：Commit**

```bash
git add main/boards/bread-compact-wifi-s3cam-airobot/config.json
git commit -m "perf: 本板 WiFi/lwIP 缓冲优先落 PSRAM 并收窄 TCP 缓冲, 缓解内部 SRAM 压力"
```

### 任务 B4：内存与拍照回归验证

**文件：** 无（验证任务）

- [ ] **步骤 1：空载内存对比（改前 24.7KB / 最低 6.1KB 为基线）**

打开页面停在「机器人控制」面板（WS 常驻 + 每秒拉日志），等待 ≥2 分钟，从日志里抄 `SystemInfo: free sram / minimal sram`。

预期：`free sram` 与基线持平或更高；**关键看 `minimal sram` 不再掉到 6KB 量级**。

- [ ] **步骤 2：AI 连拍 5 次**

对 AI 连说「拍个照片」5 次（间隔 ≥5 秒），全程保持页面开着、日志级别「信息」。

预期：5 次都返回图片描述，**不再重启**；拍照窗口的 `minimal sram` 比基线明显改善。

- [ ] **步骤 3：把两组数字记录到 README 草稿**

任务 D2 要用（`free sram` / `minimal sram` 改前改后对照表）。

---

# 阶段 C：网页拍照 + 页面显示照片

> ⚠️ **本阶段需要 1 处共享代码的纯增量改动**（`main/boards/common/esp32_camera.h/.cc`，16 个板共用）。
> 原因：`capture()` 把帧锁在私有成员 `current_fb_`，板级拿不到像素；而 `fb_count=2` 会多占 ~30KB 内部 RAM（与阶段 B 冲突）。新增一个 `EncodeCurrentFrameToJpeg()` 是唯一"零额外内存 + 复用现有编码路径"的做法。
> **若不同意动共享文件，跳过阶段 C，只交付 A/B。**

### 任务 C1：`Esp32Camera` 新增纯增量方法

**文件：**
- 修改：`main/boards/common/esp32_camera.h`（新增 1 个 public 方法声明）
- 修改：`main/boards/common/esp32_camera.cc`（新增 1 个方法实现，**不改任何现有函数**）

- [ ] **步骤 1：头文件加声明（`Explain()` 声明之后）**

```cpp
    virtual std::string Explain(const std::string &question) override;

    // 把**已捕获**的当前帧(RGB565)编码成 JPEG 写入 out（网页拍照用，避免再抓一帧）。
    // 需要先调用 Capture() 成功。返回值: 编码成功且写入 out_len 字节。
    // 说明: 复用与 Explain() 相同的字节序处理与编码器；编码过程中不可并发调用 Capture()。
    bool EncodeCurrentFrameToJpeg(uint8_t *out, size_t out_capacity, size_t &out_len);
```

- [ ] **步骤 2：实现（加在 `Explain()` 之前）**

```cpp
namespace {
// image_to_jpeg_cb 的回调：一次性把整块 JPEG 拷进调用方缓冲（不做动态分配）。
struct JpegSink {
    uint8_t *buf;
    size_t capacity;
    size_t len;
};
size_t JpegSinkCb(void *arg, size_t index, const void *data, size_t len) {
    auto *sink = static_cast<JpegSink *>(arg);
    // index==0 携带完整 JPEG；后续调用为分块/哨兵，本用法忽略
    if (index == 0 && data != nullptr && len > 0 && len <= sink->capacity) {
        memcpy(sink->buf, data, len);
        sink->len = len;
    }
    return len;
}
}  // namespace

bool Esp32Camera::EncodeCurrentFrameToJpeg(uint8_t *out, size_t out_capacity, size_t &out_len) {
    out_len = 0;
    if (current_fb_ == nullptr || out == nullptr || out_capacity == 0) {
        return false;
    }
    if (current_fb_->format != PIXFORMAT_RGB565) {
        ESP_LOGE(TAG, "EncodeCurrentFrameToJpeg: unsupported format %d", current_fb_->format);
        return false;
    }

    // 与 Explain() 一致：RGB565 需要按需换字节序（默认开启），用独立的编码缓冲，
    // 避免破坏 current_fb_（Explain 之后可能还会被复用）。
    const size_t data_size = static_cast<size_t>(current_fb_->width) * current_fb_->height * 2;
    uint8_t *src = current_fb_->buf;
    if (swap_bytes_enabled_) {
        if (encode_buf_size_ < data_size) {
            if (encode_buf_ != nullptr) {
                heap_caps_free(encode_buf_);
                encode_buf_ = nullptr;
                encode_buf_size_ = 0;
            }
            encode_buf_ = static_cast<uint8_t *>(
                heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (encode_buf_ == nullptr) {
                ESP_LOGE(TAG, "EncodeCurrentFrameToJpeg: no PSRAM for encode buffer");
                return false;
            }
            encode_buf_size_ = data_size;
        }
        const uint16_t *s = reinterpret_cast<const uint16_t *>(current_fb_->buf);
        uint16_t *d = reinterpret_cast<uint16_t *>(encode_buf_);
        const size_t pixels = static_cast<size_t>(current_fb_->width) * current_fb_->height;
        for (size_t i = 0; i < pixels; ++i) {
            d[i] = __builtin_bswap16(s[i]);
        }
        src = encode_buf_;
    }

    JpegSink sink = {out, out_capacity, 0};
    const bool ok = image_to_jpeg_cb(src, data_size, current_fb_->width, current_fb_->height,
                                     V4L2_PIX_FMT_RGB565, 80, JpegSinkCb, &sink);
    if (!ok || sink.len == 0) {
        return false;
    }
    out_len = sink.len;
    return true;
}
```

- [ ] **步骤 3：编译验证（本板 + 另选一个共用 Esp32Camera 的板）**

```sh
source ~/esp/v6.0.2/esp-idf/export.sh
python3 scripts/build.py bread-compact-wifi-s3cam-airobot --name bread-compact-wifi-s3cam-airobot
python3 scripts/build.py bread-compact-wifi-s3cam --name bread-compact-wifi-s3cam
```
预期：两块都编译通过（证明是纯增量、未破坏其它板）。

- [ ] **步骤 4：Commit**

```bash
git add main/boards/common/esp32_camera.h main/boards/common/esp32_camera.cc
git commit -m "feat(camera): 新增 EncodeCurrentFrameToJpeg, 复用已捕获帧导出 JPEG 供网页拍照(纯增量)"
```

### 任务 C2：板级拍照服务（PSRAM 常驻 JPEG 缓冲）

**文件：**
- 创建：`main/boards/bread-compact-wifi-s3cam-airobot/local_photo.h`
- 创建：`main/boards/bread-compact-wifi-s3cam-airobot/local_photo.cc`
- 修改：`main/boards/bread-compact-wifi-s3cam-airobot/compact_wifi_board_s3cam_airobot.cc`（在 `InitializeUploadServer()` 之前初始化 + 注入回调）

- [ ] **步骤 1：创建 `local_photo.h`**

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

class Esp32Camera;  // 前置声明：只需指针，避免头文件互相包含

// 本板「网页拍照」服务（板级私有）。
//
// 与 AI 拍照(replace: Esp32Camera::Explain)的分工：
//   * AI 拍照：Capture() + 编码 + 上传到云端解释（会走 api.xiaozhi.me，内存峰值高）
//   * 网页拍照：Capture() + 编码 → 存进本模块的 PSRAM 常驻缓冲 → GET /photo.jpg 回给浏览器
// 两条路复用同一个 camera 驱动与同一个编码器，串行调用（内部有互斥）。
//
// 内存：JPEG 缓冲常驻 PSRAM（VGA 约 30~60KB，配额 128KB），不申请内部 RAM；
//       编码临时缓冲由 esp32-camera 在 PSRAM 分配。

// 初始化：记住 camera 实例并分配 PSRAM JPEG 缓冲。
// camera 由板级持有（`camera_`），本模块只借用，不负责释放。
// 失败返回 false（网页拍照按钮会回错误，不影响其它功能）。
bool LocalPhotoInit(Esp32Camera* camera, size_t capacity_bytes);

// 抓一帧并编码成 JPEG（阻塞约 200ms，在 httpd 任务里调用即可）。成功返回 true。
bool LocalPhotoCapture();

// 最近一次成功的 JPEG 数据/长度（未拍过时返回 nullptr/0）。
const uint8_t* LocalPhotoData();
size_t LocalPhotoSize();
```

- [ ] **步骤 2：创建 `local_photo.cc`**

> 注意：本模块**不打 ESP_LOG**（项目约定：失败通过返回值/HTTP 响应上报，避免日志污染 UART0 与 Arduino 指令流）。

```cpp
#include "local_photo.h"

#include <esp_heap_caps.h>

#include <mutex>

#include "esp32_camera.h"

namespace {
Esp32Camera* s_camera = nullptr;
uint8_t* s_jpeg = nullptr;   // PSRAM 常驻
size_t s_capacity = 0;
size_t s_len = 0;
// 与 AI 拍照串行：两条路共用同一个 camera 驱动与 current_fb_
std::mutex s_mtx;
}  // namespace

bool LocalPhotoInit(Esp32Camera* camera, size_t capacity_bytes) {
    if (camera == nullptr || capacity_bytes == 0) {
        return false;
    }
    s_camera = camera;
    s_jpeg = static_cast<uint8_t*>(
        heap_caps_malloc(capacity_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_jpeg == nullptr) {
        return false;  // 网页拍照不可用，其余功能不受影响
    }
    s_capacity = capacity_bytes;
    s_len = 0;
    return true;
}

bool LocalPhotoCapture() {
    if (s_camera == nullptr || s_jpeg == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(s_mtx);
    // Capture() 丢弃旧帧取最新帧，并顺带更新 LCD 预览（与 AI 拍照行为一致）
    if (!s_camera->Capture()) {
        return false;
    }
    size_t len = 0;
    if (!s_camera->EncodeCurrentFrameToJpeg(s_jpeg, s_capacity, len)) {
        return false;
    }
    s_len = len;
    return true;
}

const uint8_t* LocalPhotoData() { return (s_len > 0) ? s_jpeg : nullptr; }
size_t LocalPhotoSize() { return s_len; }
```

- [ ] **步骤 3：板级接入（初始化 + 注入回调）**

`compact_wifi_board_s3cam_airobot.cc` 顶部 include 区追加：

```cpp
#include "local_photo.h"
```

找到 `InitializeUploadServer()` 的定义（用下面命令定位），在它的 `StartUploadServer(...)` 调用**之前**插入：

```sh
grep -n -A 12 "void InitializeUploadServer" main/boards/bread-compact-wifi-s3cam-airobot/compact_wifi_board_s3cam_airobot.cc
```

```cpp
    // 网页拍照：JPEG 常驻 PSRAM（128KB 配额，VGA JPEG 约 30~60KB），失败不影响其它功能
    LocalPhotoInit(camera_, 128 * 1024);
    SetCameraWebApi(CameraWebApi{
        .take_photo = []() -> bool { return LocalPhotoCapture(); },
        .data = []() -> const uint8_t* { return LocalPhotoData(); },
        .size = []() -> size_t { return LocalPhotoSize(); },
    });
```

- [ ] **步骤 4：编译验证**

```sh
source ~/esp/v6.0.2/esp-idf/export.sh
python3 scripts/build.py bread-compact-wifi-s3cam-airobot --name bread-compact-wifi-s3cam-airobot
```
预期：编译通过；`local_photo.cc` 被 `file(GLOB ...)` 自动纳入（无需改 CMake）。

- [ ] **步骤 5：Commit**

```bash
git add main/boards/bread-compact-wifi-s3cam-airobot/local_photo.h \
        main/boards/bread-compact-wifi-s3cam-airobot/local_photo.cc \
        main/boards/bread-compact-wifi-s3cam-airobot/compact_wifi_board_s3cam_airobot.cc
git commit -m "feat: 新增板级网页拍照服务(JPEG 常驻 PSRAM, 不占内部 SRAM)"
```

### 任务 C3：HTTP 接口 `/photo/take` 与 `/photo.jpg`

**文件：**
- 修改：`main/boards/bread-compact-wifi-s3cam-airobot/http_upload_server.h`
- 修改：`main/boards/bread-compact-wifi-s3cam-airobot/http_upload_server.cc`（静态回调 + 两个 handler + 注册）

- [ ] **步骤 1：头文件加接口（放在 `UnoWebApi` 之后，风格保持一致）**

顶部 include 区补 `#include <cstddef>` `#include <cstdint>`，然后追加：

```cpp
// 网页拍照回调：由板级注入（实现见 local_photo.h）。
// take_photo: 抓一帧并编码成 JPEG（阻塞约 200ms，在 httpd 任务里执行）
// data/size: 最近一次成功的 JPEG（PSRAM 常驻，未拍过时返回 nullptr/0）
struct CameraWebApi {
    std::function<bool()> take_photo;
    std::function<const uint8_t*()> data;
    std::function<size_t()> size;
};

// 注入拍照回调；之后 GET /photo.jpg 取图、POST /photo/take 触发拍照。
void SetCameraWebApi(const CameraWebApi& api);
```

- [ ] **步骤 2：实现静态存储与注入函数**

在 `static UnoWebApi s_uno_api;` 之后追加：

```cpp
// 网页拍照回调(由板级 SetCameraWebApi 注入)
static CameraWebApi s_camera_api;
```

在 `void SetUnoWebApi(const UnoWebApi& api) { s_uno_api = api; }` 之后追加：

```cpp
void SetCameraWebApi(const CameraWebApi& api) { s_camera_api = api; }
```

- [ ] **步骤 3：两个 handler（加在 `HandleUnoPost` 之后）**

```cpp
// GET /photo.jpg：把最近一次拍摄的 JPEG 直接回给浏览器（同源，页面用 <img> 显示）。
// 大响应由 httpd 分块发送，不做整块拷贝，因此不额外占用大块堆内存。
static esp_err_t HandlePhotoJpeg(httpd_req_t* req) {
    SetCors(req);
    const uint8_t* data = s_camera_api.data ? s_camera_api.data() : nullptr;
    const size_t len = s_camera_api.size ? s_camera_api.size() : 0;
    if (data == nullptr || len == 0) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "no photo yet");
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");  // 每次都要最新那张
    return httpd_resp_send(req, reinterpret_cast<const char*>(data), len);
}

// POST /photo/take：触发一次拍照。约 200ms 阻塞 httpd 任务(含 WS 推送延迟)，
// 按钮是手动触发，可接受。
static esp_err_t HandlePhotoTake(httpd_req_t* req) {
    const size_t body_len = req->content_len;
    if (body_len > 0) {  // 丢弃可选 body，避免残留导致连接复用异常
        char drain[64];
        while (httpd_req_recv(req, drain, sizeof(drain)) > 0) {}
    }
    SetCors(req);
    if (!s_camera_api.take_photo) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera unavailable");
        return ESP_FAIL;
    }
    if (!s_camera_api.take_photo()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "capture failed");
        return ESP_FAIL;
    }
    char body[64];
    snprintf(body, sizeof(body), "{\"ok\":true,\"size\":%u}",
             static_cast<unsigned>(s_camera_api.size ? s_camera_api.size() : 0));
    return SendJson(req, body);
}
```

- [ ] **步骤 4：注册路由（`StartHttpServer()` 内，跟随现有 `httpd_uri_t` 风格）**

```cpp
    httpd_uri_t photo_jpeg_uri = {
        .uri = "/photo.jpg", .method = HTTP_GET, .handler = HandlePhotoJpeg, .user_ctx = nullptr,
    };
    httpd_uri_t photo_take_uri = {
        .uri = "/photo/take", .method = HTTP_POST, .handler = HandlePhotoTake, .user_ctx = nullptr,
    };
```

紧接着现有 `httpd_register_uri_handler(server, &xxx_uri);` 一串之后追加：

```cpp
    httpd_register_uri_handler(server, &photo_jpeg_uri);
    httpd_register_uri_handler(server, &photo_take_uri);
```

并确认 handler 总数：`grep -c "httpd_register_uri_handler" main/boards/bread-compact-wifi-s3cam-airobot/http_upload_server.cc`
若超过 `cfg.max_uri_handlers = 20`，把它调到 22。

- [ ] **步骤 5：编译验证**

```sh
source ~/esp/v6.0.2/esp-idf/export.sh
python3 scripts/build.py bread-compact-wifi-s3cam-airobot --name bread-compact-wifi-s3cam-airobot
```

- [ ] **步骤 6：Commit**

```bash
git add main/boards/bread-compact-wifi-s3cam-airobot/http_upload_server.h \
        main/boards/bread-compact-wifi-s3cam-airobot/http_upload_server.cc
git commit -m "feat: 新增 /photo/take 与 /photo.jpg 接口供网页拍照"
```

### 任务 C4：网页拍照按钮与照片显示

**文件：**
- 修改：`main/boards/bread-compact-wifi-s3cam-airobot/web/index.html`（`panel-uno` 内、`<details class="logbox">` 之前插入 UI；`<script>` 里加 JS）

- [ ] **步骤 1：UI（放在「机器人控制」面板内，系统日志折叠区之前）**

```html
      <!-- 网页拍照：设备侧抓帧 → JPEG 存 PSRAM → 本页 <img> 直接从 /photo.jpg 拉取显示 -->
      <div class="row" style="margin-top:10px">
        <button class="btn" id="photoBtn" onclick="takePhoto()">📷 拍照</button>
        <span class="hint">拍完照片直接显示在下方；想让 AI 描述画面，对设备说「看看我」即可（AI 会自己拍一张）</span>
      </div>
      <div id="photoBox" style="display:none;margin-top:8px">
        <img id="photoImg" alt="照片" style="max-width:100%;border-radius:8px">
      </div>
```

- [ ] **步骤 2：JS（放在 `clearDeviceLog` 附近）**

```js
    // ===== 网页拍照 =====
    // 与 AI 拍照共用同一个 camera 驱动，设备侧已互斥；用户手动触发，不需要防抖。
    function takePhoto() {
      const btn = document.getElementById('photoBtn');
      const old = btn ? btn.textContent : '';
      if (btn) { btn.disabled = true; btn.textContent = '📷 拍摄中…'; }
      fetch('/photo/take', { method: 'POST' })
        .then(function (r) { return r.ok ? r.json() : Promise.reject(new Error('HTTP ' + r.status)); })
        .then(function () {
          const img = document.getElementById('photoImg');
          const box = document.getElementById('photoBox');
          if (img && box) { img.src = '/photo.jpg?t=' + Date.now(); box.style.display = ''; }
        })
        .catch(function (e) { alert('拍照失败：' + e.message); })
        .finally(function () {
          if (btn) { btn.disabled = false; btn.textContent = old || '📷 拍照'; }
        });
    }
```

- [ ] **步骤 3：语法检查**

```sh
node --check <(sed -n '/<script>/,/<\/script>/p' main/boards/bread-compact-wifi-s3cam-airobot/web/index.html | sed '1d;$d')
```
预期：无输出。

- [ ] **步骤 4：Commit**

```bash
git add main/boards/bread-compact-wifi-s3cam-airobot/web/index.html
git commit -m "feat(web): 机器人控制面板新增拍照按钮与照片显示"
```

### 任务 C5：真机验证

**文件：** 无（验证任务）

- [ ] **步骤 1：功能验证**

烧录后打开页面「🎮 机器人控制」→ 点「📷 拍照」。
预期：按钮短暂显示「拍摄中…」→ 下方出现照片；照片内容与摄像头朝向一致（**颜色正常**，不出现红蓝互换）；LCD 上同时出现预览图（走的是现有 `Capture()` 预览路径）；日志里能看到 `Esp32Camera: Captured frame ...`。

- [ ] **步骤 2：内存验证（关键）**

拍照前后各抄一次 `SystemInfo: free sram / minimal sram`。
预期：`minimal sram` 与阶段 B4 的拍照结果**同一量级**（网页拍照不该比 AI 拍照更吃内存——它不发云端、不带 HTTP 上传）。若明显更低，说明 JPEG 常驻缓冲或编码临时缓冲落到了内部 RAM，需检查 `heap_caps_malloc` 的 caps。

- [ ] **步骤 3：交叉验证（AI 拍照仍正常）**

点一次网页拍照，然后对 AI 说「拍个照片看看」。
预期：AI 拍照仍成功（互斥生效），不重启。若出现 AI 拍照偶发失败，把 `LocalPhotoCapture()` 的互斥改成 `std::recursive_mutex` 之外的做法——更稳的是**给互斥加超时**：把 `std::lock_guard` 换成 `std::unique_lock` + `try_lock_for(std::chrono::milliseconds(300))`，拿不到锁直接返回失败（HTTP 500），**绝不让主任务长时间等待**。

---

# 阶段 D：收尾

### 任务 D1：回归测试（含 1 处必须的测试更新）

**文件：**
- 修改：`scripts/tests/test_log_capture.py`（**仅 1 个测试受影响**）
- 创建：`scripts/tests/test_airobot_log_persist.py`
- 创建：`scripts/tests/test_airobot_web_photo.py`

- [ ] **步骤 1：更新 `test_ring_buffer_is_static`（缓冲已迁到 `.noinit` 结构体）**

把该测试体替换为（**意图不变：不得动态分配；落点从 `.bss` 数组改为 `.noinit` 结构体**）：

```python
    def test_ring_buffer_is_static(self):
        """环形缓冲必须是静态内存（内部 RAM），临界区内不能碰堆/PSRAM。

        2026-09-17 起缓冲从 .bss 数组迁到 .noinit 结构体（软重启后保留崩溃前日志），
        因此断言对象改为 RingStore + .noinit 段属性；"不得动态分配"的约束不变。
        """
        self.assertIn('__attribute__((section(".noinit")))', self.src)
        self.assertIn("RingStore s_store", self.src)
        self.assertIn("char ring[kRingSize];", self.src)
        body = _strip_comments(self.src)
        for bad in ("malloc(", "heap_caps_malloc(", "new ", "calloc("):
            self.assertNotIn(bad, body, f"日志缓冲不得动态分配（{bad}）")
        self.assertIn("constexpr size_t kRingSize", self.src)
```

- [ ] **步骤 2：创建 `scripts/tests/test_airobot_log_persist.py`**

```python
"""测试崩溃日志留存（log_capture 的 .noinit 持久化）的回归防护。

背景：2026-09-17 前，日志环形缓冲在 .bss，设备一重启就清零，导致"拍照片偶发重启"
完全拿不到现场。改为 .noinit 段后，软重启(panic/看门狗/OTA/esp_restart)仍能保留
崩溃前的日志，网页重连即可看到。

必须固化的约束：
1. 缓冲必须在 .noinit 段（否则重启即丢，功能等于没做）；
2. 必须有三重校验(magic/version/size/head)并在冷启动时清空
   （否则会把上电时的随机 DRAM 当成日志显示给用户）；
3. 只有"值得保留"的复位原因才保留旧日志（上电/未知必须丢弃）；
4. 重启分隔行必须带复位原因（用户据此判断是内存/看门狗/欠压）。
"""

import os
import re
import unittest

BOARD_DIR = ("main", "boards", "bread-compact-wifi-s3cam-airobot")


def _read(*parts):
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", *parts)
    with open(path, encoding="utf-8") as f:
        return f.read()


def _strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    return src


class TestLogPersist(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.src = _read(*BOARD_DIR, "log_capture.cc")
        cls.header = _read(*BOARD_DIR, "log_capture.h")
        m = re.search(r"void LogCaptureInit\(\)\s*\{(.*?)\n\}", cls.src, re.S)
        assert m is not None, "未找到 LogCaptureInit 定义"
        cls.init = _strip_comments(m.group(1))

    def test_ring_lives_in_noinit(self):
        """缓冲与游标必须在 .noinit 段，软重启后才留得住崩溃日志。"""
        self.assertIn('__attribute__((section(".noinit")))', self.src)
        self.assertIn("RingStore s_store", self.src)
        # RingWrite / LogCapturePull 都必须操作同一个持久化结构
        self.assertIn("s_store.ring", self.src)
        self.assertIn("s_store.total", self.src)
        self.assertNotIn("char s_ring[kRingSize];", self.src,
                         "旧的 .bss 数组应已删除，避免两份状态不一致")

    def test_cold_boot_is_validated(self):
        """必须有 magic/version/size/head 校验，冷启动要清空（不能显示随机 RAM）。"""
        for token in ("kRingMagic", "kRingVersion", "s_store.magic", "s_store.size",
                      "s_store.head < kRingSize"):
            self.assertIn(token, self.init, f"LogCaptureInit 缺少校验项 {token}")
        self.assertIn("s_store.head = 0", self.init, "冷启动必须清空游标")
        self.assertIn("s_store.total = 0", self.init, "冷启动必须重置序号")

    def test_reset_reason_gates_and_reports(self):
        """保留与否必须由复位原因决定，且分隔行要带上原因文字。"""
        self.assertIn("esp_reset_reason()", self.init)
        self.assertIn("ShouldKeepPreviousLog(reason)", self.init)
        self.assertIn("ResetReasonText(reason)", self.init)
        # 关键复位原因必须可区分（内存问题/看门狗/欠压）
        for r in ("ESP_RST_PANIC", "ESP_RST_INT_WDT", "ESP_RST_TASK_WDT", "ESP_RST_BROWNOUT"):
            self.assertIn(r, self.src, f"复位原因文案应覆盖 {r}")
        self.assertIn("esp_system.h", self.src, "esp_reset_reason 需要 esp_system.h")

    def test_poweron_is_discarded(self):
        """上电冷启动绝不能在保留名单里。"""
        m = re.search(r"bool ShouldKeepPreviousLog\(esp_reset_reason_t r\)\s*\{(.*?)\n\}",
                      self.src, re.S)
        self.assertIsNotNone(m, "未找到 ShouldKeepPreviousLog")
        body = _strip_comments(m.group(1))
        self.assertNotIn("ESP_RST_POWERON", body)
        self.assertNotIn("default", body, "不得用 default 放行未知原因")

    def test_separator_written_through_ring_write(self):
        """分隔行必须走 RingWrite（不能调 ESP_LOGx/printf，会递归自锁）。"""
        self.assertIn("RingWrite(line, len)", self.init)
        self.assertNotIn("ESP_LOGI", self.init)
        self.assertIsNone(re.search(r"(?<![A-Za-z_])printf\s*\(", self.init))

    def test_clear_api_exposed(self):
        self.assertIn("void LogCaptureClear()", self.src)
        self.assertIn("void LogCaptureClear();", self.header)

    def test_web_can_clear_device_buffer(self):
        server = _read(*BOARD_DIR, "http_upload_server.cc")
        web = _read(*BOARD_DIR, "web", "index.html")
        self.assertIn('strcmp(action, "log_clear")', server)
        self.assertIn("LogCaptureClear()", server)
        self.assertIn("clearDeviceLog", web)

    def test_header_documents_panic_blind_spot(self):
        """头文件必须写清：panic/backtrace 不走钩子，网页看不到，必须接串口。"""
        self.assertIn("panic", self.header.lower())
        self.assertIn("UART0", self.header)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **步骤 3：创建 `scripts/tests/test_airobot_web_photo.py`**

```python
"""测试网页拍照链路（/photo/take + /photo.jpg + 前端按钮）的回归防护。

背景：网页拍照复用 Esp32Camera 已捕获的帧做 JPEG 编码，避免 fb_count=2 —— 后者会让
cam_hal 多占 ~30KB DMA 内部 RAM，与本板"内部 SRAM 只有几十 KB"的现实直接冲突。

必须固化的约束：
1. 编码必须复用 Capture() 的帧（EncodeCurrentFrameToJpeg），不得新增帧缓冲；
2. JPEG 必须是 PSRAM 常驻，不得落内部 RAM；
3. /photo.jpg 必须带 no-store（否则浏览器缓存旧图，用户以为拍照坏了）；
4. 前端按钮必须走 /photo/take 且用时间戳刷新 <img>，否则同 URL 不重载。
"""

import os
import re
import unittest

BOARD_DIR = ("main", "boards", "bread-compact-wifi-s3cam-airobot")


def _read(*parts):
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", *parts)
    with open(path, encoding="utf-8") as f:
        return f.read()


class TestWebPhoto(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = _read(*BOARD_DIR, "http_upload_server.cc")
        cls.header = _read(*BOARD_DIR, "http_upload_server.h")
        cls.web = _read(*BOARD_DIR, "web", "index.html")
        cls.board = _read(*BOARD_DIR, "compact_wifi_board_s3cam_airobot.cc")
        cls.local = _read(*BOARD_DIR, "local_photo.cc")

    def test_routes_registered(self):
        for uri in ('"/photo.jpg"', '"/photo/take"'):
            self.assertIn(uri, self.server, f"缺少路由 {uri}")
        self.assertIn("httpd_register_uri_handler(server, &photo_jpeg_uri)", self.server)
        self.assertIn("httpd_register_uri_handler(server, &photo_take_uri)", self.server)

    def test_jpeg_not_cached(self):
        m = re.search(r"static esp_err_t HandlePhotoJpeg\(.*?\n\}", self.server, re.S)
        self.assertIsNotNone(m, "未找到 HandlePhotoJpeg")
        body = m.group(0)
        self.assertIn('"image/jpeg"', body)
        self.assertIn("no-store", body, "必须禁缓存，否则浏览器显示的是上一张")

    def test_reuses_captured_frame(self):
        """必须复用 Capture() 的帧，不得自己再 fb_get（会与 fb_count=1 的帧池抢帧）。"""
        self.assertIn("EncodeCurrentFrameToJpeg", self.local)
        self.assertIn("->Capture()", self.local)
        self.assertNotIn("esp_camera_fb_get", self.local,
                         "不得自己取帧：fb_count=1 时 current_fb_ 被 Esp32Camera 持有，会阻塞")

    def test_jpeg_buffer_in_psram(self):
        """JPEG 常驻缓冲必须在 PSRAM，内部 SRAM 留给音频/网络。"""
        self.assertIn("MALLOC_CAP_SPIRAM", self.local)
        self.assertIn("heap_caps_malloc", self.local)

    def test_no_frame_buffer_added(self):
        """fb_count 必须保持 1：改 2 会多占 ~30KB DMA 内部 RAM。"""
        self.assertIn("config.fb_count = 1;", self.board)

    def test_board_injects_camera_api(self):
        self.assertIn("SetCameraWebApi", self.board)
        self.assertIn("LocalPhotoInit(camera_", self.board)
        self.assertIn("void SetCameraWebApi(const CameraWebApi& api);", self.header)

    def test_frontend_button(self):
        self.assertIn('id="photoBtn"', self.web)
        self.assertIn("function takePhoto()", self.web)
        self.assertIn("'/photo/take'", self.web)
        # 同 URL 图片不会重新加载，必须带时间戳
        self.assertIn("'/photo.jpg?t=' + Date.now()", self.web)

    def test_take_handler_drains_body(self):
        """POST body 要读掉，否则 keep-alive 复用连接时残留会导致解析异常。"""
        m = re.search(r"static esp_err_t HandlePhotoTake\(.*?\n\}", self.server, re.S)
        self.assertIsNotNone(m, "未找到 HandlePhotoTake")
        self.assertIn("httpd_req_recv", m.group(0))


if __name__ == "__main__":
    unittest.main()
```

- [ ] **步骤 4：运行测试**

```sh
cd /Users/liuguoqing/data/www/wwwroot/xiaozhi-esp32-airobot
python3 -m unittest discover -s scripts/tests -v 2>&1 | tail -40
```
预期：全部 PASS（含被更新的 `test_log_capture.py`）。若有 FAIL，先修代码或测试断言，不允许跳过。

- [ ] **步骤 5：Commit**

```bash
git add scripts/tests/test_log_capture.py scripts/tests/test_airobot_log_persist.py \
        scripts/tests/test_airobot_web_photo.py
git commit -m "test: 增加崩溃日志留存与网页拍照的静态回归, 更新缓冲段位置断言"
```

### 任务 D2：文档更新

**文件：**
- 修改：`main/boards/bread-compact-wifi-s3cam-airobot/README.md`

- [ ] **步骤 1：修订「实时日志与下位机指令（网页查看）」章节**

在该章节内补充/修正：

1. **内存开销段落**改为：
   > **内存开销**：环形缓冲 4KB（放在 `.noinit` 段，**不额外增加占用**，只是从 `.bss` 平移）+ 拉取缓冲 0.5KB；另有 httpd 任务栈 8KB、一条 WS 连接。
   > 本板内部 SRAM（**不是 PSRAM**）实测空载 `free sram` 仅 20~25KB、历史最低 `minimal sram` 曾低至 6KB —— 因此**做拍照等重内存操作时建议先关掉网页**（页面常驻 WS 会一直占用内部 SRAM）。
   > 注意 `SystemInfo: free sram` 打印的是 `MALLOC_CAP_INTERNAL`，与 8MB PSRAM 无关，别被"内存很大"误导。
2. 新增小节「**崩溃日志怎么看**」：
   - 设备**软重启后**（panic/看门狗/OTA/`self.reboot`）环形缓冲保留，网页日志里会出现分隔行：
     `================ 设备重启 #N：PANIC 异常或 abort(多为内存不足/空指针/栈溢出) ================`
     这行的**上方**就是崩溃前的日志；`上电启动(冷启动)` 时缓冲清空（正常）。
   - 重启原因文案直接指向方向：`PANIC` 多为内存/空指针/栈溢出；`中断看门狗` 多为临界区过长或中断被饿死；`任务看门狗` 多为某任务不让出 CPU；`欠压重启` 是供电问题。
   - **panic 的 Guru Meditation / backtrace / `stack overflow in task xxx` 永远不在网页里**：它们由 IDF panic handler 用 ROM printf 直写 UART0，不经过 `esp_log_set_vprintf` 钩子。**要抓 backtrace 必须接 USB 串口**（拔掉 Arduino 接线），且**不需要**打开「同时输出到串口」开关——panic 输出本来就写 UART0。
   - 解析方法见踩坑 14 的 `addr2line` 命令。
3. 在按钮说明里加上「🗑 清空设备缓冲」（只清设备侧历史）。
4. 拉取频率说明：日志区**展开 1s / 收起 3s**，离开「机器人控制」面板停止（收起时仍拉是因为下位机指令记录与系统日志同源）。

- [ ] **步骤 2：新增踩坑 16**

```markdown
### 16. AI 拍照片偶发重启（内部 SRAM 耗尽，已修复/已缓解）

**现象**：加网页日志功能后，让 AI 拍照片，有时拍照成功后 ESP32 直接重启；网页日志在
`HttpClient: Established new connection ... cost=40` 一行后戛然而止，下一行就是开机日志，
没有任何错误信息（`SystemInfo` 显示 `free sram: 24579 minimal sram: 6175`）。

**根因（两层）**：

1. **内部 SRAM 余量被 web 功能吃掉**：本板 `free sram`（`MALLOC_CAP_INTERNAL`，非 PSRAM）
   空载只有 20~25KB。web 日志功能常驻占用 = 4KB 环形缓冲 + 0.5~1KB 拉取缓冲
   + httpd 任务栈 8KB + 一条 WS 连接；而**拍照上传那一刻**主任务还要开一条到
   `api.xiaozhi.me` 的 HTTP 连接、创建 JPEG 编码线程（pthread 默认栈 3KB），
   同时 LVGL 任务在把 640×480 RGB565 预览图缩放渲染到 240×240 —— 多方并发抢内部 SRAM，
   于是"有时够、有时不够"。
2. **lwIP/WiFi 缓冲不能落 PSRAM**：`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` 默认关闭，
   8MB PSRAM 帮不上忙；`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=2048` 又把所有 ≤2KB 的分配
   塞进内部 RAM，加剧碎片。

**为什么完全看不到崩溃原因（重要教训）**：`log_capture` 用 `esp_log_set_vprintf()` 只接管
`ESP_LOGx`；panic 的 `Guru Meditation`、backtrace、`abort()` 消息由 IDF panic handler 用
ROM printf **直写 UART0**，不经过该钩子 —— 所以网页日志**永远看不到崩溃原因**；
而设备一重启，`.bss` 里的环形缓冲清零，崩溃前的日志也一起没了。

**修复**：
- 环形缓冲迁到 `.noinit` 段（软重启保留崩溃前日志），并打印 `esp_reset_reason()` 分隔行；
- 本板 `config.json` 打开 `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`，并把
  `LWIP_TCP_SND_BUF_DEFAULT` / `LWIP_TCP_WND_DEFAULT` 从 5760 收到 2920、`LWIP_MAX_SOCKETS` 16→10；
- 日志拉取路径瘦身（原地 UTF-8 清洗、512B 缓冲、WS 会话上限 4→2）、前端拉取频率分层（1s/3s）。

**排查手段固化**：以后再遇到"偶发重启"，先看网页日志的重启分隔行拿复位原因，
再接 USB 串口抓 backtrace（panic 输出不受网页日志开关影响）。
```

- [ ] **步骤 3：新增「网页拍照」章节**

```markdown
## 网页拍照（按钮 + 页面显示照片）

「🎮 机器人控制」面板点 **📷 拍照** → 照片直接显示在按钮下方。

- **实现**：`POST /photo/take` 触发 `LocalPhotoCapture()`：复用 `Esp32Camera::Capture()`
  已抓到的帧（**不新开帧缓冲**）→ JPEG 编码到 **PSRAM 常驻缓冲**（128KB 配额）→
  页面用 `<img src="/photo.jpg?t=时间戳">` 拉取显示。
- **为什么不多开一块帧缓冲**：`cam_hal` 每帧要 30720 字节 **DMA 内部 RAM**，
  `fb_count` 从 1 改 2 会多占 ~30KB 内部 SRAM —— 本板内部 SRAM 只有几十 KB，不能这么花。
- **与 AI 拍照的关系**：两条路复用同一个 camera 驱动，设备侧已加互斥。
  同一时刻点按钮又喊 AI 拍照，可能有一次失败（返回 500/回执报错），**不会重启**，重试即可。
- **颜色不对**（红蓝互换）说明字节序处理被跳过：`EncodeCurrentFrameToJpeg()` 复用
  `swap_bytes_enabled_` 逻辑，改这块时注意与 `Explain()` 保持一致。
```

- [ ] **步骤 4：更新「与上游合并提示」**

补充一句：本板新增 `local_photo.*` 由 `main/CMakeLists.txt` 的 `file(GLOB boards/<BOARD_DIR>/*.cc)`
自动纳入，无需在核心 CMake 里登记。

- [ ] **步骤 5：Commit**

```bash
git add main/boards/bread-compact-wifi-s3cam-airobot/README.md
git commit -m "docs: 补充崩溃日志查看方式、内部 SRAM 排查经验与网页拍照说明"
```

---

## 自检

**1. 规格覆盖度**

| 需求 | 对应任务 |
|---|---|
| web 日志要能看到真正出错时的日志 | A1（`.noinit` 留存 + 重启分隔行）、A2（验证）、D1（`test_airobot_log_persist.py`） |
| 判断"到底是怎么挂的" | A1（`ResetReasonText`：PANIC/看门狗/欠压）| 
| 优化内存使用 | B1（拉取路径瘦身）、B2（前端分层）、B3（lwIP/WiFi 下沉 PSRAM）、B4（验证） |
| 保住 web 日志功能（且不干扰 Arduino） | A1/B1/B2 均保留"不落 UART0"语义；B2 保留指令记录同源刷新 |
| 不动分区 | 未使用 core dump；`partitions/*` 零改动 |
| 尽量只在本板范围改 | 仅阶段 C 需 `main/boards/common/esp32_camera.*` 纯增量（已标注待确认） |
| 后续要加：网页拍照 + 页面显示照片 | 阶段 C（C1~C5） |
| 无 placeholder | 所有步骤均给出可直接落地的代码/命令/预期输出 |

**2. 占位符扫描**：无 "TODO/待定/类似任务 N"；阶段 C 顶部对"需确认的共享改动"给出了明确的跳过条件。

**3. 类型一致性**：
- `RingStore` 字段（`magic/version/size/head/total/boots/ring`）在 A1 定义，B1 未引用；
- `LogCaptureClear()` 在 A1 声明并实现，B1 的 `log_clear` 分支与 D1 测试一致引用；
- `LocalPhotoInit(Esp32Camera*, size_t)` 在 C2 头文件与实现签名一致，C2 步骤 3 的注入调用一致；
- `CameraWebApi` 的 3 个成员（`take_photo/data/size`）在 C3 声明、C2 注入、C3 handler 消费，命名一致；
- `EncodeCurrentFrameToJpeg(uint8_t*, size_t, size_t&)` 在 C1 声明/实现/C2 调用一致。

**4. 已知残余风险**
- `.noinit` 在个别复位路径下可能不保留 → A1 已写 `RTC_NOINIT_ATTR` 回退方案；
- `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` 可能影响音频/延迟 → B3 步骤 3 强制回归；
- 网页拍照与 AI 拍照并发会有一方失败 → C5 步骤 3 给出 `try_lock_for` 兜底并写入文档；
- panic backtrace 仍拿不到（用户要求不改分区/核心）→ 已在 README 固化"接串口抓"的流程。

---

## 执行交接

**计划已完成并保存到 `docs/superpowers/plans/2026-09-17-airobot-log-persist-and-memory-web-photo.md`。**

确认事项（开工前需要你拍板）：

1. **阶段 A + B**（崩溃日志留存 + 内存回收）：纯板级、零共享改动，**可以直接开工**；
2. **阶段 C**（网页拍照）：需要 `main/boards/common/esp32_camera.h/.cc` 增加一个
   **纯新增**方法 `EncodeCurrentFrameToJpeg()`（不动任何现有函数）——是否同意？
   若暂不同意，先执行 A/B，阶段 C 留待后续。

执行方式（二选一）：

1. **内联执行** —— 本会话按 executing-plans 批量推进，每个阶段末端设检查点（推荐，改动集中在本板目录）；
2. **子代理驱动** —— 每个任务派一个新子代理 + 任务间审查（适合你希望我并行推进时）。
