// Copyright 2026 MicroPixel contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <span>
#include <string_view>

#include "nt26_at_response.h"
#include "nt26_frame.h"
#include "nt26_tx_pool.h"

namespace micropixel::nt26 {

// Allocate once in PSRAM before starting workers; never accessed from an ISR.
struct TaskWorkspace {
    TxPool tx_pool;
    AtResponse at_response;
    std::array<uint8_t, TxPool::kFrameSize> command{};

    // The caller holds the AT lock until SendFrame has copied this into the TX pool.
    std::span<const uint8_t> PrepareCommand(std::string_view text) {
        if (text.empty() || text.size() > command.size() - sizeof(FrameHeader) - 1) return {};
        std::copy(text.begin(), text.end(), command.begin());
        size_t length = text.size();
        if (command[length - 1] != '\r') command[length++] = '\r';
        return {command.data(), length};
    }
};

}  // namespace micropixel::nt26
