#include "platform/boards/metalio-claw4/cellular_controller.hpp"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "platform/boards/metalio-claw4/board_config.hpp"
#include "platform/buses/i2c_executor.hpp"
#include "work/background_executor.hpp"

namespace micropixel::platform::metalio_claw4 {
namespace {
constexpr char kTag[] = "claw4_cellular";
constexpr char kNamespace[] = "network";
constexpr char kModeKey[] = "type";
constexpr uint8_t kOutputPort0 = 0x02U;
constexpr uint8_t kReset4gMask = 1U << 7U;
}  // namespace

CellularController::CellularController()
    : modem_(UartEthModem::Config{.uart_num = board::kCellularUart,
                                  .baud_rate = board::kCellularBaud,
                                  .tx_pin = board::kCellularTx,
                                  .rx_pin = board::kCellularRx,
                                  .mrdy_pin = board::kCellularMrdy,
                                  .srdy_pin = board::kCellularSrdy,
                                  .rx_buffer_count = 4,
                                  .rx_buffer_size = 1600}) {
    modem_.SetNetworkEventCallback([this](auto event) { OnModemEvent(event); });
}

void CellularController::Configure(i2c_master_dev_handle_t expander, buses::I2cExecutor& executor) {
    expander_ = expander;
    i2c_ = &executor;
}

void CellularController::BindBackgroundExecutor(work::BackgroundExecutor& executor) { background_ = &executor; }

device::CellularSnapshot CellularController::Snapshot() const {
    std::lock_guard lock(snapshot_mutex_);
    return snapshot_;
}

void CellularController::SetStateChangeSink(device::CellularStateChangeSink sink, void* context) {
    std::lock_guard lock(snapshot_mutex_);
    sink_ = sink;
    sink_context_ = context;
}

void CellularController::Publish(device::CellularState state) {
    std::lock_guard lock(snapshot_mutex_);
    snapshot_.state = state;
    snapshot_.connected = state == device::CellularState::kConnected;
    if (!snapshot_.connected) {
        snapshot_.signal_bars = 0;
        next_signal_refresh_us_ = 0;
    }
    // Like Wifi, sinks only post Host wakeups and must not call this service.
    if (sink_ != nullptr) sink_(sink_context_);
}

std::expected<void, device::CellularError> CellularController::Initialize() {
    std::lock_guard operation(operation_mutex_);
    if (initialized_ || expander_ == nullptr || i2c_ == nullptr || background_ == nullptr) {
        return std::unexpected(device::CellularError::kUnavailable);
    }
    int32_t mode = 0;
    nvs_handle_t settings{};
    esp_err_t status = nvs_open_from_partition("runtime_nvs", kNamespace, NVS_READONLY, &settings);
    if (status == ESP_OK) {
        status = nvs_get_i32(settings, kModeKey, &mode);
        nvs_close(settings);
    }
    if (status != ESP_OK && status != ESP_ERR_NVS_NOT_FOUND) {
        return std::unexpected(device::CellularError::kStorage);
    }
    {
        std::lock_guard lock(snapshot_mutex_);
        snapshot_.available = true;
        snapshot_.enabled = mode == 1;
    }
    initialized_ = true;
    status = mode == 1 ? StartModem() : SetPower(false);
    if (status != ESP_OK) {
        Publish(device::CellularState::kFailed);
        return std::unexpected(device::CellularError::kOperationFailed);
    }
    return {};
}

esp_err_t CellularController::SetPower(bool enabled) {
    if (expander_ == nullptr || i2c_ == nullptr) return ESP_ERR_INVALID_STATE;
    struct Request final {
        i2c_master_dev_handle_t expander;
        bool enabled;
    } request{expander_, enabled};
    return i2c_->Invoke(
        buses::I2cExecutor::Priority::kHigh,
        [](void* context) {
            const auto& request = *static_cast<Request*>(context);
            uint8_t value{};
            esp_err_t status = i2c_master_transmit_receive(request.expander, &kOutputPort0, 1, &value, 1, 100);
            if (status != ESP_OK) return status;
            value = request.enabled ? static_cast<uint8_t>(value | kReset4gMask)
                                    : static_cast<uint8_t>(value & ~kReset4gMask);
            const uint8_t bytes[]{kOutputPort0, value};
            return i2c_master_transmit(request.expander, bytes, sizeof(bytes), 100);
        },
        &request);
}

esp_err_t CellularController::StartModem() {
    const esp_err_t power = SetPower(true);
    if (power != ESP_OK) return power;
    const esp_err_t pdp = modem_.SetPdpContext("eapn1.net", "IP");
    if (pdp != ESP_OK) return pdp;
    Publish(device::CellularState::kConnecting);
    return modem_.Start();
}

std::expected<void, device::CellularError> CellularController::SetEnabled(bool enabled) {
    std::lock_guard lock(snapshot_mutex_);
    if (!snapshot_.available || background_ == nullptr || stopping_) {
        return std::unexpected(device::CellularError::kUnavailable);
    }
    if (snapshot_.switching) return std::unexpected(device::CellularError::kBusy);
    if (enabled == snapshot_.enabled) return {};
    requested_mode_ = enabled;
    snapshot_.switching = true;
    snapshot_.switch_failed = false;
    if (!background_->Submit(SwitchMode, this)) {
        snapshot_.switching = false;
        return std::unexpected(device::CellularError::kBusy);
    }
    if (sink_ != nullptr) sink_(sink_context_);
    return {};
}

void CellularController::SwitchMode(void* context) {
    auto& self = *static_cast<CellularController*>(context);
    std::lock_guard operation(self.operation_mutex_);
    if (self.stopping_) return;
    bool mode;
    {
        std::lock_guard lock(self.snapshot_mutex_);
        mode = self.requested_mode_;
    }
    nvs_handle_t settings{};
    esp_err_t status = nvs_open_from_partition("runtime_nvs", kNamespace, NVS_READWRITE, &settings);
    if (status == ESP_OK) {
        status = nvs_set_i32(settings, kModeKey, mode ? 1 : 0);
        if (status == ESP_OK) status = nvs_commit(settings);
        nvs_close(settings);
    }
    if (status != ESP_OK) {
        {
            std::lock_guard lock(self.snapshot_mutex_);
            self.snapshot_.switching = false;
            self.snapshot_.switch_failed = true;
            if (self.sink_ != nullptr) self.sink_(self.sink_context_);
        }
        ESP_LOGE(kTag, "network mode was not saved: %s", esp_err_to_name(status));
        return;
    }
    // Factory behavior: show the selected mode for one second, then restart.
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (self.stopping_) return;
    status = self.modem_.Stop();
    if (status != ESP_OK) ESP_LOGW(kTag, "modem stop before restart: %s", esp_err_to_name(status));
    if (!self.stopping_) esp_restart();
}

void CellularController::OnModemEvent(UartEthModem::UartEthModemEvent event) {
    using Event = UartEthModem::UartEthModemEvent;
    using State = device::CellularState;
    switch (event) {
        case Event::Connected:
            Publish(State::kConnected);
            RequestSignalRefresh();
            break;
        case Event::Connecting:
            Publish(State::kConnecting);
            break;
        case Event::Disconnected:
            Publish(Snapshot().enabled ? State::kUnregistered : State::kOff);
            break;
        case Event::InFlightMode:
            Publish(State::kUnregistered);
            break;
        case Event::RequestingPdpContext:
            break;
        default:
            Publish(State::kFailed);
            break;
    }
}

void CellularController::RequestSignalRefresh() {
    std::lock_guard lock(snapshot_mutex_);
    const int64_t now = esp_timer_get_time();
    if (background_ == nullptr || stopping_ || signal_pending_ || !snapshot_.connected || snapshot_.switching ||
        now < next_signal_refresh_us_)
        return;
    signal_pending_ = true;
    if (!background_->Submit(ReadSignal, this)) signal_pending_ = false;
    next_signal_refresh_us_ = now + 5000000;
}

void CellularController::ReadSignal(void* context) {
    auto& self = *static_cast<CellularController*>(context);
    std::lock_guard operation(self.operation_mutex_);
    int strength = 99;
    if (!self.stopping_ && self.Snapshot().connected) strength = self.modem_.GetSignalStrength();
    std::lock_guard lock(self.snapshot_mutex_);
    self.signal_pending_ = false;
    // Match the factory's CSQ thresholds; 99/invalid remains unknown (no bars).
    self.snapshot_.signal_bars = !self.snapshot_.connected || strength < 0 || strength > 31 ? 0
                                 : strength < 10                                            ? 1
                                 : strength < 15                                            ? 2
                                 : strength < 20                                            ? 3
                                                                                            : 4;
    if (self.sink_ != nullptr) self.sink_(self.sink_context_);
}

esp_err_t CellularController::Pause() {
    std::lock_guard operation(operation_mutex_);
    const esp_err_t status = modem_.Stop();
    if (status != ESP_OK) return status;
    return SetPower(false);
}

esp_err_t CellularController::Resume() {
    std::lock_guard operation(operation_mutex_);
    if (!Snapshot().enabled || stopping_) return ESP_OK;
    return StartModem();
}

void CellularController::Shutdown() {
    stopping_ = true;
    const esp_err_t status = Pause();
    if (status != ESP_OK) ESP_LOGW(kTag, "modem shutdown: %s", esp_err_to_name(status));
}

}  // namespace micropixel::platform::metalio_claw4
