// Copyright 2026 MicroPixel contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstring>
#include <string_view>

namespace micropixel::nt26 {

class AtResponse {
   public:
    static constexpr size_t kCapacity = 4096;
    static constexpr size_t kMarkerCapacity = 64;
    enum class Status { kPending, kOk, kError, kOverflow };

    bool Begin(std::string_view marker = {}) {
        if (marker.size() >= marker_.size()) {
            return false;
        }
        size_ = 0;
        data_[0] = '\0';
        marker_size_ = marker.size();
        if (!marker.empty()) {
            std::memcpy(marker_.data(), marker.data(), marker.size());
        }
        status_ = Status::kPending;
        return true;
    }

    Status Append(std::string_view bytes) {
        if (status_ != Status::kPending) {
            return status_;
        }
        if (bytes.size() > data_.size() - 1 - size_) {
            status_ = Status::kOverflow;
            return status_;
        }
        if (!bytes.empty()) {
            std::memcpy(data_.data() + size_, bytes.data(), bytes.size());
        }
        size_ += bytes.size();
        data_[size_] = '\0';
        const auto response = text();
        if (HasLine(response, "ERROR") || HasLinePrefix(response, "+CME ERROR:") ||
            HasLinePrefix(response, "+CMS ERROR:")) {
            status_ = Status::kError;
        } else if (marker_size_ != 0) {
            if (response.find(std::string_view(marker_.data(), marker_size_)) != std::string_view::npos) {
                status_ = Status::kOk;
            }
        } else if (HasLine(response, "OK")) {
            status_ = Status::kOk;
        }
        return status_;
    }

    std::string_view text() const { return {data_.data(), size_}; }
    Status status() const { return status_; }

   private:
    static bool HasLinePrefix(std::string_view text, std::string_view prefix) {
        while (!text.empty()) {
            const auto end = text.find('\n');
            if (end == std::string_view::npos) {
                return false;
            }
            if (text.substr(0, end).starts_with(prefix)) {
                return true;
            }
            text.remove_prefix(end + 1);
        }
        return false;
    }

    static bool HasLine(std::string_view text, std::string_view expected) {
        while (!text.empty()) {
            const auto end = text.find('\n');
            if (end == std::string_view::npos) {
                return false;
            }
            auto line = text.substr(0, end);
            if (line.ends_with('\r')) {
                line.remove_suffix(1);
            }
            if (line == expected) {
                return true;
            }
            text.remove_prefix(end + 1);
        }
        return false;
    }

    std::array<char, kCapacity> data_{};
    std::array<char, kMarkerCapacity> marker_{};
    size_t size_{};
    size_t marker_size_{};
    Status status_{Status::kPending};
};

}  // namespace micropixel::nt26
