#pragma once

#include <cstddef>
#include <cstdint>

namespace micropixel::platform::transports {

// Fixed-storage printable-ASCII line parser for byte-stream transports. The
// caller owns the buffer, so a board can place it in PSRAM or static memory.
// Invalid and overlong input is discarded through the next newline instead of
// accidentally accepting a trailing fragment as a command.
class AsciiLineFramer final {
   public:
    void Bind(char* buffer, size_t capacity) {
        buffer_ = buffer;
        capacity_ = capacity;
        length_ = 0U;
        discarding_ = buffer == nullptr || capacity < 2U;
    }

    template <typename LineSink>
    void Consume(const uint8_t* bytes, size_t size, LineSink&& sink) {
        if (bytes == nullptr) {
            return;
        }
        for (size_t index = 0U; index < size; ++index) {
            const uint8_t byte = bytes[index];
            if (byte == '\r') {
                continue;
            }
            if (byte == '\n') {
                if (!discarding_ && length_ != 0U) {
                    buffer_[length_] = '\0';
                    sink(buffer_);
                }
                length_ = 0U;
                discarding_ = buffer_ == nullptr || capacity_ < 2U;
                continue;
            }
            if (discarding_) {
                continue;
            }
            if (byte < 0x20U || byte > 0x7eU || length_ + 1U >= capacity_) {
                length_ = 0U;
                discarding_ = true;
                continue;
            }
            buffer_[length_++] = static_cast<char>(byte);
        }
    }

   private:
    char* buffer_{};
    size_t capacity_{};
    size_t length_{};
    bool discarding_{true};
};

}  // namespace micropixel::platform::transports
