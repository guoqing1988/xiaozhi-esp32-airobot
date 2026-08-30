#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include "esp32_camera.h"
#include "local_music_player.h"
#include "http_upload_server.h"

#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <driver/i2c_master.h>
#include <driver/sdmmc_host.h>
#include <sdmmc_cmd.h>
#include <esp_vfs_fat.h>
#include "driver/uart.h"
#include <cstring>
#include <cstdio>
#include <memory>
#include <atomic>
#include <mutex>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <driver/spi_common.h>

#if defined(LCD_TYPE_ILI9341_SERIAL)
#include "esp_lcd_ili9341.h"
#endif

#if defined(LCD_TYPE_GC9A01_SERIAL)
#include "esp_lcd_gc9a01.h"
static const gc9a01_lcd_init_cmd_t gc9107_lcd_init_cmds[] = {
    //  {cmd, { data }, data_size, delay_ms}
    {0xfe, (uint8_t[]){0x00}, 0, 0},
    {0xef, (uint8_t[]){0x00}, 0, 0},
    {0xb0, (uint8_t[]){0xc0}, 1, 0},
    {0xb1, (uint8_t[]){0x80}, 1, 0},
    {0xb2, (uint8_t[]){0x27}, 1, 0},
    {0xb3, (uint8_t[]){0x13}, 1, 0},
    {0xb6, (uint8_t[]){0x19}, 1, 0},
    {0xb7, (uint8_t[]){0x05}, 1, 0},
    {0xac, (uint8_t[]){0xc8}, 1, 0},
    {0xab, (uint8_t[]){0x0f}, 1, 0},
    {0x3a, (uint8_t[]){0x05}, 1, 0},
    {0xb4, (uint8_t[]){0x04}, 1, 0},
    {0xa8, (uint8_t[]){0x08}, 1, 0},
    {0xb8, (uint8_t[]){0x08}, 1, 0},
    {0xea, (uint8_t[]){0x02}, 1, 0},
    {0xe8, (uint8_t[]){0x2A}, 1, 0},
    {0xe9, (uint8_t[]){0x47}, 1, 0},
    {0xe7, (uint8_t[]){0x5f}, 1, 0},
    {0xc6, (uint8_t[]){0x21}, 1, 0},
    {0xc7, (uint8_t[]){0x15}, 1, 0},
    {0xf0,
    (uint8_t[]){0x1D, 0x38, 0x09, 0x4D, 0x92, 0x2F, 0x35, 0x52, 0x1E, 0x0C,
                0x04, 0x12, 0x14, 0x1f},
    14, 0},
    {0xf1,
    (uint8_t[]){0x16, 0x40, 0x1C, 0x54, 0xA9, 0x2D, 0x2E, 0x56, 0x10, 0x0D,
                0x0C, 0x1A, 0x14, 0x1E},
    14, 0},
    {0xf4, (uint8_t[]){0x00, 0x00, 0xFF}, 3, 0},
    {0xba, (uint8_t[]){0xFF, 0xFF}, 2, 0},
};
#endif
 
#define TAG "CompactWifiBoardS3CamAirobot"

class CompactWifiBoardS3CamAirobot : public WifiBoard {
private:
 
    Button boot_button_;
    LcdDisplay* display_;
    Esp32Camera* camera_;
    std::unique_ptr<LocalMusicPlayer> music_player_;
    bool sd_card_mounted_ = false;
    esp_timer_handle_t ip_timer_ = nullptr;  // 待机状态底部显示 IP 的定时器
    // Arduino 下位机双向状态(RX 解析任务写, MCP 工具读)
    std::atomic<bool> uno_busy_{false};       // Arduino 正在执行动作
    std::mutex uno_status_mutex_;             // 保护 uno_last_*
    std::string uno_last_action_;             // 最近动作(如 go-forward-10)
    std::string uno_last_result_;             // 最近结果(busy/done)
    TaskHandle_t uno_status_task_ = nullptr;  // UART0 RX 解析任务

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
#if defined(LCD_TYPE_ILI9341_SERIAL)
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
#elif defined(LCD_TYPE_GC9A01_SERIAL)
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(panel_io, &panel_config, &panel));
        gc9a01_vendor_config_t gc9107_vendor_config = {
            .init_cmds = gc9107_lcd_init_cmds,
            .init_cmds_size = sizeof(gc9107_lcd_init_cmds) / sizeof(gc9a01_lcd_init_cmd_t),
        };        
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
#endif
        
        esp_lcd_panel_reset(panel);

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
#ifdef  LCD_TYPE_GC9A01_SERIAL
        panel_config.vendor_config = &gc9107_vendor_config;
#endif
        display_ = new SpiLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeCamera() {
        camera_config_t config = {};
        config.pin_d0 = CAMERA_PIN_D0;
        config.pin_d1 = CAMERA_PIN_D1;
        config.pin_d2 = CAMERA_PIN_D2;
        config.pin_d3 = CAMERA_PIN_D3;
        config.pin_d4 = CAMERA_PIN_D4;
        config.pin_d5 = CAMERA_PIN_D5;
        config.pin_d6 = CAMERA_PIN_D6;
        config.pin_d7 = CAMERA_PIN_D7;
        config.pin_xclk = CAMERA_PIN_XCLK;
        config.pin_pclk = CAMERA_PIN_PCLK;
        config.pin_vsync = CAMERA_PIN_VSYNC;
        config.pin_href = CAMERA_PIN_HREF;
        config.pin_sccb_sda = CAMERA_PIN_SIOD;
        config.pin_sccb_scl = CAMERA_PIN_SIOC;
        config.sccb_i2c_port = 0;
        config.pin_pwdn = CAMERA_PIN_PWDN;
        config.pin_reset = CAMERA_PIN_RESET;
        config.xclk_freq_hz = XCLK_FREQ_HZ;
        config.pixel_format = PIXFORMAT_RGB565;
        config.frame_size = FRAMESIZE_VGA;
        config.jpeg_quality = 12;
        config.fb_count = 1;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
        camera_ = new Esp32Camera(config);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            // 播放音乐时按按钮：先停歌并本地回到待命（不等服务器 tts:stop 响应，
            // 避免状态卡在“说话中”）；再按一次按钮即进入聆听对话
            if (music_player_ != nullptr && music_player_->IsPlaying()) {
                music_player_->Stop();
                if (app.GetDeviceState() == kDeviceStateSpeaking) {
                    app.SetDeviceState(kDeviceStateIdle);
                }
                return;
            }
            app.ToggleChatState();
        });
    }

    void InitializeSDCard() {
        ESP_LOGI(TAG, "Initializing SD card");
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.slot = SDMMC_HOST_SLOT_0;
        host.max_freq_khz = SDMMC_FREQ_DEFAULT;
        host.flags = SDMMC_HOST_FLAG_1BIT;

        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.cd = SDMMC_SLOT_NO_CD;
        slot.wp = SDMMC_SLOT_NO_WP;
        slot.width = 1;
        slot.cmd = SD_MMC_CMD_GPIO;
        slot.clk = SD_MMC_CLK_GPIO;
        slot.d0  = SD_MMC_D0_GPIO;

        const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = false,
            .max_files = 5,
            .allocation_unit_size = 64 * 1024,
        };

        sdmmc_card_t* card = nullptr;
        esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mount_config, &card);
        if (ret == ESP_OK) {
            sd_card_mounted_ = true;
            ESP_LOGI(TAG, "SD card mounted at %s", SD_MOUNT_POINT);
        } else {
            sd_card_mounted_ = false;
            ESP_LOGW(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
        }
    }

    void InitializeUploadServer() {
        if (!sd_card_mounted_) {
            ESP_LOGW(TAG, "SD card not mounted, skip upload server");
            return;
        }
        // 上传成功回调：刷新歌曲列表缓存，AI 立刻能查到新歌(无需重启)
        StartUploadServer([this]() { GetMusicPlayer()->ScanSongs(); });
    }

    // 唤醒词检测到：立即打断本地音乐播放（配合 Application 的板级回调钩子）
    void OnWakeWordDetected(const std::string& wake_word) override {
        if (music_player_ != nullptr && music_player_->IsPlaying()) {
            ESP_LOGI(TAG, "Wake word '%s' detected, stop local music", wake_word.c_str());
            music_player_->Stop();
        }
    }

    // 取 WiFi STA 的 IPv4 地址（无则返回空串）
    static std::string GetLocalIp() {
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif == nullptr) {
            return "";
        }
        esp_netif_ip_info_t ip = {};
        if (esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0) {
            return "";
        }
        char buf[16];
        snprintf(buf, sizeof(buf), "%d.%d.%d.%d",
                 (int)((ip.ip.addr >> 0) & 0xff), (int)((ip.ip.addr >> 8) & 0xff),
                 (int)((ip.ip.addr >> 16) & 0xff), (int)((ip.ip.addr >> 24) & 0xff));
        return buf;
    }

    // 待机(Idle)且未播放时，在底部字幕条显示本机 IP（供访问上传页）；
    // 播放/对话时字幕条被歌词和聊天消息占用，不干预。
    void UpdateIpDisplay() {
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() != kDeviceStateIdle) {
            return;
        }
        if (music_player_ != nullptr && music_player_->IsPlaying()) {
            return;
        }
        std::string ip = GetLocalIp();
        if (ip.empty()) {
            return;
        }
        auto* display = GetDisplay();
        if (display != nullptr) {
            display->SetChatMessage("system", ip.c_str());
        }
    }

    static void OnIpDisplayTimer(void* arg) {
        static_cast<CompactWifiBoardS3CamAirobot*>(arg)->UpdateIpDisplay();
    }

    void InitializeIpDisplay() {
        esp_timer_create_args_t args = {};
        args.callback = OnIpDisplayTimer;
        args.arg = this;
        args.name = "ip_display";
        if (esp_timer_create(&args, &ip_timer_) == ESP_OK) {
            esp_timer_start_periodic(ip_timer_, 1000000);  // 每秒刷新
        }
    }

    LocalMusicPlayer* GetMusicPlayer() {
        if (music_player_ == nullptr) {
            music_player_ = std::make_unique<LocalMusicPlayer>(Application::GetInstance().GetAudioService());
            music_player_->ScanSongs();
        }
        return music_player_.get();
    }

    void InitializeMusicTools() {
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.music.play_random",
            "Play a random song from the TF card's music folder",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                return GetMusicPlayer()->PlayRandom();
            });
        mcp.AddTool("self.music.play",
            "Play a specific song from the TF card by name",
            PropertyList({ Property("name", kPropertyTypeString) }),
            [this](const PropertyList& props) -> ReturnValue {
                return GetMusicPlayer()->PlaySong(props["name"].value<std::string>());
            });
        mcp.AddTool("self.music.list",
            "List song names available on the TF card",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                auto songs = GetMusicPlayer()->ListSongs();
                std::string result;
                for (const auto& s : songs) {
                    result += s + "\n";
                }
                return result;
            });
        mcp.AddTool("self.music.pause",
            "Pause the current TF card song",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                GetMusicPlayer()->Pause();
                return true;
            });
        mcp.AddTool("self.music.resume",
            "Resume the paused TF card song",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                GetMusicPlayer()->Resume();
                return true;
            });
        mcp.AddTool("self.music.stop",
            "Stop playing the TF card song",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                GetMusicPlayer()->Stop();
                return true;
            });
    }

    void InitializeEchoUart() {
        uart_config_t uart_config = {
            .baud_rate = ECHO_UART_BAUD_RATE,
            .data_bits = UART_DATA_8_BITS,
            .parity    = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        int intr_alloc_flags = 0;
        ESP_ERROR_CHECK(uart_driver_install(ECHO_UART_PORT_NUM, BUF_SIZE * 2, 0, 0, NULL, intr_alloc_flags));
        ESP_ERROR_CHECK(uart_param_config(ECHO_UART_PORT_NUM, &uart_config));
        ESP_ERROR_CHECK(uart_set_pin(ECHO_UART_PORT_NUM, UART_ECHO_TXD, UART_ECHO_RXD, UART_ECHO_RTS, UART_ECHO_CTS));
        SendUartMessage("w2");
        // 启动 UART0 RX 解析任务, 读取 Arduino 回执(@busy/@done), 供 self.uno.get_status 查询
        xTaskCreate(UnoStatusTask, "uno_status", 4096, this, 3, &uno_status_task_);
    }

    // 静态任务包装: 解析 Arduino 下位机回执
    static void UnoStatusTask(void* arg) {
        auto* self = static_cast<CompactWifiBoardS3CamAirobot*>(arg);
        self->UnoStatusLoop();
    }

    void UnoStatusLoop() {
        char line[64];
        while (true) {
            int len = uart_read_bytes(ECHO_UART_PORT_NUM, line, sizeof(line) - 1, pdMS_TO_TICKS(200));
            if (len <= 0) {
                continue;
            }
            line[len] = '\0';
            // 按行解析 @busy / @done(可能一次读到多行)
            char* tok = strtok(line, "\r\n");
            while (tok != nullptr) {
                if (strncmp(tok, "@busy", 5) == 0) {
                    uno_busy_ = true;
                    std::lock_guard<std::mutex> lock(uno_status_mutex_);
                    uno_last_result_ = "busy";
                    if (tok[5] == ' ') {
                        uno_last_action_ = tok + 6;
                    }
                } else if (strncmp(tok, "@done", 5) == 0) {
                    uno_busy_ = false;
                    std::lock_guard<std::mutex> lock(uno_status_mutex_);
                    uno_last_result_ = "done";
                    if (tok[5] == ' ') {
                        uno_last_action_ = tok + 6;
                    }
                }
                tok = strtok(nullptr, "\r\n");
            }
        }
    }

    // 发送 UART 指令并返回描述性结果(成功/失败), 避免 AI 看到 true/false 无法确认执行结果而重复调用
    static std::string SendUartMessage(const char* command_str) {
        // 指令防抖：AI 无执行确认机制时可能反复调用相同工具(实测会重复调用几十次,
        // 间隔约 700-900ms)。相同指令 1 秒内只发送一次，避免 Arduino 串口堆积重复指令。
        // 不同指令(动作切换/组合编排)不受影响，照常发送。
        static char s_last_cmd[32] = {};
        static int64_t s_last_us = 0;
        int64_t now = esp_timer_get_time();
        if (strcmp(s_last_cmd, command_str) == 0 && (now - s_last_us) < 1000000) {
            return std::string("指令已发送(防抖): ") + command_str;  // 防抖丢弃, 视为成功
        }
        snprintf(s_last_cmd, sizeof(s_last_cmd), "%s", command_str);
        s_last_us = now;
        // 统一加 '@' 前缀, 让 Arduino 只认带前缀的命令行(过滤日志乱码)
        int written = uart_write_bytes(ECHO_UART_PORT_NUM, "@", 1);
        if (written < 0) return std::string("指令发送失败: ") + command_str;
        written = uart_write_bytes(ECHO_UART_PORT_NUM, command_str, strlen(command_str));
        if (written < 0) return std::string("指令发送失败: ") + command_str;
        written = uart_write_bytes(ECHO_UART_PORT_NUM, "\n", 1);
        if (written < 0) return std::string("指令发送失败: ") + command_str;
        return std::string("指令已发送: ") + command_str;
    }

    void InitializeUnoTools() {
        auto& mcp_server = McpServer::GetInstance();

        mcp_server.AddTool(
            "self.uno.action",
            "麦克纳姆轮机器人控制。调用本工具一次即完成整个动作并自动停止，不要重复调用。action: 0=停止,1=前进,2=后退,3=左转,4=右转,5=左移,6=右移,7=左上斜移,8=右上斜移,9=左下斜移,10=右下斜移; steps: 动作执行步数(1-100, 越大动作时间越长)",
            PropertyList({Property("action", kPropertyTypeInteger, 0),
                          Property("steps", kPropertyTypeInteger, 10, 5, 100)}),
            [this](const PropertyList& properties) -> ReturnValue {
                int action_type = properties["action"].value<int>();
                int steps = properties["steps"].value<int>();
                const char* action_str = nullptr;
                switch (action_type) {
                    case 0: action_str = "stop"; break;
                    case 1: action_str = "forward"; break;
                    case 2: action_str = "back"; break;
                    case 3: action_str = "left"; break;
                    case 4: action_str = "right"; break;
                    case 5: action_str = "leftmove"; break;
                    case 6: action_str = "rightmove"; break;
                    case 7: action_str = "leftup"; break;
                    case 8: action_str = "rightup"; break;
                    case 9: action_str = "leftdown"; break;
                    case 10: action_str = "rightdown"; break;
                    default: action_str = "stop"; break;
                }
                char cmd[32];
                snprintf(cmd, sizeof(cmd), "go-%s-%d", action_str, steps);
                return SendUartMessage(cmd);
            });

        mcp_server.AddTool(
            "self.uno.servo",
            "头部舵机控制，调用一次即转到指定角度。180度舵机，回正角度为82，大于82向左转，小于82向右转。degree: 0-180",
            PropertyList({Property("degree", kPropertyTypeInteger, 82, 0, 180)}),
            [this](const PropertyList& properties) -> ReturnValue {
                int degree = properties["degree"].value<int>();
                char cmd[16];
                snprintf(cmd, sizeof(cmd), "servo-%d", degree);
                return SendUartMessage(cmd);
            });

        mcp_server.AddTool(
            "self.uno.teji",
            "执行特技，调用一次即执行完毕，不要重复调用。action: 1摇头 2闪电走位 3转圈 4蛇形走位 5调头 6开灯 7关灯",
            PropertyList({Property("action", kPropertyTypeInteger, 1)}),
            [this](const PropertyList& properties) -> ReturnValue {
                int action = properties["action"].value<int>();
                const char* action_str = nullptr;
                switch (action) {
                    case 1: action_str = "yaotou"; break;
                    case 2: action_str = "shandian"; break;
                    case 3: action_str = "zhuanquan"; break;
                    case 4: action_str = "sxzw"; break;
                    case 5: action_str = "diaotou"; break;
                    case 6: action_str = "ledon"; break;
                    case 7: action_str = "ledoff"; break;
                    default: action_str = "yaotou"; break;
                }
                char cmd[32];
                snprintf(cmd, sizeof(cmd), "tj-%s", action_str);
                return SendUartMessage(cmd);
            });

        mcp_server.AddTool(
            "self.uno.speed",
            "机器人速度控制。speed: 100-255",
            PropertyList({Property("speed", kPropertyTypeInteger, 200, 100, 255)}),
            [this](const PropertyList& properties) -> ReturnValue {
                int speed = properties["speed"].value<int>();
                char cmd[16];
                snprintf(cmd, sizeof(cmd), "speed-%d", speed);
                return SendUartMessage(cmd);
            });

        mcp_server.AddTool(
            "self.uno.get_status",
            "获取 Arduino 下位机(麦克纳姆轮机器人)的实时状态。返回 moving(正在执行动作)或 idle(空闲)；"
            "若刚执行完动作会附上最近动作与结果(如 idle (last: go-forward-10 done))。",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                if (uno_busy_) {
                    return std::string("moving");
                }
                std::lock_guard<std::mutex> lock(uno_status_mutex_);
                if (!uno_last_result_.empty()) {
                    return std::string("idle (last: ") + uno_last_action_ + " " + uno_last_result_ + ")";
                }
                return std::string("idle");
            });

        mcp_server.AddTool(
            "self.uno.line_follow",
            "巡线模式控制：让机器人沿地面黑线自动行驶。action: 1=开始巡线, 0=停止巡线。"
            "调用一次即开始/停止，不要重复调用；巡线结束(丢线超时/超时上限)会自动停止。",
            PropertyList({Property("action", kPropertyTypeInteger, 1)}),
            [this](const PropertyList& properties) -> ReturnValue {
                int action = properties["action"].value<int>();
                return SendUartMessage(action == 1 ? "line-start" : "line-stop");
            });
    }

    // 调试工具: 临时切换系统日志级别(避免 GPIO43 日志污染 Arduino)
    void InitializeDebugTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool(
            "self.debug.set_log_level",
            "临时切换系统日志级别(调试用). level: 0=无日志,1=错误,2=警告,3=信息,4=调试",
            PropertyList({Property("level", kPropertyTypeInteger, 3, 0, 4)}),
            [](const PropertyList& properties) -> ReturnValue {
                int lv = properties["level"].value<int>();
                esp_log_level_set("*", (esp_log_level_t)lv);
                return true;
            });
    }

public:
    CompactWifiBoardS3CamAirobot() :
        boot_button_(BOOT_BUTTON_GPIO) {
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeButtons();
        InitializeCamera();
        InitializeSDCard();
        InitializeUploadServer();
        InitializeIpDisplay();
        InitializeMusicTools();
        InitializeEchoUart();
        InitializeUnoTools();
        InitializeDebugTools();
        // [TEMP 诊断] 打开 INFO 日志定位播放卡死(定位后恢复 ERROR, 避免 GPIO43 污染 Arduino 串口)
        esp_log_level_set("*", ESP_LOG_INFO);
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            GetBacklight()->RestoreBrightness();
        }
        
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
            return &backlight;
        }
        return nullptr;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }
};

DECLARE_BOARD(CompactWifiBoardS3CamAirobot);
