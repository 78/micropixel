#pragma once

#include <atomic>
#include <mutex>

#include "device/contracts/cellular.hpp"
#include "driver/i2c_master.h"
#include "uart_eth_modem.h"

namespace micropixel::work {
class BackgroundExecutor;
}
namespace micropixel::platform::buses {
class I2cExecutor;
}

namespace micropixel::platform::metalio_claw4 {

class CellularController final : public device::Cellular {
   public:
    CellularController();
    void Configure(i2c_master_dev_handle_t expander, buses::I2cExecutor& executor);
    void BindBackgroundExecutor(work::BackgroundExecutor& executor);
    [[nodiscard]] std::expected<void, device::CellularError> Initialize() override;
    [[nodiscard]] device::CellularSnapshot Snapshot() const override;
    void RequestSignalRefresh() override;
    void SetStateChangeSink(device::CellularStateChangeSink sink, void* context) override;
    [[nodiscard]] std::expected<void, device::CellularError> SetEnabled(bool enabled) override;
    [[nodiscard]] esp_err_t Pause();
    [[nodiscard]] esp_err_t Resume();
    void Shutdown();

   private:
    static void SwitchMode(void* context);
    static void ReadSignal(void* context);
    void OnModemEvent(UartEthModem::UartEthModemEvent event);
    void Publish(device::CellularState state);
    [[nodiscard]] esp_err_t SetPower(bool enabled);
    [[nodiscard]] esp_err_t StartModem();
    mutable std::mutex snapshot_mutex_;
    std::mutex operation_mutex_;
    device::CellularSnapshot snapshot_{};
    device::CellularStateChangeSink sink_{};
    void* sink_context_{};
    UartEthModem modem_;
    i2c_master_dev_handle_t expander_{};
    buses::I2cExecutor* i2c_{};
    work::BackgroundExecutor* background_{};
    std::atomic<bool> stopping_{};
    bool requested_mode_{};
    bool initialized_{};
    bool signal_pending_{};
    int64_t next_signal_refresh_us_{};
};

}  // namespace micropixel::platform::metalio_claw4
