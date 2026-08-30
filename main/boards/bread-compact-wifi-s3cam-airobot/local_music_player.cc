#include "local_music_player.h"

#include "application.h"
#include "board.h"
#include "display/display.h"

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <algorithm>
#include <random>
#include <chrono>

#include <esp_log.h>
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_audio_dec_default.h"
#include "esp_ae_ch_cvt.h"
#include "esp_pthread.h"

#define TAG "LocalMusicPlayer"

#define MUSIC_DIR "/sdcard/music"

namespace {
// std::thread 底层 pthread 默认栈仅 3KB(CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT=3072)，
// esp_mp3_dec(Helix) 解码需要约 20KB 栈(esp_audio_codec 文档要求)，不加大会栈溢出导致设备重启。
constexpr size_t kPlayThreadStackBytes = 32 * 1024;

// 以显式大栈创建播放线程；创建后恢复默认 pthread 配置，避免影响后续其它线程。
std::thread CreatePlayThread(void (LocalMusicPlayer::*task)(), LocalMusicPlayer* self) {
    esp_pthread_cfg_t saved = esp_pthread_get_default_config();
    esp_pthread_cfg_t cfg = saved;
    cfg.stack_size = kPlayThreadStackBytes;
    cfg.thread_name = "music_play";
    cfg.prio = 2;  // 低于音频/AFE(8)、主循环(10)、httpd(6)，解码忙时不抢交互任务
    esp_pthread_set_cfg(&cfg);
    std::thread t(task, self);
    esp_pthread_set_cfg(&saved);
    return t;
}

// ---- LRC 歌词（标准 [mm:ss.xx] 格式，UTF-8 编码）----
struct LyricLine {
    int start_ms;
    std::string text;
};

// 简单 UTF-8 合法性校验：GBK 编码的 .lrc 无法在设备端转换(ESP-IDF v6 无 iconv)，
// 需先用本板 scripts/mp3_convert_for_esp32s3.py 转成 UTF-8。
static bool IsValidUtf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            i++;
            continue;
        }
        int extra = 0;
        if ((c & 0xE0) == 0xC0) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0) extra = 3;
        else return false;
        if (i + extra >= s.size()) return false;
        for (int k = 1; k <= extra; k++) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += extra + 1;
    }
    return true;
}

// 读取并解析同名 .lrc；支持一行多个时间标签(如 [00:10.00][00:20.00]歌词)，
// 跳过 [ti:][ar:][offset:] 等元数据行。返回是否成功加载到有效歌词。
static bool LoadLyrics(const std::string& mp3_path, std::vector<LyricLine>& out) {
    std::string lrc_path = mp3_path.substr(0, mp3_path.size() - 4) + ".lrc";
    FILE* f = fopen(lrc_path.c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    std::string content;
    char buf[256];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) {
        content.append(buf, r);
    }
    fclose(f);
    if (content.empty()) {
        return false;
    }
    if (!IsValidUtf8(content)) {
        ESP_LOGW(TAG, "Lyrics %s is not UTF-8 (likely GBK), run mp3_convert_for_esp32s3.py to convert",
                 lrc_path.c_str());
        return false;
    }

    size_t pos = 0;
    while ((pos = content.find('[', pos)) != std::string::npos) {
        size_t rb = content.find(']', pos);
        if (rb == std::string::npos) {
            break;
        }
        int min = 0, sec = 0, ms = 0;
        if (sscanf(content.c_str() + pos + 1, "%d:%d.%d", &min, &sec, &ms) < 2) {
            pos = rb + 1;  // [ti:..] 等元数据行，跳过
            continue;
        }
        size_t text_start = rb + 1;
        size_t next_lb = content.find('[', text_start);
        size_t text_end = (next_lb == std::string::npos) ? content.size() : next_lb;
        std::string text = content.substr(text_start, text_end - text_start);
        while (!text.empty() &&
               (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) {
            text.pop_back();
        }
        if (!text.empty()) {
            out.push_back({min * 60000 + sec * 1000 + ms * 10, text});
        }
        pos = rb + 1;
    }
    if (out.empty()) {
        return false;
    }
    std::sort(out.begin(), out.end(),
              [](const LyricLine& a, const LyricLine& b) { return a.start_ms < b.start_ms; });
    return true;
}

// 返回当前播放位置对应的歌词行（取最后一个 start_ms <= pos_ms 的行）
static std::string LyricAt(const std::vector<LyricLine>& lyrics, int pos_ms) {
    const LyricLine* cur = nullptr;
    for (const auto& l : lyrics) {
        if (l.start_ms <= pos_ms) {
            cur = &l;
        } else {
            break;
        }
    }
    return cur ? cur->text : "";
}
}  // namespace

LocalMusicPlayer::LocalMusicPlayer(AudioService& audio_service)
    : audio_service_(audio_service) {}

LocalMusicPlayer::~LocalMusicPlayer() {
    Stop();
    if (play_thread_.joinable()) {
        play_thread_.join();
    }
}

void LocalMusicPlayer::ScanSongs() {
    std::lock_guard<std::mutex> lock(songs_mutex_);
    songs_.clear();
    DIR* dir = opendir(MUSIC_DIR);
    if (dir == nullptr) {
        ESP_LOGW(TAG, "Cannot open music dir %s", MUSIC_DIR);
        return;
    }
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type != DT_REG) {
            continue;
        }
        std::string name = entry->d_name;
        if (name.size() < 4) {
            continue;
        }
        std::string ext = name.substr(name.size() - 4);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".mp3") {
            songs_.push_back(name);
        }
    }
    closedir(dir);
    std::sort(songs_.begin(), songs_.end());
    ESP_LOGI(TAG, "Found %u songs in %s", static_cast<unsigned>(songs_.size()), MUSIC_DIR);
}

std::vector<std::string> LocalMusicPlayer::ListSongs() const {
    std::lock_guard<std::mutex> lock(songs_mutex_);
    return songs_;
}

std::string LocalMusicPlayer::PickNextSong() {
    // 注意: 调用方(PlayTask)必须已持有 state_mutex_ 才能调用本函数,
    // 此处不再加锁 —— std::mutex 非递归, 重复加同一把锁会死锁
    if (queue_pos_ < play_queue_.size()) {
        return play_queue_[queue_pos_++];
    }
    return "";  // 队列播完
}

std::string LocalMusicPlayer::FindSong(const std::string& name) {
    if (name.empty()) {
        return "";  // 空名不匹配任何歌(否则 find("") 会命中第一首)
    }
    std::lock_guard<std::mutex> lock(songs_mutex_);
    for (const auto& s : songs_) {
        if (s == name || s.find(name) != std::string::npos) {
            return s;
        }
    }
    // 忽略大小写再找一遍
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    for (const auto& s : songs_) {
        std::string sl = s;
        std::transform(sl.begin(), sl.end(), sl.begin(), ::tolower);
        if (sl.find(lower) != std::string::npos) {
            return s;
        }
    }
    return "";
}

bool LocalMusicPlayer::PlayRandom() {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        std::lock_guard<std::mutex> songs_lock(songs_mutex_);
        // 随机顺序: 洗牌全部歌曲作为播放队列, 播完队列自动停止
        play_queue_ = songs_;
        std::shuffle(play_queue_.begin(), play_queue_.end(),
                     std::mt19937(std::random_device{}()));
        queue_pos_ = 0;
        pending_song_.clear();
    }
    if (playing_.load()) {
        return true;  // 已在播放，保持当前队列(不打断)
    }
    playing_ = true;
    paused_ = false;
    stop_requested_ = false;
    if (play_thread_.joinable()) {
        play_thread_.join();
    }
    play_thread_ = CreatePlayThread(&LocalMusicPlayer::PlayTask, this);
    return true;
}

std::string LocalMusicPlayer::PlaySong(const std::string& name) {
    if (name.empty()) {
        return "请提供歌曲名";
    }
    std::string found = FindSong(name);
    if (found.empty()) {
        ESP_LOGW(TAG, "Song not found: %s", name.c_str());
        // 返回描述性文本(而非 false), 让 AI 明确知道未找到并引导用户,
        // 避免 AI 反复瞎猜歌名重试(实测: 模糊歌名"黄玲"vs"黄龄"会卡在重试)
        return "未找到歌曲 \"" + name + "\", 请确认歌名或让用户提供准确名称";
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        std::lock_guard<std::mutex> songs_lock(songs_mutex_);
        pending_song_ = found;
        // 顺序播放: 队列 = 全部歌曲(字典序), 从指定歌曲的下一首开始播, 播完队列停止
        play_queue_ = songs_;
        auto it = std::find(play_queue_.begin(), play_queue_.end(), found);
        if (it != play_queue_.end()) {
            queue_pos_ = static_cast<size_t>(it - play_queue_.begin()) + 1;
        } else {
            queue_pos_ = play_queue_.size();  // 未找到(理论上不会), 队列为空
        }
    }
    if (playing_.load()) {
        return "正在播放, 已切到: " + found;  // 播放中换歌
    }
    playing_ = true;
    paused_ = false;
    stop_requested_ = false;
    if (play_thread_.joinable()) {
        play_thread_.join();
    }
    play_thread_ = CreatePlayThread(&LocalMusicPlayer::PlayTask, this);
    return "已开始播放: " + found;
}

void LocalMusicPlayer::Pause() {
    paused_ = true;
}

void LocalMusicPlayer::Resume() {
    paused_ = false;
}

void LocalMusicPlayer::Stop() {
    stop_requested_ = true;
    paused_ = false;
    playing_ = false;
    // 清空播放队列，尽快停止出声（本地播放时 opus 通道不活动，安全）
    audio_service_.ResetDecoder();
}

void LocalMusicPlayer::PlayTask() {
    // 播放会话开始：记录当前设备状态作为打断检测基准
    // （AI 命令启动播放时设备处于 Speaking/Listening，此时不允许立即打断）
    interaction_state_ = Application::GetInstance().GetDeviceState();
    while (!stop_requested_.load() && playing_.load()) {
        if (paused_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        std::string song;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (!pending_song_.empty()) {
                song = pending_song_;
                pending_song_.clear();
            } else {
                song = PickNextSong();
            }
        }
        if (song.empty()) {
            // 队列播完(顺序/随机均到末尾)或没有任何歌曲
            playing_ = false;
            break;
        }
        PlayOneSong(std::string(MUSIC_DIR) + "/" + song);
    }
    playing_ = false;
    // 自然播完(非外部停止)且状态仍是我们钉住的 Speaking -> 回到待命；
    // 外部停止(MCP stop/唤醒/按钮)时状态由对话/唤醒流程接管，不干预
    if (!stop_requested_.load() &&
        Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking) {
        Application::GetInstance().SetDeviceState(kDeviceStateIdle);
    }
}

void LocalMusicPlayer::PlayOneSong(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        ESP_LOGW(TAG, "Cannot open %s", path.c_str());
        return;
    }

    // 歌词：加载同名 .lrc(UTF-8)，播放时逐行显示在屏幕底部字幕条
    std::vector<LyricLine> lyrics;
    if (LoadLyrics(path, lyrics)) {
        ESP_LOGI(TAG, "Lyrics loaded: %zu lines", lyrics.size());
    }
    auto* display = Board::GetInstance().GetDisplay();
    std::string shown_lyric;
    auto last_lyric_update = std::chrono::steady_clock::now();

    // 注册默认解码器（含 MP3，由 CONFIG_AUDIO_DECODER_MP3_SUPPORT 控制），只注册一次。
    // 需要两层都注册：decoder 层(esp_audio_dec_register_default)注册解码器 ops，
    // simple_dec 层(esp_audio_simple_dec_register_default)注册 parser。
    static bool s_dec_registered = false;
    if (!s_dec_registered) {
        esp_audio_dec_register_default();
        esp_audio_simple_dec_register_default();
        s_dec_registered = true;
    }

    // 官方 MP3 简单解码器（带 parser）：自动跳过 ID3v2 标签、搜索帧同步、处理跨块边界，
    // 支持任意大小输入。底层 esp_mp3_dec 不处理这些，遇 ID3 标签会整首报 Not supported format。
    esp_audio_simple_dec_cfg_t dec_cfg = {};
    dec_cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    dec_cfg.dec_cfg = nullptr;
    dec_cfg.cfg_size = 0;
    dec_cfg.use_frame_dec = false;  // parser 模式（非帧模式）

    esp_audio_simple_dec_handle_t decoder = nullptr;
    if (esp_audio_simple_dec_open(&dec_cfg, &decoder) != ESP_AUDIO_ERR_OK || decoder == nullptr) {
        ESP_LOGE(TAG, "Failed to create mp3 decoder");
        fclose(f);
        return;
    }
    // 官方声道转换器：立体声(交错 L/R) -> 单声道，见下方 channel == 2 分支
    esp_ae_ch_cvt_handle_t ch_cvt = nullptr;

    // MP3 一帧(44.1kHz 立体声 16bit) = 4608B；若输出不足会按 needed_size 自动扩容
    std::vector<uint8_t> pcm_buf(8192);
    // 立体声降混的输出缓冲：函数级复用，避免每帧堆分配（resize 不释放容量）
    std::vector<int16_t> mono;
    int sample_rate = 0;
    int channels = 1;
    // 精确节流基准：从本曲开始累计注入样本数，用 sleep_until 对准绝对时间点，
    // 避免逐帧 sleep_for 的累积漂移导致越播越慢。
    auto play_start = std::chrono::steady_clock::now();
    uint64_t injected_samples = 0;

    while (!stop_requested_.load() && playing_.load()) {
        // 打断检测：仅当设备从 Idle 变为 Listening/Connecting(真正的用户唤醒/交互信号)时打断。
        // 播放线程每帧自钉 Speaking, 服务器 goodbye 也会造成 idle->speaking 波动,
        // 若按“非Idle即打断”会把会话超时误判为用户交互导致误停(实测: 听歌时服务器
        // 长时间无交互自动结束会话 -> 播放被误停)。唤醒词/按钮打断走明确 hook, 不受影响。
        auto state = Application::GetInstance().GetDeviceState();
        if (interaction_state_ == kDeviceStateIdle &&
            (state == kDeviceStateListening || state == kDeviceStateConnecting)) {
            ESP_LOGI(TAG, "User interaction detected, stop local playback");
            display->ClearChatMessages();
            Stop();
            return;
        }
        interaction_state_ = state;
        if (paused_.load()) {
            // 暂停中：不钉状态，交给对话/状态机流程
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        // 播放期间保持“说话中”(Speaking)状态：屏幕明确显示设备在播放，
        // 唤醒打断走 xiaozhi 标准的 Speaking 分支(AbortSpeaking)。
        // 服务器 tts:stop 会把状态切回 Listening，这里每帧钉回 Speaking。
        // 但状态已到 Idle(会话结束/goodbye)时不再钉回, 避免状态卡在说话中;
        // 此时歌曲继续播, 状态保持待命, 播完由 PlayTask 末尾逻辑处理。
        if (state != kDeviceStateSpeaking && state != kDeviceStateIdle) {
            Application::GetInstance().SetDeviceState(kDeviceStateSpeaking);
        }

        uint8_t in[4096];
        size_t r = fread(in, 1, sizeof(in), f);
        bool eos = (r == 0);  // 文件读尽：通知 parser flush 内部缓存

        esp_audio_simple_dec_raw_t raw = {};
        raw.buffer = in;
        raw.len = static_cast<uint32_t>(r);
        raw.eos = eos;
        raw.frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE;

        // 本块输入可能产出 0..N 帧 PCM，循环处理直到消费完或 eos flush 完毕
        int stall = 0;  // 连续无进展计数(防御解码状态异常死循环)
        while (true) {
            esp_audio_simple_dec_out_t frame = {};
            frame.buffer = pcm_buf.data();
            frame.len = static_cast<uint32_t>(pcm_buf.size());

            auto ret = esp_audio_simple_dec_process(decoder, &raw, &frame);
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                if (frame.needed_size > pcm_buf.size()) {
                    pcm_buf.resize(frame.needed_size);
                    continue;  // 扩容后重试，raw 未消费数据保留
                }
                // 缓冲已够大仍报 not_enough：解码状态异常，放弃本块避免死循环卡死
                ESP_LOGW(TAG, "Decoder BUFF_NOT_ENOUGH size=%u need=%u, skip block",
                         (unsigned)pcm_buf.size(), (unsigned)frame.needed_size);
                break;
            }
            if (ret != ESP_AUDIO_ERR_OK) {
                break;  // 解码错误：放弃本块输入（parser 已跳过 ID3/垃圾数据）
            }
            if (frame.decoded_size > 0) {
                stall = 0;
                // 防御: 解码输出异常超大(状态异常)时放弃本块, 避免大分配/样本数爆炸
                if (frame.decoded_size > pcm_buf.size() * 4) {
                    ESP_LOGW(TAG, "Decoder decoded_size %u too large, skip block",
                             (unsigned)frame.decoded_size);
                    break;
                }
                if (sample_rate == 0) {
                    esp_audio_simple_dec_info_t info = {};
                    if (esp_audio_simple_dec_get_info(decoder, &info) == ESP_AUDIO_ERR_OK) {
                        sample_rate = static_cast<int>(info.sample_rate);
                        channels = (info.channel > 0) ? info.channel : 1;
                    }
                }
                int sr = sample_rate ? sample_rate : 44100;
                std::vector<int16_t> pcm(frame.decoded_size / sizeof(int16_t));
                memcpy(pcm.data(), frame.buffer, frame.decoded_size);
                // MP3 多为立体声(交错 L/R)，而 AudioService 重采样与 I2S 输出均为单声道；
                // 用官方 ESP-Audio-Effects 声道转换器(esp_ae_ch_cvt)统一降混为单声道，
                // 不降混直接按单声道处理会变成噪音(滋滋呜呜)。
                if (channels == 2) {
                    if (ch_cvt == nullptr) {
                        esp_ae_ch_cvt_cfg_t cfg = {};
                        cfg.sample_rate = static_cast<uint32_t>(sr);
                        cfg.bits_per_sample = 16;
                        cfg.src_ch = 2;
                        cfg.dest_ch = 1;
                        cfg.weight = nullptr;  // 默认 1/src_ch_num，即左右声道平均
                        cfg.weight_len = 0;
                        esp_ae_ch_cvt_open(&cfg, &ch_cvt);
                        if (ch_cvt == nullptr) {
                            ESP_LOGE(TAG, "Failed to create channel converter, skip frame");
                            break;
                        }
                    }
                    mono.resize(pcm.size() / 2);
                    esp_ae_ch_cvt_process(ch_cvt, static_cast<uint32_t>(pcm.size() / 2),
                                          pcm.data(), mono.data());
                    pcm.swap(mono);
                }
                // 节流：按累计注入样本数推算期望播放时间点，解码慢时不额外等待(不会越播越慢)
                uint32_t samples = static_cast<uint32_t>(pcm.size());
                audio_service_.PushLocalPcm(std::move(pcm), sr);
                injected_samples += samples;
                // 歌词显示：每 500ms 检查一次当前播放位置(按注入样本数推算)对应的歌词行，
                // 行变化时刷新到底部字幕条
                if (!lyrics.empty() && sr > 0) {
                    auto now = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(now -
                                                                               last_lyric_update)
                            .count() >= 500) {
                        last_lyric_update = now;
                        int pos_ms = static_cast<int>(injected_samples * 1000 / sr);
                        std::string line = LyricAt(lyrics, pos_ms);
                        if (line != shown_lyric) {
                            shown_lyric = line;
                            display->SetChatMessage("system", line.c_str());
                        }
                    }
                }
                if (sr > 0) {
                    auto target = play_start +
                                  std::chrono::microseconds(injected_samples * 1000000 / sr);
                    auto now = std::chrono::steady_clock::now();
                    // 防御: 目标时间异常超前(样本数跳变)时重置基准, 避免 sleep_until 远睡假死
                    if (target > now + std::chrono::seconds(2)) {
                        ESP_LOGW(TAG, "Throttle target far ahead, reset base");
                        play_start = now -
                                     std::chrono::microseconds(injected_samples * 1000000 / sr);
                        target = now;
                    }
                    std::this_thread::sleep_until(target);
                }
                if (paused_.load() || stop_requested_.load() || !playing_.load()) {
                    break;
                }
            }
            if (raw.consumed > 0) {
                raw.buffer += raw.consumed;
                raw.len -= raw.consumed;
                stall = 0;
            } else if (frame.decoded_size == 0 && ++stall > 50) {
                // 连续 50 次无任何进展(不消费输入、不产出数据)：状态机异常，放弃本块
                ESP_LOGW(TAG, "Decoder stalled, skip block");
                break;
            }
            if (raw.consumed == 0 && frame.decoded_size == 0) {
                break;  // 无进展：eos flush 完毕或等待更多输入
            }
            if (!eos && raw.len == 0) {
                break;  // 本块输入消费完，读下一块
            }
        }

        if (eos) {
            break;  // 文件结束
        }
    }

    if (display != nullptr) {
        display->ClearChatMessages();  // 播放结束/被打断：清掉歌词
    }
    if (ch_cvt != nullptr) {
        esp_ae_ch_cvt_close(ch_cvt);
    }
    esp_audio_simple_dec_close(decoder);
    fclose(f);
}
