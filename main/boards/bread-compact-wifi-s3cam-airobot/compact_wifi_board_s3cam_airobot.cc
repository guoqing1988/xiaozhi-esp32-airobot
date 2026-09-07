#include "wifi_board.h"
#include <wifi_manager.h>
#include "codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "airobot_lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include "esp32_camera.h"
#include "driver/gpio.h"
#include "settings.h"
#ifdef CONFIG_XIAOZHI_AIROBOT_ENABLE_TF_CARD
#include "local_music_player.h"
#include "http_upload_server.h"
#include "alarm_manager.h"
#include "assets/lang_config.h"
#endif

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
#include <time.h>
#include <cstdlib>
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
#ifdef CONFIG_XIAOZHI_AIROBOT_ENABLE_TF_CARD
    std::unique_ptr<LocalMusicPlayer> music_player_;
    std::unique_ptr<AlarmManager> alarm_manager_;
    esp_timer_handle_t alarm_chime_timer_ = nullptr;  // 无歌兜底: 几声提示音连播的定时器
    std::atomic<int> alarm_chime_left_ = 0;           // 剩余要补播的提示音次数(跨任务, 用原子)
    bool sd_card_mounted_ = false;
#endif
    esp_timer_handle_t ip_timer_ = nullptr;  // 待机状态底部显示 IP 的定时器
    // Arduino 下位机双向状态(RX 解析任务写, MCP 工具读)
    std::atomic<bool> uno_busy_{false};       // Arduino 正在执行动作
    std::atomic<int64_t> uno_busy_since_us_{0};  // 进入 busy 时刻(看门狗用)
    std::mutex uno_status_mutex_;             // 保护 uno_last_*
    std::string uno_last_action_;             // 最近动作(如 go-forward-10)
    std::string uno_last_result_;             // 最近结果(busy/done)
    std::atomic<int> uno_speed_{-1};          // 最新速度(@stat 上报, -1=未上报)
    std::atomic<int> uno_servo_{-1};          // 最新舵机1角度(@stat 上报, -1=未上报)
    TaskHandle_t uno_status_task_ = nullptr;  // UART0 RX 解析任务

    // ---- 待机全屏大时钟（AI 可控: self.clock.set(开关+主题合一) / self.clock.current, NVS 持久化）----
    bool clock_mode_ = false;                // 时钟显示开关
    int clock_theme_ = 0;                    // 时钟主题索引: 0=黑底白字, 1=白底黑字(仅两套经典高对比)
    esp_timer_handle_t clock_timer_ = nullptr;

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
        display_ = new AirobotLcdDisplay(panel_io, panel,
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

    // 应用 NVS 保存的摄像头翻转设置(开机调用, 断电重启仍保持)
    void ApplyCameraFlip() {
        Settings settings("camera", false);
        int32_t mode = settings.GetInt("flip", 0);
        if (camera_ != nullptr) {
            camera_->SetHMirror(mode & 1);
            camera_->SetVFlip((mode & 2) != 0);
        }
    }

    // 摄像头翻转控制工具(AI 可调, 设置本地持久化)
    void InitializeCameraTools() {
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool(
            "self.camera.set_flip",
            "设置摄像头画面翻转方向，设置后本地保存(断电重启仍生效)。mode: 0=正常, 1=左右镜像, 2=上下翻转, 3=旋转180(镜像+翻转)",
            PropertyList({Property("mode", kPropertyTypeInteger, 0, 0, 3)}),
            [this](const PropertyList& props) -> ReturnValue {
                int mode = props["mode"].value<int>();
                if (camera_ == nullptr) {
                    return std::string("摄像头未初始化");
                }
                if (!camera_->SetHMirror(mode & 1) || !camera_->SetVFlip((mode & 2) != 0)) {
                    return std::string("设置失败");
                }
                Settings settings("camera", true);
                settings.SetInt("flip", mode);
                return std::string("摄像头画面已设置为模式 ") + std::to_string(mode);
            });
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
#ifdef CONFIG_XIAOZHI_AIROBOT_ENABLE_TF_CARD
            // 播放音乐时按按钮：先停歌并本地回到待命（不等服务器 tts:stop 响应，
            // 避免状态卡在“说话中”）；再按一次按钮即进入聆听对话
            if (music_player_ != nullptr && music_player_->IsPlaying()) {
                music_player_->Stop();
                if (app.GetDeviceState() == kDeviceStateSpeaking) {
                    app.SetDeviceState(kDeviceStateIdle);
                }
                return;
            }
#endif
            app.ToggleChatState();
        });
    }

#ifdef CONFIG_XIAOZHI_AIROBOT_ENABLE_TF_CARD
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
#endif  // CONFIG_XIAOZHI_AIROBOT_ENABLE_TF_CARD

#ifdef CONFIG_XIAOZHI_AIROBOT_ENABLE_TF_CARD
    // 唤醒词检测到：立即打断本地音乐播放（配合 Application 的板级回调钩子）
    void OnWakeWordDetected(const std::string& wake_word) override {
        if (music_player_ != nullptr && music_player_->IsPlaying()) {
            ESP_LOGI(TAG, "Wake word '%s' detected, stop local music", wake_word.c_str());
            music_player_->Stop();
        }
    }
#endif

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
#ifdef CONFIG_XIAOZHI_AIROBOT_ENABLE_TF_CARD
        if (music_player_ != nullptr && music_player_->IsPlaying()) {
            return;
        }
#endif
        // 大时钟显示中: 底部字幕条已被隐藏, 不再发 IP(避免 SetChatMessage 把字幕条重新显示出来)
        if (clock_mode_ && time(nullptr) > 1700000000) {
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

    // 时钟主题名称(与 airobot_lcd_display.h 的 kClockThemes 顺序一致; 已精简为 2 套)
    static const char* ClockThemeName(int id) {
        switch (id) {
            case 1: return "白底黑字";
            case 0:
            default: return "黑底白字";
        }
    }

    // 应用 NVS 保存的时钟显示设置(开机调用, 断电重启仍保持)
    void ApplyClockMode() {
        Settings settings("clock", false);
        clock_mode_ = settings.GetInt("mode", 0) == 1;
        clock_theme_ = settings.GetInt("theme", 0);  // 0..1, 旧版只存 0/1, 兼容
        if (GetDisplay() != nullptr) {
            static_cast<AirobotLcdDisplay*>(GetDisplay())->SetClockTheme(clock_theme_);
        }
    }

    // 待机大时钟工具(AI 可调, 设置本地持久化)
    void InitializeClockTools() {
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool(
            "self.clock.set",
            "设置待机全屏大时钟。mode: 1=开启(待机时整个屏幕显示大号时间+日期), 0=关闭, -1=切换开关(用户说\"打开/关闭/切换时钟模式\"时用); theme: 0=黑底白字, 1=白底黑字, -1=切换下一个主题(用户说\"切换时钟颜色/主题\"时用)。mode 与 theme 可只传其一, 未传的参数保持当前值不变; 两者都传则同时生效。设置本地保存(NVS), 断电重启仍生效",
            PropertyList({Property("mode", kPropertyTypeInteger, -2),
                          Property("theme", kPropertyTypeInteger, -2)}),
            [this](const PropertyList& props) -> ReturnValue {
                // 哨兵 -2 = 未指定, 保持当前值; 仅处理 AI 显式传入的参数
                int mode = props["mode"].value<int>();
                int theme = props["theme"].value<int>();
                Settings settings("clock", true);
                std::string result;

                if (mode != -2) {
                    if (mode == -1) mode = clock_mode_ ? 0 : 1;      // 切换开关
                    if (mode != 0 && mode != 1) mode = clock_mode_ ? 1 : 0;  // 非法→保持
                    clock_mode_ = (mode == 1);
                    settings.SetInt("mode", mode);
                    result = clock_mode_ ? "时钟显示已开启" : "时钟显示已关闭";
                }

                if (theme != -2) {
                    const int kCount = 2;  // 主题仅保留黑底白字/白底黑字(见 kClockThemes)
                    if (theme == -1) theme = (clock_theme_ + 1) % kCount;  // 切下一个
                    if (theme < -1 || theme >= kCount) theme = clock_theme_;  // 非法→保持
                    clock_theme_ = theme;
                    settings.SetInt("theme", theme);
                    if (GetDisplay() != nullptr) {
                        static_cast<AirobotLcdDisplay*>(GetDisplay())->SetClockTheme(theme);
                    }
                    if (!result.empty()) result += "; ";
                    result += "主题:" + std::string(ClockThemeName(theme));
                }

                if (result.empty()) {
                    return "时钟设置未改变(未指定 mode/theme)。当前: " +
                           std::string(clock_mode_ ? "已开启" : "已关闭") +
                           ", 主题 " + ClockThemeName(clock_theme_);
                }
                return result;
            });
        mcp.AddTool(
            "self.clock.current",
            "查询当前待机大时钟的状态。返回: 时钟是否开启(open/off) 以及当前主题名称(黑底白字/白底黑字)。用户问\"现在是什么时钟主题\"或\"时钟开没开\"时调用此工具",
            PropertyList(),
            [this](const PropertyList& props) -> ReturnValue {
                std::string r = clock_mode_ ? "时钟已开启，当前主题：" : "时钟未开启，当前主题：";
                r += ClockThemeName(clock_theme_);
                return r;
            });
    }

    // 每秒刷新：仅"时钟模式开启 + 待机 + 系统时间已同步(联网后NTP)"时显示大号时间
    void UpdateClock() {
        auto* display = GetDisplay();
        if (display == nullptr) {
            return;
        }
        auto* lcd = static_cast<AirobotLcdDisplay*>(display);
        time_t now = time(nullptr);
        bool show = clock_mode_ && Application::GetInstance().GetDeviceState() == kDeviceStateIdle
                    && now > 1700000000;  // 未同步到 2023 年之后视为无效时间
        if (!show) {
            lcd->UpdateClock(false, nullptr, nullptr);
            return;
        }
        // 仅 esp_timer 单任务调用, 静态缓冲安全
        static char buf[6];    // HH:MM
        static char dbuf[16];  // YYYY-MM-DD
        struct tm t = *localtime(&now);
        strftime(buf, sizeof(buf), "%H:%M", &t);
        strftime(dbuf, sizeof(dbuf), "%Y-%m-%d", &t);
        lcd->UpdateClock(true, buf, dbuf);
    }

    static void OnClockTimer(void* arg) {
        static_cast<CompactWifiBoardS3CamAirobot*>(arg)->UpdateClock();
    }

    void InitializeClock() {
        ApplyClockMode();
        esp_timer_create_args_t args = {};
        args.callback = OnClockTimer;
        args.arg = this;
        args.name = "clock_display";
        if (esp_timer_create(&args, &clock_timer_) == ESP_OK) {
            esp_timer_start_periodic(clock_timer_, 1000000);  // 每秒刷新
        }
    }

#ifdef CONFIG_XIAOZHI_AIROBOT_ENABLE_TF_CARD
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
            "List songs available on the TF card (returns up to 30 names plus total count, "
            "enough for the AI to answer questions like \"how many songs\"). "
            "To find a specific song by name/artist, use self.music.search instead.",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                auto songs = GetMusicPlayer()->ListSongs();
                std::string result;
                const size_t kMaxShown = 30;  // 截断: 避免几百首歌名撑爆 AI 上下文
                for (size_t i = 0; i < songs.size() && i < kMaxShown; ++i) {
                    result += songs[i] + "\n";
                }
                if (songs.size() > kMaxShown) {
                    result += "...(共 " + std::to_string(songs.size()) + " 首, 仅显示前 " +
                              std::to_string(kMaxShown) + " 首)";
                } else {
                    result += "共 " + std::to_string(songs.size()) + " 首";
                }
                return result;
            });
        mcp.AddTool("self.music.search",
            "Search songs on the TF card by keyword (substring match, case-insensitive). "
            "Use when the user asks whether a specific song/artist exists, e.g. keyword=\"薛之谦\". "
            "Returns up to 30 matching names plus the match count.",
            PropertyList({Property("keyword", kPropertyTypeString)}),
            [this](const PropertyList& props) -> ReturnValue {
                std::string kw = props["keyword"].value<std::string>();
                std::string lower_kw = kw;
                std::transform(lower_kw.begin(), lower_kw.end(), lower_kw.begin(), ::tolower);
                auto songs = GetMusicPlayer()->ListSongs();
                std::string result;
                size_t matched = 0;
                const size_t kMaxShown = 30;
                for (const auto& s : songs) {
                    bool hit = kw.empty();
                    if (!hit) {
                        hit = s.find(kw) != std::string::npos;
                        if (!hit) {
                            std::string sl = s;
                            std::transform(sl.begin(), sl.end(), sl.begin(), ::tolower);
                            hit = sl.find(lower_kw) != std::string::npos;
                        }
                    }
                    if (hit && matched < kMaxShown) {
                        result += s + "\n";
                    }
                    if (hit) {
                        matched++;
                    }
                }
                if (matched == 0) {
                    return std::string("未找到包含 \"") + kw + "\" 的歌曲";
                }
                if (matched > kMaxShown) {
                    result += "...(共 " + std::to_string(matched) + " 首匹配, 仅显示前 " +
                              std::to_string(kMaxShown) + " 首)";
                } else {
                    result += "共 " + std::to_string(matched) + " 首匹配";
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
#endif  // CONFIG_XIAOZHI_AIROBOT_ENABLE_TF_CARD

#ifdef CONFIG_XIAOZHI_AIROBOT_ENABLE_TF_CARD
    // AI 闹钟/定时提醒: 注册 self.alarm.* MCP 工具 + web 页 REST 回调 + 后台到点检查
    void InitializeAlarmTools() {
        alarm_manager_ = std::make_unique<AlarmManager>(Application::GetInstance().GetAudioService());
        // 到点触发: 检查线程回调, 用 Schedule 切回主任务再播报(避免跨任务操作音频/显示)
        alarm_manager_->SetTriggerCallback([this](const AlarmItem& a) {
            Application::GetInstance().Schedule([this, a]() { AlarmSpeak(a); });
        });
        alarm_manager_->Start();

        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.alarm.set",
            "设置一个闹钟/定时提醒。用于'X 分钟后提醒我'或'每天 HH:MM 提醒'。\n"
            "Args:\n"
            "  `type`: 'relative' 表示从现在起 N 分钟后提醒(一次性); 'absolute' 表示每天 HH:MM 提醒。\n"
            "  `value`: relative 用分钟数(如 '5'); absolute 用 'HH:MM'(如 '07:30')。\n"
            "  `label`: 提醒内容(如 '喝水'), 可省略。\n"
            "  `song`: 指定铃声歌曲名(如 '晴天.mp3'), 可省略; 省略时到点随机播放。\n"
            "Return: 创建的闹钟编号及说明。",
            PropertyList({
                Property("type", kPropertyTypeString),
                Property("value", kPropertyTypeString),
                Property("label", kPropertyTypeString, std::string("")),
                Property("song", kPropertyTypeString, std::string(""))
            }),
            [this](const PropertyList& props) -> ReturnValue {
                auto type = props["type"].value<std::string>();
                auto value = props["value"].value<std::string>();
                auto label = props["label"].value<std::string>();
                auto song = props["song"].value<std::string>();
                AlarmType t = (type == "absolute") ? kAlarmTypeAbsolute : kAlarmTypeRelative;
                int sec = 0;
                if (t == kAlarmTypeRelative) {
                    sec = atoi(value.c_str()) * 60;   // 分钟 -> 秒
                } else {
                    int hh = 0, mm = 0;
                    sscanf(value.c_str(), "%d:%d", &hh, &mm);
                    sec = hh * 3600 + mm * 60;        // HH:MM -> 当天秒数
                }
                // 用户语音说的歌名往往不准(如"晴天"vs"晴天.mp3"), 此处解析成本地准确文件名
                // 再持久化, 保证到点响铃能准确播中; 指定了歌但本地无匹配 -> 回退随机。
                std::string resolved;
                if (!song.empty() && GetMusicPlayer() != nullptr) {
                    resolved = GetMusicPlayer()->ResolveSong(song);
                }
                int id = alarm_manager_->Add(t, sec, label, resolved);
                std::string ring;
                if (resolved.empty()) {
                    ring = song.empty() ? "随机选歌" : ("随机选歌(未找到 \"" + song + "\")");
                } else {
                    ring = "铃声: " + resolved;
                }
                if (t == kAlarmTypeRelative) {
                    return std::string("已设置闹钟 #") + std::to_string(id) + ", 将于 " + value +
                           " 分钟后提醒 (" + ring + ")";
                }
                return std::string("已设置闹钟 #") + std::to_string(id) + ", 每天 " + value +
                       " 提醒 (" + ring + ")";
            });
        mcp.AddTool("self.alarm.list",
            "列出所有闹钟。返回 JSON 数组, 每条含 id/type/trigger_sec/label/enabled。",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                return alarm_manager_->ListJson();
            });
        mcp.AddTool("self.alarm.remove",
            "按编号删除一个闹钟。id: 闹钟编号(见 self.alarm.list)。",
            PropertyList({ Property("id", kPropertyTypeInteger) }),
            [this](const PropertyList& props) -> ReturnValue {
                int id = props["id"].value<int>();
                bool ok = alarm_manager_->Remove(id);
                return ok ? std::string("已删除闹钟 #") + std::to_string(id)
                          : std::string("未找到编号为 ") + std::to_string(id) + " 的闹钟";
            });

        // 让 web 页面(上传页)能读写闹钟
        SetAlarmWebApi({
            .get_alarms_json = [this]() { return alarm_manager_->ListJson(); },
            .add_alarm       = [this](const std::string& type, int value_sec, const std::string& label,
                                       const std::string& song) {
                AlarmType t = (type == "absolute") ? kAlarmTypeAbsolute : kAlarmTypeRelative;
                return alarm_manager_->Add(t, value_sec, label, song);
            },
            .remove_alarm    = [this](int id) { return alarm_manager_->Remove(id); },
        });
    }

    // 到点提醒: 打断本地音乐 -> 播放铃声(音乐闹钟)。
    // 指定了 a.song 则优先播该歌；未指定或指定歌曲未找到则回退随机播放；仍失败或无歌退回内置提示音。
    // 复用现有播放链路(播放/唤醒打断/歌词显示均走原始逻辑), 声音明显且持续,
    // 用户唤醒词/按钮/说停即可打断响铃。
    void AlarmSpeak(const AlarmItem& a) {
        if (music_player_ != nullptr && music_player_->IsPlaying()) {
            music_player_->Stop();  // 闹钟优先: 先停掉用户正在听的歌
        }
        bool ringing = false;
        auto* player = GetMusicPlayer();  // 懒创建(首次响铃时建对象 + 扫描 SD 卡)
        if (player != nullptr && !player->ListSongs().empty()) {
            if (!a.song.empty()) {
                // 指定铃声: PlaySong 成功返回以"已开始播放"/"正在播放"开头；未找到则回退随机
                std::string r = player->PlaySong(a.song);
                ringing = (r.find("已开始播放") != std::string::npos ||
                           r.find("正在播放") != std::string::npos);
            }
            if (!ringing) {
                ringing = player->PlayRandom();
            }
        }
        if (!ringing) {
            StartAlarmChime(3);  // 无歌/启动失败兜底: 立即一声 + 每 1s 补 3 声(共 4 声, 比单声容易听到)
        }
        if (GetDisplay() != nullptr) {
            GetDisplay()->ShowNotification(a.label.empty() ? "⏰ 闹钟提醒" : a.label.c_str());
        }
    }

    // ---- 兜底长提示音: 无歌/播放失败时闹钟不能“一声就完”, 用周期定时器补几声 ----
    // esp_timer 回调(esp_timer 任务上下文)里再 Schedule 回主任务播报,
    // 与 PlaySound 常规调用保持同上下文, 不新增线程、不阻塞任何任务。
    static void OnAlarmChimeTimer(void* arg) {
        auto* self = static_cast<CompactWifiBoardS3CamAirobot*>(arg);
        if (self->alarm_chime_left_.load() <= 0) {
            self->StopAlarmChime();
            return;
        }
        self->alarm_chime_left_.fetch_sub(1);
        Application::GetInstance().Schedule([]() {
            Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP);
        });
        if (self->alarm_chime_left_.load() <= 0) {
            self->StopAlarmChime();
        }
    }

    void StartAlarmChime(int extra_times) {
        StopAlarmChime();
        alarm_chime_left_ = extra_times;
        esp_timer_create_args_t args = {};
        args.callback = OnAlarmChimeTimer;
        args.arg = this;
        args.name = "alarm_chime";
        if (esp_timer_create(&args, &alarm_chime_timer_) == ESP_OK) {
            esp_timer_start_periodic(alarm_chime_timer_, 1000000);  // 每秒补一声
        }
    }

    void StopAlarmChime() {
        if (alarm_chime_timer_ != nullptr) {
            esp_timer_stop(alarm_chime_timer_);
            esp_timer_delete(alarm_chime_timer_);
            alarm_chime_timer_ = nullptr;
        }
        alarm_chime_left_ = 0;
    }
#endif  // CONFIG_XIAOZHI_AIROBOT_ENABLE_TF_CARD

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
        // TX buffer 传 BUF_SIZE(1024) 而非 0: 默认 0 只用 128B 硬件 FIFO,
        // web 摇杆 8Hz 心跳 + AI 指令同时涌来时 uart_write_bytes 会因 FIFO 满而阻塞
        // httpd 单任务, 导致 /uno GET/POST 串行排队变慢。分配软件环形缓冲可降低阻塞。
        ESP_ERROR_CHECK(uart_driver_install(ECHO_UART_PORT_NUM, BUF_SIZE * 2, BUF_SIZE, 0, NULL, intr_alloc_flags));
        ESP_ERROR_CHECK(uart_param_config(ECHO_UART_PORT_NUM, &uart_config));
        ESP_ERROR_CHECK(uart_set_pin(ECHO_UART_PORT_NUM, UART_ECHO_TXD, UART_ECHO_RXD, UART_ECHO_RTS, UART_ECHO_CTS));
        // 未接 Arduino 时 GPIO44(RX) 悬空, 噪声会触发 RX 中断风暴(偶发中断看门狗复位);
        // 内部上拉稳定电平; 接 Arduino 时上拉不影响正常通信
        gpio_set_pull_mode(UART_ECHO_RXD, GPIO_PULLUP_ONLY);
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
            // 看门狗: @busy 后 30 秒未收到 @done(下位机复位/串口丢失)自动清零,
            // 防止状态卡死在 moving 导致 AI 无限轮询
            if (uno_busy_ && (esp_timer_get_time() - uno_busy_since_us_.load()) > 30LL * 1000 * 1000) {
                ESP_LOGW(TAG, "Uno @busy without @done for 30s, force clear");
                uno_busy_ = false;
                WebNotifyUnoStatus();
            }
            int len = uart_read_bytes(ECHO_UART_PORT_NUM, line, sizeof(line) - 1, pdMS_TO_TICKS(200));
            if (len <= 0) {
                continue;
            }
            line[len] = '\0';
            // 按行解析 @busy / @done(可能一次读到多行); 状态变化后置位 pending, 在锁外推送
            bool status_changed = false;
            char* tok = strtok(line, "\r\n");
            while (tok != nullptr) {
                if (strncmp(tok, "@busy", 5) == 0) {
                    uno_busy_ = true;
                    uno_busy_since_us_ = esp_timer_get_time();
                    std::lock_guard<std::mutex> lock(uno_status_mutex_);
                    uno_last_result_ = "busy";
                    if (tok[5] == ' ') {
                        uno_last_action_ = tok + 6;
                    }
                    status_changed = true;
                } else if (strncmp(tok, "@stat ", 6) == 0) {
                    // 事件驱动的状态快照(速度/舵机变化时上报), 后续在此追加可选字段
                    int s = -1, v = -1;
                    if (sscanf(tok, "@stat s%d v%d", &s, &v) == 2) {
                        uno_speed_ = s;
                        uno_servo_ = v;
                        status_changed = true;
                    }
                } else if (strncmp(tok, "@done", 5) == 0) {
                    uno_busy_ = false;
                    std::lock_guard<std::mutex> lock(uno_status_mutex_);
                    uno_last_result_ = "done";
                    if (tok[5] == ' ') {
                        uno_last_action_ = tok + 6;
                    }
                    status_changed = true;
                }
                tok = strtok(nullptr, "\r\n");
            }
            // 状态变化时经 WebSocket 推送给 web 前端(锁已在上面作用域释放, 此处可安全读)
            if (status_changed) {
                WebNotifyUnoStatus();
            }
        }
    }

    // 发送 UART 指令并返回描述性结果(成功/失败), 避免 AI 看到 true/false 无法确认执行结果而重复调用
    // debounce=true 时对相同指令 1 秒防抖(挡 AI 重复调用); web 遥感驾驶走 false(心跳可重复)。
    static std::string SendUartMessage(const char* command_str, bool debounce = true) {
        // 指令防抖：AI 无执行确认机制时可能反复调用相同工具(实测会重复调用几十次,
        // 间隔约 700-900ms)。相同指令 1 秒内只发送一次，避免 Arduino 串口堆积重复指令。
        // 不同指令(动作切换/组合编排)不受影响，照常发送。
        static char s_last_cmd[32] = {};
        static int64_t s_last_us = 0;
        int64_t now = esp_timer_get_time();
        if (debounce && strcmp(s_last_cmd, command_str) == 0 && (now - s_last_us) < 1000000) {
            return std::string("指令已发送(防抖): ") + command_str;  // 防抖丢弃, 视为成功
        }
        if (debounce) {
            snprintf(s_last_cmd, sizeof(s_last_cmd), "%s", command_str);
            s_last_us = now;
        }
        // 统一加 '@' 前缀, 让 Arduino 只认带前缀的命令行(过滤日志乱码)
        int written = uart_write_bytes(ECHO_UART_PORT_NUM, "@", 1);
        if (written < 0) return std::string("指令发送失败: ") + command_str;
        written = uart_write_bytes(ECHO_UART_PORT_NUM, command_str, strlen(command_str));
        if (written < 0) return std::string("指令发送失败: ") + command_str;
        written = uart_write_bytes(ECHO_UART_PORT_NUM, "\n", 1);
        if (written < 0) return std::string("指令发送失败: ") + command_str;
        return std::string("指令已发送: ") + command_str;
    }

    // 设置头部舵机回正角度: 存 NVS + 下发给下位机, 返回说明文本(web /uno 调用)。
    std::string SetServoHome(int value) {
        if (value < 0) value = 0;
        if (value > 180) value = 180;
        Settings settings("servo", true);
        settings.SetInt("home", value);
        std::string cmd = "servo-home-set " + std::to_string(value);
        return SendUartMessage(cmd.c_str(), false);
    }

    // 读取已保存的头部回正角度(默认 82), 返回 JSON(web /uno 调用)。
    std::string GetServoHome() {
        Settings settings("servo", false);
        int v = settings.GetInt("home", 82);
        if (v < 0) v = 0;
        if (v > 180) v = 180;
        return std::string("{\"value\":") + std::to_string(v) + "}";
    }

    // 开机应用: 把 NVS 保存的回正角度下发给下位机(断电重启仍保持用户设置)。
    void ApplyServoHome() {
        Settings settings("servo", false);
        int v = settings.GetInt("home", 82);
        if (v < 0) v = 0;
        if (v > 180) v = 180;
        std::string cmd = "servo-home-set " + std::to_string(v);
        SendUartMessage(cmd.c_str(), false);
        ESP_LOGI(TAG, "Servo home applied: %d", v);
    }

    // 汇总下位机状态为 JSON(mode/action/speed/servo), 供 self.uno.get_status 与 web /uno 接口复用。
    std::string UnoStatusJson() {
        std::string mode, action;
        {
            std::lock_guard<std::mutex> lock(uno_status_mutex_);
            action = uno_last_action_;
        }
        if (uno_busy_) {
            mode = (action == "line-follow") ? "line_follow" : "moving";
        } else {
            mode = "idle";
        }
        int speed = uno_speed_.load();
        int servo = uno_servo_.load();
        auto root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "mode", mode.c_str());
        cJSON_AddStringToObject(root, "action", action.c_str());
        if (speed >= 0) cJSON_AddNumberToObject(root, "speed", speed);
        else cJSON_AddNullToObject(root, "speed");
        if (servo >= 0) cJSON_AddNumberToObject(root, "servo", servo);
        else cJSON_AddNullToObject(root, "servo");
        auto str = cJSON_PrintUnformatted(root);
        std::string result(str);
        cJSON_free(str);
        cJSON_Delete(root);
        return result;
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
                std::string result = SendUartMessage(cmd);
                // 附上确定性的动作时长, 让 AI 知道动作会自动完成停止, 无需查状态确认
                int t_ms = (strcmp(action_str, "left") == 0 || strcmp(action_str, "right") == 0)
                               ? steps * 10
                               : steps * 100;
                if (t_ms % 1000 == 0) {
                    result += "，动作约" + std::to_string(t_ms / 1000) + "秒后自动停止";
                } else {
                    result += "，动作约" + std::to_string(t_ms) + "毫秒后自动停止";
                }
                return result;
            });

        mcp_server.AddTool(
            "self.uno.servo",
            "头部舵机控制，调用一次即转到指定角度。180度舵机，回正角度为82，大于82向左转，小于82向右转。degree: 0-180",
            PropertyList({Property("degree", kPropertyTypeInteger, 82, 0, 180)}),
            [this](const PropertyList& properties) -> ReturnValue {
                int degree = properties["degree"].value<int>();
                char cmd[16];
                snprintf(cmd, sizeof(cmd), "servo-%d", degree);
                return SendUartMessage(cmd) + "，舵机约0.5秒后到位";
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
                return SendUartMessage(cmd) + "，特技执行完毕后自动结束";
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
            "获取 Arduino 下位机(麦克纳姆轮机器人)的状态。返回 JSON: mode(idle=空闲/moving=正在执行动作/"
            "line_follow=巡线中), action(当前或最近动作名), speed(电机速度, null=下位机未上报), "
            "servo(头部舵机角度, null=下位机未上报)。仅当用户询问状态/速度/舵机/在干什么时使用；"
            "发送控制指令后动作会自动完成并停止, 无需查询状态确认。",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                return UnoStatusJson();
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

        // 让 web 摇杆页面能连续控制下位机(/uno REST 接口)
        SetUnoWebApi({
            .send_drive = [this](const std::string& cmd) {
                // web 遥感为高频心跳, 绕过 1 秒防抖(防抖只用于点动)
                return SendUartMessage(cmd.c_str(), false);
            },
            .get_status = [this]() { return UnoStatusJson(); },
            .set_servo_home = [this](int value) { return SetServoHome(value); },
            .get_servo_home = [this]() { return GetServoHome(); },
        });
        ApplyServoHome();   // 开机把 NVS 保存的回正角度下发给下位机
    }

    // 网络状态查询工具（可扩展的网络信息入口，当前返回 IP，后续可加 SSID/信号/MAC 等）
    void InitializeNetworkTools() {
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool(
            "self.network.get_status",
            "查询当前网络状态。返回本机网络信息(JSON)：ip(局域网 IPv4)、connected(是否已连接WiFi)、ssid(连接的WiFi名)、"
            "signal(信号强弱: strong/medium/weak)、rssi(信号原始值dBm)。未连接时 ip/ssid 为空。"
            "用于回答\"当前 IP 是多少\"、\"连的哪个WiFi\"、\"网速/信号好不好\"，或引导用户访问本机 Web(如 http://<ip>)。"
            "后续可在此工具继续扩展字段(channel/mac 等)",
            PropertyList(),
            [this](const PropertyList& props) -> ReturnValue {
                auto& wifi = WifiManager::GetInstance();
                auto root = cJSON_CreateObject();
                std::string ip = wifi.GetIpAddress();
                cJSON_AddStringToObject(root, "ip", ip.empty() ? "" : ip.c_str());
                cJSON_AddBoolToObject(root, "connected", wifi.IsConnected());
                cJSON_AddStringToObject(root, "ssid", wifi.GetSsid().c_str());
                int rssi = wifi.GetRssi();
                cJSON_AddNumberToObject(root, "rssi", rssi);
                const char* signal = rssi >= -60 ? "strong" : (rssi >= -70 ? "medium" : "weak");
                cJSON_AddStringToObject(root, "signal", signal);
                auto str = cJSON_PrintUnformatted(root);
                std::string result(str);
                cJSON_free(str);
                cJSON_Delete(root);
                return result;
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
        ApplyCameraFlip();              // 应用 NVS 保存的摄像头翻转设置
        InitializeClock();              // 应用 NVS 时钟设置 + 启动每秒刷新定时器
#ifdef CONFIG_XIAOZHI_AIROBOT_ENABLE_TF_CARD
        InitializeSDCard();
        InitializeUploadServer();
        InitializeMusicTools();
        InitializeAlarmTools();
#endif
        InitializeIpDisplay();
        InitializeEchoUart();
        InitializeUnoTools();
        InitializeCameraTools();
        InitializeClockTools();
        InitializeNetworkTools();
        InitializeDebugTools();
        // 默认把日志压到 ERROR, 避免 GPIO43 日志污染 Arduino 串口(平时命令更稳定)
        esp_log_level_set("*", ESP_LOG_ERROR);
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
