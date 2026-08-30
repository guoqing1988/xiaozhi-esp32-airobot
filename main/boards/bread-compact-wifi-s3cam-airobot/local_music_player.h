#ifndef LOCAL_MUSIC_PLAYER_H
#define LOCAL_MUSIC_PLAYER_H

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>

#include "audio_service.h"
#include "device_state.h"

// 本地 TF 卡音乐播放器。
// 负责扫描 /sdcard/music 下的 MP3，并通过 AudioService 的本地播放接口
// 将解码后的 PCM 注入现有播放队列（复用扬声器 I2S 路径）。
// 支持随机播放、指定播放、连播、暂停/恢复、停止。
class LocalMusicPlayer {
public:
    explicit LocalMusicPlayer(AudioService& audio_service);
    ~LocalMusicPlayer();

    // 扫描歌曲列表（挂载成功后调用）
    void ScanSongs();

    // ---- MCP 工具回调（由 Board 注册到 McpServer）----
    bool PlayRandom();
    bool PlaySong(const std::string& name);
    void Pause();
    void Resume();
    void Stop();

    std::vector<std::string> ListSongs() const;
    bool IsPlaying() const { return playing_.load(); }

private:
    void PlayTask();
    void PlayOneSong(const std::string& path);
    std::string PickNextSong(bool random);
    std::string FindSong(const std::string& name);

    AudioService& audio_service_;
    std::vector<std::string> songs_;             // 歌曲名（不含路径）
    mutable std::mutex songs_mutex_;
    std::atomic<bool> playing_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> continuous_{true};         // 默认连播
    std::atomic<bool> random_{true};             // 随机播放
    std::string pending_song_;                    // 指定要播的歌曲
    std::mutex state_mutex_;
    std::thread play_thread_;

    // 打断检测：记录上次检查的设备状态，仅当“Idle -> 非 Idle”转换(唤醒词/按钮触发交互)
    // 时打断本地播放。AI 命令启动播放时状态为 Speaking/Listening，保持播放不打断。
    DeviceState interaction_state_ = kDeviceStateIdle;
};

#endif // LOCAL_MUSIC_PLAYER_H
