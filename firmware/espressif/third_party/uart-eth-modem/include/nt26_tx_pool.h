// Copyright 2026 MicroPixel contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace micropixel::nt26 {

// The caller serializes access. A timeout releases only the waiter's ownership:
// the queued worker keeps both the payload and completion storage alive.
class TxPool {
   public:
    static constexpr size_t kCapacity = 32;
    static constexpr size_t kFrameSize = 1600;

    struct Slot {
        std::array<uint8_t, kFrameSize> data{};
        size_t length{};
        int32_t result{};
        bool worker{};
        bool waiter{};
        bool completed{};
    };

    Slot* Acquire(size_t length, bool waiter) {
        if (length == 0 || length > kFrameSize) {
            return nullptr;
        }
        for (auto& slot : slots_) {
            if (!slot.worker && !slot.waiter) {
                slot.length = length;
                slot.result = 0;
                slot.worker = true;
                slot.waiter = waiter;
                slot.completed = false;
                return &slot;
            }
        }
        return nullptr;
    }

    void Complete(Slot& slot, int32_t result) {
        slot.result = result;
        slot.completed = true;
        slot.worker = false;
    }

    void ReleaseWaiter(Slot& slot) { slot.waiter = false; }

    size_t Index(const Slot& slot) const { return static_cast<size_t>(&slot - slots_.data()); }

   private:
    std::array<Slot, kCapacity> slots_{};
};

}  // namespace micropixel::nt26
