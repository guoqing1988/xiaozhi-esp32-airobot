#pragma once

#include <cstddef>

// UART 是字节流；超长行必须整体丢弃，不能把尾部误认为新消息。
class UnoLineParser {
public:
    template <typename Callback>
    void Feed(const char* data, size_t length, Callback on_line) {
        for (size_t i = 0; i < length; ++i) {
            const char ch = data[i];
            if (ch == '\n' || ch == '\r') {
                if (!discarding_ && length_ != 0) {
                    line_[length_] = '\0';
                    on_line(line_);
                }
                length_ = 0;
                discarding_ = false;
            } else if (!discarding_) {
                if (length_ == sizeof(line_) - 1) {
                    length_ = 0;
                    discarding_ = true;
                } else {
                    line_[length_++] = ch;
                }
            }
        }
    }

private:
    char line_[128] = {};
    size_t length_ = 0;
    bool discarding_ = false;
};
