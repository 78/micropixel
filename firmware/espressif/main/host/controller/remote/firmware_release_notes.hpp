// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstring>
#include <span>
#include <string_view>

namespace micropixel::firmware::remote_control {

// Writes plain text into caller-owned storage; truncation preserves UTF-8 boundaries.
class FirmwareReleaseNotesBuilder final {
   public:
    explicit FirmwareReleaseNotesBuilder(std::span<char> destination) : destination_(destination) {
        if (!destination_.empty()) destination_[0] = '\0';
    }

    void Append(std::string_view note) {
        if (note.empty() || truncated_ || destination_.empty()) return;
        const size_t separator = size_ == 0U ? 0U : 1U;
        const size_t remaining = destination_.size() - 1U - size_;
        if (separator + note.size() <= remaining) {
            if (separator != 0U) destination_[size_++] = '\n';
            std::memcpy(destination_.data() + size_, note.data(), note.size());
            size_ += note.size();
        } else {
            // Reserve the ellipsis before copying; never leave a partial multibyte character.
            size_t count = remaining > separator + 3U ? remaining - separator - 3U : 0U;
            while (count > 0U && (static_cast<unsigned char>(note[count]) & 0xc0U) == 0x80U) --count;
            if (count != 0U) {
                if (separator != 0U) destination_[size_++] = '\n';
                std::memcpy(destination_.data() + size_, note.data(), count);
                size_ += count;
            }
            // A prior entry may have filled the buffer: back up to a UTF-8 boundary.
            const size_t dots = std::min<size_t>(3U, destination_.size() - 1U);
            if (size_ + dots >= destination_.size()) {
                size_ = destination_.size() - 1U - dots;
                while (size_ > 0U && (static_cast<unsigned char>(destination_[size_]) & 0xc0U) == 0x80U) --size_;
            }
            for (size_t i = 0U; i < dots; ++i) destination_[size_++] = '.';
            truncated_ = true;
        }
        destination_[size_] = '\0';
    }

   private:
    std::span<char> destination_;
    size_t size_{};
    bool truncated_{};
};

}  // namespace micropixel::firmware::remote_control
