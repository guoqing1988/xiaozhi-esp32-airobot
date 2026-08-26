#include "local_music_player.h"

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <algorithm>
#include <random>
#include <chrono>

#include <esp_log.h>
#include "esp_mp3_dec.h"

#define TAG "LocalMusicPlayer"

#define MUSIC_DIR "/sdcard/music"

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

std::string LocalMusicPlayer::PickNextSong(bool random) {
    std::lock_guard<std::mutex> lock(songs_mutex_);
    if (songs_.empty()) {
        return "";
    }
    if (random) {
        static std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<size_t> dist(0, songs_.size() - 1);
        return songs_[dist(rng)];
    }
    return songs_[0];
}

std::string LocalMusicPlayer::FindSong(const std::string& name) {
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
        continuous_ = true;
        random_ = true;
    }
    if (playing_.load()) {
        return true;  // 已在播放，保持随机连播
    }
    playing_ = true;
    paused_ = false;
    stop_requested_ = false;
    if (play_thread_.joinable()) {
        play_thread_.join();
    }
    play_thread_ = std::thread(&LocalMusicPlayer::PlayTask, this);
    return true;
}

bool LocalMusicPlayer::PlaySong(const std::string& name) {
    std::string found = FindSong(name);
    if (found.empty()) {
        ESP_LOGW(TAG, "Song not found: %s", name.c_str());
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        pending_song_ = found;
        random_ = false;
        continuous_ = true;
    }
    if (playing_.load()) {
        return true;  // 正在播放，PlayTask 下一轮会处理 pending_song_
    }
    playing_ = true;
    paused_ = false;
    stop_requested_ = false;
    if (play_thread_.joinable()) {
        play_thread_.join();
    }
    play_thread_ = std::thread(&LocalMusicPlayer::PlayTask, this);
    return true;
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
                song = PickNextSong(random_.load());
            }
        }
        if (song.empty()) {
            // 没有任何歌曲
            playing_ = false;
            break;
        }
        PlayOneSong(std::string(MUSIC_DIR) + "/" + song);
        if (!continuous_.load()) {
            break;
        }
    }
    playing_ = false;
}

void LocalMusicPlayer::PlayOneSong(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        ESP_LOGW(TAG, "Cannot open %s", path.c_str());
        return;
    }

    void* decoder = nullptr;
    if (esp_mp3_dec_open(nullptr, 0, &decoder) != ESP_AUDIO_ERR_OK || decoder == nullptr) {
        ESP_LOGE(TAG, "Failed to create mp3 decoder");
        fclose(f);
        return;
    }

    std::vector<uint8_t> remain;
    std::vector<uint8_t> pcm_buf(4096);
    int sample_rate = 0;

    while (!stop_requested_.load() && playing_.load()) {
        if (paused_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (remain.empty()) {
            // 从文件补充输入
            uint8_t in[2048];
            size_t r = fread(in, 1, sizeof(in), f);
            if (r == 0) {
                break;  // EOF
            }
            remain.insert(remain.end(), in, in + r);
        }

        esp_audio_dec_in_raw_t raw = {};
        raw.buffer = remain.data();
        raw.len = static_cast<uint32_t>(remain.size());
        raw.frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE;

        uint32_t consumed_total = 0;
        bool any_output = false;
        while (raw.len > 0) {
            esp_audio_dec_info_t info = {};
            esp_audio_dec_out_frame_t frame = {};
            frame.buffer = pcm_buf.data();
            frame.len = static_cast<uint32_t>(pcm_buf.size());

            auto ret = esp_mp3_dec_decode(decoder, &raw, &frame, &info);
            if (ret != ESP_AUDIO_ERR_OK && ret != ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                // 解码错误：跳过剩余输入，播下一首
                consumed_total += raw.len;
                raw.len = 0;
                break;
            }
            if (ret == ESP_AUDIO_ERR_OK && frame.decoded_size > 0) {
                if (sample_rate == 0) {
                    sample_rate = static_cast<int>(info.sample_rate);
                }
                int sr = sample_rate ? sample_rate : static_cast<int>(info.sample_rate);
                std::vector<int16_t> pcm(frame.decoded_size / sizeof(int16_t));
                memcpy(pcm.data(), frame.buffer, frame.decoded_size);
                audio_service_.PushLocalPcm(std::move(pcm), sr);

                // 节流，使注入速率接近实时播放
                uint32_t samples = frame.decoded_size / sizeof(int16_t);
                if (sr > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(samples * 1000 / sr));
                }
                any_output = true;
                if (paused_.load() || stop_requested_.load() || !playing_.load()) {
                    break;
                }
            }
            if (raw.consumed == 0) {
                break;  // 输入不足，等待更多数据
            }
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
            consumed_total += raw.consumed;
        }

        if (consumed_total > 0) {
            if (consumed_total > remain.size()) {
                consumed_total = static_cast<uint32_t>(remain.size());
            }
            remain.erase(remain.begin(), remain.begin() + consumed_total);
        } else if (!any_output && feof(f)) {
            // 文件读尽但无法进一步解码，结束
            break;
        }
    }

    esp_mp3_dec_close(decoder);
    fclose(f);
}
