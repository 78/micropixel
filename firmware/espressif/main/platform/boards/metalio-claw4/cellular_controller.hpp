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
    [[nodiscard]] bool TryBeginFirmwareUpdate() override;
    void EndFirmwareUpdate() override;
    void RequestSignalRefresh() override;
    void RequestSimRefresh() override;
    [[nodiscard]] std::expected<void, device::CellularError> SetSimSlot(device::CellularSimSlot slot) override;
    void SetStateChangeSink(device::CellularStateChangeSink sink, void* context) override;
    [[nodiscard]] std::expected<void, device::CellularError> SetEnabled(bool enabled) override;
    [[nodiscard]] esp_err_t Pause();
    [[nodiscard]] esp_err_t Resume();
    void Shutdown();

   private:
    static void SwitchMode(void* context);
    static void ReadSignal(void* context);
    static void ReadSim(void* context);
    static void SwitchSim(void* context);
    [[nodiscard]] device::CellularSimSlot QuerySimSlot();
    [[nodiscard]] device::CellularDiagnostics QueryDiagnostics();
    void FinishSim(bool failed, device::CellularSimSlot slot);
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
    std::atomic<bool> paused_{true};
    std::atomic<bool> sim_cancelled_{};
    device::CellularSimSlot requested_sim_{device::CellularSimSlot::kUnknown};
    bool firmware_update_active_{};  // Protected by snapshot_mutex_.
    bool requested_mode_{};
    bool initialized_{};
    bool signal_pending_{};
    int64_t next_signal_refresh_us_{};
};

}  // namespace micropixel::platform::metalio_claw4
