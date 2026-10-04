// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>

#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "platform/buses/i2c_executor.hpp"
#include "platform/drivers/rtc/rx8130.hpp"

namespace micropixel::platform::m5stack_tab5 {

// Keeps the Host system clock and the board's RX8130CE in step without a time
// contract: bring-up seeds the system clock from the RTC (so time is right
// before Wi-Fi and SNTP exist), and the system clock - owned by SNTP or the
// user afterwards - is written back whenever it differs. The RTC holds UTC, the
// same basis the system clock uses, and the backup capacitor keeps it running
// across a power-off.
class RtcClock final {
   public:
    RtcClock() = default;
    RtcClock(const RtcClock&) = delete;
    RtcClock& operator=(const RtcClock&) = delete;
    ~RtcClock();

    void Initialize(i2c_master_bus_handle_t bus, buses::I2cExecutor& executor);

   private:
    static void RefreshTimer(void* context);
    static esp_err_t RefreshEntry(void* context);
    void SyncOnWorker();
    void RestoreSystemClock();
    void PersistSystemClock();

    drivers::Rx8130 rtc_{};
    buses::I2cExecutor* executor_{};
    esp_timer_handle_t refresh_timer_{};
    std::atomic<bool> refresh_pending_{false};
    bool configured_{};
};

}  // namespace micropixel::platform::m5stack_tab5
