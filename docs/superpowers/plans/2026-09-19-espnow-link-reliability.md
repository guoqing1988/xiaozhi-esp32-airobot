# 实施计划：ESP-NOW 链路可靠性 + 事件播报修复

- 日期：2026-09-19
- 分支：`feature/espnow-home-control`
- 关联设计：`docs/superpowers/specs/2026-09-18-espnow-home-control-design.md`（§11 未完成事项 1/2/4/5/6）
- 两侧固件都要重编烧录（主控 + 节点），只刷一侧不会瘫痪，但拿不到 ACK 保障

## 1. 要解决的问题

| # | 现象 | 影响 |
|---|---|---|
| 1 | 节点触发 `say motion` 后主控喇叭不响（完全没声音） | 演示脚本②③失效 |
| 2 | 语音控灯偶发"要等很久才执行"或"指令没反应" | 演示脚本①失灵 |
| 3 | 节点偶发被判离线（`online:false`） | AI 拒绝执行控制命令 |

## 2. 根因分析

### 2.1 播报不响（现象 1）

链路上有 4 个可能断点，逐一核对源码后：

1. **文件不存在（主要根因）**：`LocalMusicPlayer::PlayAnnounce()` 用 `fopen("/sdcard/announce/<name>.mp3")` 探测，失败即返回
   `false` → 板级打 `播报失败: 打不开 ...` 后跳过。而 `/sdcard/announce` 目录**过去从未创建**
   （`d20e556` 才把 `EnsureDir` 加进上传路径），旧固件的上传必定失败 → 卡上根本没有提示音文件。
   → 现场必须重编主控才能验证；本次再补"开机即建目录"，不再依赖"先上传一次"。
2. **播报冷却按"节点"而非"事件"**：`home_announce_ms_[node_id]` 只有一个时间戳。融合节点（`NODE_ID 3`）
   同时接四路传感器，`motion` 与 `beam` 在 10 秒内互相压制 → "播报时有时无"。
3. **播报被网络抖动掐断**：`PlayTask()` 的打断判定把 `Idle→Connecting` 也当成"用户交互"
   （播歌时是合理取舍），但 2~3 秒的播报一旦被误判就直接静音，现场观感就是"没响"。
4. 冷却 / Idle / 文件 / 播放器四道关卡**都有 INFO 日志**（`9a07e90` 已加，TAG=`ESP-NOW`），
   网页日志面板能直接看到卡在哪一步——本次保留并补细节。

### 2.2 控制命令延迟/丢失（现象 2、3）

主控是 STA，射频信道**由 AP 决定**；节点不连 AP、靠 hop 找主控信道（`ESP-NOW` 只能同信道通信）。
AP 触发 ACS（自动信道选择）换信道后：

- 节点仍停在旧信道，直到 `LOST_TIMEOUT_MS`（12s）才回 hop；
- hop 一圈 `13 × 2000ms = 26s`，而稳态 beacon 每 3000ms 一次 → 每个信道只有 ~67% 概率撞上，
  实测经常要转 2~3 圈；
- 这期间主控下发命令 → `esp_now_send()` **只表示"已入队"，不表示已送达**；
  主控侧既不检查发送回调（未注册 `send_cb`），也没有应用层 ACK/重传 →
  **命令静默打空**，用户看到"指令失败"或"延迟很久"（等节点转回来重新锁定）。
- 节点无周期性心跳，`evt` 只在其传感器值变化时才发 → 主控 15s 判离线，出现**假离线**。

### 2.3 官方依据（IDF v6.0.2 自带文档与头文件，本机已核对）

`~/esp/v6.0.2/esp-idf/docs/zh_CN/api-reference/network/esp_now.rst`：

- **发送失败原因**明确包含"设备的信道不相同"（原文），并建议：
  > "应用层并不一定可以总能接收到数据。如果需要，应用层**可在接收 ESP-NOW 数据时发回一个应答 (ACK) 数据**。
  > 如果接收 ACK 数据超时，则将**重新传输** ESP-NOW 数据。可以为 ESP-NOW 数据**设置序列号**，从而**删除重复的数据**。"
- **发送节流**：`esp_now_send()` 后应等发送回调返回再发下一条；回调运行在高优先级 Wi-Fi 任务，
  **不得做耗时操作**。
- **功耗**：`esp_now_set_wake_window()` 默认已是最大值，无需调整（本设计不动）。
- 相关 API（`components/esp_wifi/include/esp_now.h`）：
  `esp_now_register_send_cb()`（拿 MAC 层真实发送结果）、`esp_now_set_peer_rate_config()`（可选）。

本方案 `esp_now_switch_channel_tx()` / `esp_now_remain_on_channel()`（IDF v6 新增，S3 库有符号）
**暂不使用**：官方文档没有给出使用约束与示例，且 ROC 会让主控短暂离开 AP 信道（丢 AP 包、
可能打断音频流），收益不确定。节点 hop + ACK 重传已能兜住，且不引入未验证变量。

## 3. 方案（全部使用 IDF/Arduino 官方 API）

### 3.1 下行可靠性：应用层 ACK + 序号 + 非阻塞重传（官方文档建议）

**协议扩展（向后兼容，两侧任一没更新的后果可接受）**

| 方向 | 报文 | 说明 |
|---|---|---|
| 主控→节点 | `@n1#12 do light on 1` | 序号放在**信封**（`@n<id>#<seq>`），正文完全不变 |
| 节点→主控 | `@n1#12 ok light 1` | 回执带回同序号 |

为什么序号放信封而不是正文：旧节点固件用 `atoi("@n1#12 ...")` 仍得到 `1`，`strchr(p,' ')` 仍能找到
正文 → **旧节点照样能执行命令**；旧节点回执不带序号时，主控按"该节点有唯一在途命令"匹配即可。
两侧都不存在"只刷一边就彻底不能用"的硬失败。

**主控侧（`espnow_home.h/.cc`）**

1. `SendTo()` 内部生成序号、发送首包、登记 `PendingCmd`（每节点最多 1 条，新命令覆盖旧命令）。
2. 收到 `ok`/`err` → 清除该节点 pending（链路确认）。
3. `retry_timer_`（100ms，**只在有在途命令时运行**）驱动重传：150ms 间隔最多重发 4 次，
   仍无 ACK 则保留挂起至 7s（覆盖节点 hop 一圈 6.5s），期间一旦收到该节点任何上行
   （`info`/`evt`/心跳）→ 立即补发一次——节点重锁后命令自动补上，用户不必重说。
4. 注册 `esp_now_register_send_cb()`：累计 MAC 层发送结果；连续失败 ≥3 次 → 请求进入
   快速 beacon 窗口（大概率是信道变了）。回调里只写标志，不做重活（官方要求）。
5. 超时未确认的命令通过链路事件上报（板级打 TAG=`ESP-NOW` 日志），工具返回值明确
   说明"已下发/未确认"，不返回假成功。

**节点侧（`EspNowNode.ino`）**

1. 解析信封里的 `#<seq>`，回执原样带回；同一序号重复到达 → **只重发上次回执，不重复执行**
   （官方建议的"序列号去重"；对灯控幂等，但对 `speed+` 一类非幂等动作是必须的）。
2. 心跳：每 5s 一条 `@n3 evt hb 1`（走现有上行队列/合并/连发），消除主控假离线。

### 3.2 信道跟随：主控看护 + 快速 beacon（不依赖路由器）

1. 主控每次发 beacon 时用 `esp_wifi_get_channel()` 检查信道是否变化（官方 API）；
   变化 → 打链路事件日志 + 进入**快速 beacon 窗口**（500ms 周期，持续 8s）。
2. 节点侧：`HOP_INTERVAL_MS` 2000 → **500**（一圈 13×0.5 = 6.5s）、
   `LOST_TIMEOUT_MS` 12000 → **5000**（配合主控快速窗口，重锁从最坏 40s+ 压到 ~10s 内）。
3. 稳态 beacon 仍是 3000ms（省电），只在"信道变化 / 命令未确认 / 发送连续失败"时提速。

### 3.3 播报修复

1. `LocalMusicPlayer::ScanSongs()`（SD 挂载后调用）确保 `/sdcard/announce` 存在。
2. 播报冷却由"按节点"改为**按 (节点, 音频名)**（8 项定长表），`motion` 不再压制 `beam`。
3. 播报播放期间只认 `Idle→Listening` 为打断信号（`Connecting` 不算，网络重连不该掐断播报）；
   歌曲播放行为不变。
4. `self.home.announce` 与事件播报共用同一检查路径，返回值/日志都给出"文件不存在"的具体路径。

## 4. 改动文件

| 文件 | 改动 |
|---|---|
| `espnow_home.h/.cc` | 序号信封解析、`PendingCmd` 表、重传定时器、`send_cb`、信道看护、快速 beacon 窗口、链路事件回调 |
| `compact_wifi_board_s3cam_airobot.cc` | 链路事件日志、播报冷却按 (节点,名字)、控制工具返回文本 |
| `local_music_player.h/.cc` | announce 目录自建、播报播放模式（Connecting 不打断） |
| `arduino/EspNowNode/EspNowNode.ino` | 序号去重 + 回执带序号、心跳、hop 500ms / 失联 5s |
| `scripts/tests/test_espnow_home_protocol.py` | 序号信封、重传策略、心跳、hop 参数、播报冷却维度等断言 |
| 本文 + spec §11 + 两侧 README | 回写方案与真机验证步骤 |

## 5. 验证结果（2026-09-19 执行）

| # | 项目 | 命令 | 结果 |
|---|---|---|---|
| 1 | 宿主测试 | `python3 -m unittest scripts.tests.test_espnow_home_protocol` | ✅ **99 tests OK** |
| 2 | 主控改动文件编译 | `-fsyntax-only` + `build/compile_commands.json` 里 IDF v6.0.2 的完整参数（含 `-Werror`） | ✅ 三个文件全过（`espnow_home.cc`、`compact_wifi_board_s3cam_airobot.cc`、`local_music_player.cc`） |
| 3 | 主控全量编译 | `python3 scripts/build.py bread-compact-wifi-s3cam-airobot --name bread-compact-wifi-s3cam-airobot` | ⛔ 被环境阻塞（见 §5.1），与本次改动无关 |
| 4 | 节点编译 | `arduino-cli compile --fqbn esp32:esp32:esp32s3 .` | ✅ 成功（Flash 896138 B / 68%，RAM 46060 B / 14%） |

编译期抓到一个真实错误并已修：`++volatile int`（C++20 起 `-Werror=volatile`）
→ 改成 `std::atomic<int>` / `std::atomic<bool>`，对"`send_cb` 跑在 Wi-Fi 任务、定时器跑在 esp_timer 任务"
这个跨任务访问语义也更正确。

### 5.1 主控全量编译为什么失败（环境问题，非本次改动）

```
CMake Error at tools/cmake/project.cmake:789 (message): Missing required kconfig option after retry.
```

- 失败在 **CMake configure 阶段**，早于任何源码编译；根因是 IDF v6.0.2 的组件管理器把
  `espressif/esp_h264` 的 Kconfig 需求记为"引用了不存在的符号 `ESP_VIDEO_USE_CUSTOMIZED_ESP_H264_VERSION`"。
- 该组件在 `esp_video` 的清单里声明为 `if: target in [esp32p4]`，本仓库 target 是 `esp32s3` → 被 skip，
  但组件管理器仍要求这个符号有定义 → 每次 configure 都以退出码 10 要求重跑，第二次重跑即 FATAL。
- 本机 `managed_components/espressif__esp_h264` 目录当前不存在（`dependencies.lock` 里仍有它的条目）。
- 处理办法（需联网、会动项目依赖文件，**确认后再执行**）：`idf.py update-dependencies`，
  或临时 `idf.py add-dependency "espressif/esp_h264^1.3.0"` 装回组件，然后 `idf.py build`。

### 5.2 真机清单（待硬件）

- [ ] 上传/确认 `/sdcard/announce/motion.mp3` 存在 → 手靠近超声波 → 喇叭出声，网页日志 `播报开始:`；
- [ ] 手靠近同时挡红外 → `motion` / `beam` 两个提示音都能播（不再互相压制）；
- [ ] 播报中途让服务端断一次（网络抖动）→ 音频**不被打断**；
- [ ] 「打开客厅灯」→ 网页日志出现 `收到节点N消息: kind=ok`；拔掉节点 30 秒再插回，期间说一次「开灯」
      → 节点重锁后灯自动亮（走 `cmdfail` + 补发路径）；
- [ ] 路由器换信道（或重启路由器触发 ACS）→ 网页日志出现 `链路事件(channel)` → 节点 1~2 秒内重锁；
- [ ] 节点串口：`[cmd] seq=N do …` → `TX (1/3) @n1#N ok …`，且主控日志无 `cmdfail`。
