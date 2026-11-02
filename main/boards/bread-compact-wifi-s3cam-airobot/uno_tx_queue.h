#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// 外部调用方负责同步；不在持锁时执行 UART 写入。
class UnoTxQueue {
public:
    static constexpr size_t kCommandSize = 64;
    static constexpr size_t kCapacity = 8;

    bool Push(const char* command, int64_t now_us) {
        cancelled_ = 0;
        const size_t len = strlen(command);
        if (len == 0 || len >= kCommandSize) return false;
        if (strcmp(command, "drive-stop") == 0 || strcmp(command, "go-stop-0") == 0 ||
            strcmp(command, "line-stop") == 0) {
            // 停止取消已经排队但尚未发送的驾驶方向，避免停止后再次启动。
            drive_pending_ = false;
            size_t kept = 0;
            for (size_t i = 0; i < count_; ++i) {
                const size_t src = (head_ + i) % kCapacity;
                if (strncmp(queue_[src], "go-", 3) == 0 ||
                    strncmp(queue_[src], "tj-", 3) == 0 ||
                    strcmp(queue_[src], "line-start") == 0) {
                    ++cancelled_;
                    continue;
                }
                const size_t dst = (head_ + kept++) % kCapacity;
                if (src != dst) memcpy(queue_[dst], queue_[src], kCommandSize);
            }
            count_ = kept;
            memcpy(stop_, command, len + 1);
            stop_pending_ = true;
            return true;
        }
        if (strncmp(command, "drive-", 6) == 0) {
            memcpy(drive_, command, len + 1);
            drive_pending_ = true;
            drive_time_us_ = now_us;
            return true;
        }
        if (count_ == kCapacity) return false;
        memcpy(queue_[(head_ + count_) % kCapacity], command, len + 1);
        ++count_;
        return true;
    }

    size_t Cancelled() const { return cancelled_; }

    bool Pop(char (&out)[kCommandSize], int64_t now_us) {
        // 过期的驾驶心跳不能在长时间阻塞后重新启动电机。
        if (drive_pending_ && now_us - drive_time_us_ > 500000) drive_pending_ = false;
        if (stop_pending_) {
            memcpy(out, stop_, kCommandSize);
            stop_pending_ = false;
        } else if (count_ != 0) {
            memcpy(out, queue_[head_], kCommandSize);
            head_ = (head_ + 1) % kCapacity;
            --count_;
        } else if (drive_pending_) {
            memcpy(out, drive_, kCommandSize);
            drive_pending_ = false;
        } else {
            return false;
        }
        return true;
    }

private:
    char queue_[kCapacity][kCommandSize] = {};
    char stop_[kCommandSize] = {};
    char drive_[kCommandSize] = {};
    size_t head_ = 0;
    size_t count_ = 0;
    size_t cancelled_ = 0;
    bool stop_pending_ = false;
    bool drive_pending_ = false;
    int64_t drive_time_us_ = 0;
};
