#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

namespace micropixel::platform::esp_mosaico {

class UsbCdcEarlyLogBuffer final {
   public:
    explicit UsbCdcEarlyLogBuffer(std::span<char> storage = {}) : storage_(storage) {}

    void Reset(std::span<char> storage) {
        storage_ = storage;
        Clear();
    }

    void Release() {
        storage_ = {};
        Clear();
    }

    void Append(std::span<const char> bytes) {
        const size_t capacity = storage_.size();
        if (bytes.empty() || capacity == 0U) {
            return;
        }
        if (bytes.size() >= capacity) {
            dropped_bytes_ += size_ + bytes.size() - capacity;
            std::copy(bytes.end() - static_cast<std::ptrdiff_t>(capacity), bytes.end(), storage_.begin());
            start_ = 0U;
            size_ = capacity;
            return;
        }

        const size_t free_bytes = capacity - size_;
        if (bytes.size() > free_bytes) {
            const size_t displaced = bytes.size() - free_bytes;
            start_ = (start_ + displaced) % capacity;
            size_ -= displaced;
            dropped_bytes_ += displaced;
        }

        const size_t write_start = (start_ + size_) % capacity;
        const size_t first_size = std::min(bytes.size(), capacity - write_start);
        std::copy_n(bytes.begin(), first_size, storage_.begin() + static_cast<std::ptrdiff_t>(write_start));
        std::copy(bytes.begin() + static_cast<std::ptrdiff_t>(first_size), bytes.end(), storage_.begin());
        size_ += bytes.size();
    }

    [[nodiscard]] std::array<std::span<const char>, 2U> Segments() const {
        if (storage_.empty()) {
            return {};
        }
        const size_t first_size = std::min(size_, storage_.size() - start_);
        return {
            std::span<const char>(storage_.data() + start_, first_size),
            std::span<const char>(storage_.data(), size_ - first_size),
        };
    }

    [[nodiscard]] size_t size() const { return size_; }
    [[nodiscard]] size_t dropped_bytes() const { return dropped_bytes_; }
    [[nodiscard]] bool has_storage() const { return !storage_.empty(); }

    void AccountDropped(size_t bytes) { dropped_bytes_ += bytes; }

    void Clear() {
        start_ = 0U;
        size_ = 0U;
        dropped_bytes_ = 0U;
    }

   private:
    std::span<char> storage_{};
    size_t start_{};
    size_t size_{};
    size_t dropped_bytes_{};
};

}  // namespace micropixel::platform::esp_mosaico
