#include "platform/boards/metalio-claw4/cellular_controller.hpp"

#include <string_view>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "platform/boards/metalio-claw4/board_config.hpp"
#include "platform/boards/metalio-claw4/cellular_diagnostics.hpp"
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
                                  .rx_buffer_size = 1600,
                                  .tx_queue_depth = 32,
                                  .use_psram = true}) {
#if CONFIG_MICROPIXEL_CELLULAR_DEBUG
    modem_.SetDebug(true);
#endif
    modem_.SetNetworkEventCallback([this](auto event, const std::string& detail) {
        if (!detail.empty()) ESP_LOGI(kTag, "modem event %d: %s", static_cast<int>(event), detail.c_str());
        OnModemEvent(event);
    });
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

bool CellularController::TryHoldConfiguration() {
    std::lock_guard lock(snapshot_mutex_);
    if (stopping_ || configuration_held_ || recovery_needed_ || snapshot_.switching || sim_switching_ ||
        (snapshot_.enabled && paused_))
        return false;
    configuration_held_ = true;
    return true;
}

void CellularController::ReleaseConfiguration() {
    std::lock_guard lock(snapshot_mutex_);
    configuration_held_ = false;
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
    modem_.SetPdpContext("eapn1.net", "IP");
    Publish(device::CellularState::kConnecting);
    const esp_err_t status = modem_.Start();
    if (status == ESP_OK) {
        std::lock_guard lock(snapshot_mutex_);
        paused_ = recovery_needed_;
    } else {
        paused_ = true;
        if (modem_.Stop(100) == ESP_OK)
            (void)SetPower(false);
        else
            ScheduleRecovery(false);
    }
    return status;
}

std::expected<void, device::CellularError> CellularController::SetEnabled(bool enabled) {
    std::lock_guard lock(snapshot_mutex_);
    if (!snapshot_.available || background_ == nullptr || stopping_) {
        return std::unexpected(device::CellularError::kUnavailable);
    }
    if (configuration_held_ || recovery_needed_ || snapshot_.switching || (enabled && snapshot_.sim_pending))
        return std::unexpected(device::CellularError::kBusy);
    if (enabled == snapshot_.enabled) return {};
    requested_mode_ = enabled;
    modem_stop_requested_ = false;
    switch_requested_us_ = esp_timer_get_time();
    switch_cancelled_ = false;
    snapshot_.switching = true;
    snapshot_.switch_failed = false;
    if (!background_->Submit(SwitchMode, this)) {
        snapshot_.switching = false;
        return std::unexpected(device::CellularError::kBusy);
    }
    if (!enabled) {
        // A diagnostic read must never lock the user out of turning the radio off.
        // Stop(0) only publishes cancellation; it does not join workers. Wake a
        // blocked diagnostic/registration wait now, before our queued cleanup can
        // run. SIM switching still owns Start/Stop, so let that job finish first.
        sim_cancelled_ = true;
        sim_refresh_needed_ = false;
        if (!sim_switching_ && !paused_) {
            (void)modem_.Stop(0);
            modem_stop_requested_ = true;
        }
    }
    ESP_LOGI(kTag, "switch to %s queued", enabled ? "on" : "off");
    if (sink_ != nullptr) sink_(sink_context_);
    return {};
}

void CellularController::RequestSimRefresh() {
    std::lock_guard lock(snapshot_mutex_);
    sim_refresh_needed_ = true;
    SubmitSimRefreshLocked();
}

void CellularController::SubmitSimRefreshLocked() {
    if (!snapshot_.available || !snapshot_.enabled || background_ == nullptr || stopping_ || paused_ ||
        recovery_needed_ || snapshot_.switching || snapshot_.sim_pending || sim_read_pending_ || !modem_.IsAtReady())
        return;
    sim_cancelled_ = false;
    sim_read_pending_ = true;
    if (background_->Submit(ReadSim, this))
        sim_refresh_needed_ = false;
    else
        sim_read_pending_ = false;
}

std::expected<void, device::CellularError> CellularController::SetSimSlot(device::CellularSimSlot slot) {
    std::lock_guard lock(snapshot_mutex_);
    if (!snapshot_.available || !snapshot_.enabled || background_ == nullptr || stopping_ || paused_ ||
        (slot != device::CellularSimSlot::kExternal && slot != device::CellularSimSlot::kInternal)) {
        return std::unexpected(device::CellularError::kUnavailable);
    }
    if (configuration_held_ || recovery_needed_ || snapshot_.switching || snapshot_.sim_pending)
        return std::unexpected(device::CellularError::kBusy);
    if (slot == snapshot_.sim_slot) return {};
    requested_sim_ = slot;
    sim_cancelled_ = false;
    snapshot_.sim_pending = true;
    sim_switching_ = true;
    snapshot_.sim_failed = false;
    if (!background_->Submit(SwitchSim, this)) {
        snapshot_.sim_pending = false;
        sim_switching_ = false;
        return std::unexpected(device::CellularError::kBusy);
    }
    if (sink_ != nullptr) sink_(sink_context_);
    return {};
}

device::CellularSimSlot CellularController::QuerySimSlot() {
    using Slot = device::CellularSimSlot;
    std::string response;
    // Do not require registration or IsInitialized: a missing SIM must not
    // prevent querying or selecting the other slot, as on the factory firmware.
    if (modem_.SendAt("AT+ECSIMCFG?", response, 5000) != ESP_OK) return Slot::kUnknown;
    std::string_view remaining(response);
    while (!remaining.empty()) {
        const size_t end = remaining.find_first_of("\r\n");
        auto line = remaining.substr(0, end);
        if (line.starts_with("+ECSIMCFG:")) {
            line.remove_prefix(std::string_view("+ECSIMCFG:").size());
            const auto first = line.find_first_not_of(" \t");
            if (first != std::string_view::npos) line.remove_prefix(first);
            if (line.starts_with("\"SimSlot\"")) {
                line.remove_prefix(std::string_view("\"SimSlot\"").size());
                const auto comma = line.find_first_not_of(" \t");
                if (comma == std::string_view::npos || line[comma] != ',') return Slot::kUnknown;
                line.remove_prefix(comma + 1);
                const auto value = line.find_first_not_of(" \t");
                if (value == std::string_view::npos) return Slot::kUnknown;
                line.remove_prefix(value);
                if (line.find_first_not_of(" \t", 1) != std::string_view::npos) return Slot::kUnknown;
                if (line.front() == '0') return Slot::kExternal;
                if (line.front() == '1') return Slot::kInternal;
                return Slot::kUnknown;
            }
        }
        if (end == std::string_view::npos) break;
        remaining.remove_prefix(end + 1);
    }
    return Slot::kUnknown;
}

void CellularController::FinishSim(bool failed, device::CellularSimSlot slot) {
    std::lock_guard lock(snapshot_mutex_);
    if (!sim_cancelled_) {
        snapshot_.sim_slot = slot;
        snapshot_.sim_failed = failed;
    }
    snapshot_.sim_pending = false;
    sim_switching_ = false;
    if (sink_ != nullptr) sink_(sink_context_);
}

device::CellularDiagnostics CellularController::QueryDiagnostics() {
    device::CellularDiagnostics result{};
    std::string response;
    const auto query = [&](const char* command) {
        response.clear();
        if (stopping_ || paused_ || sim_cancelled_) {
            result.incomplete = true;
            return false;
        }
        if (modem_.SendAt(command, response, 1000) != ESP_OK) {
            result.incomplete = true;
            return false;
        }
        return true;
    };
    (void)query("AT+CPIN?");
    result.sim_status = diagnostics::SimStatus(response);
    if (query("AT+CFUN?")) result.radio_function = diagnostics::Number(diagnostics::Line(response, "+CFUN:"), 127);
    if (query("AT+CSQ")) {
        result.signal_csq = diagnostics::Number(diagnostics::Field(diagnostics::Line(response, "+CSQ:"), 0), 99);
        if (result.signal_csq > 31 && result.signal_csq != 99) result.signal_csq = -1;
    }
    if (query("AT+CEREG?"))
        result.registration = diagnostics::Number(diagnostics::Field(diagnostics::Line(response, "+CEREG:"), 1), 10);
    if (query("AT+CGATT?")) result.attached = diagnostics::Number(diagnostics::Line(response, "+CGATT:"), 1);
    if (query("AT+COPS?")) {
        const auto line = diagnostics::Line(response, "+COPS:");
        const auto name = diagnostics::Field(line, 2);
        if (!name.empty() && !diagnostics::Text(name, result.operator_name)) result.incomplete = true;
    }
    if (query("AT+CGDCONT?")) {
        const auto line = diagnostics::Line(response, "+CGDCONT: 1,");
        if (!diagnostics::Text(diagnostics::Field(line, 1), result.apn)) result.incomplete = true;
    }
    if (query("AT+CGPADDR=1")) {
        auto address = diagnostics::Field(diagnostics::Line(response, "+CGPADDR: 1,"), 0);
        if (!address.empty()) {
            if (address.front() == '"') {
                if (!diagnostics::Text(address, result.pdp_address)) result.incomplete = true;
            } else if (address.size() < result.pdp_address.size() &&
                       address.find_first_not_of("0123456789abcdefABCDEF:.") == std::string_view::npos) {
                std::copy(address.begin(), address.end(), result.pdp_address.begin());
            } else
                result.incomplete = true;
        }
    }
    result.sampled = true;
    result.incomplete = result.incomplete || result.sim_status == device::CellularSimStatus::kUnknown ||
                        result.signal_csq < 0 || result.registration < 0 || result.radio_function < 0 ||
                        result.attached < 0;
    return result;
}

void CellularController::ReadSim(void* context) {
    auto& self = *static_cast<CellularController*>(context);
    std::lock_guard operation(self.operation_mutex_);
    device::CellularSimSlot slot = device::CellularSimSlot::kUnknown;
    device::CellularDiagnostics details{};
    if (!self.stopping_ && !self.paused_ && !self.sim_cancelled_) {
        slot = self.QuerySimSlot();
        details = self.QueryDiagnostics();
    }
    std::lock_guard lock(self.snapshot_mutex_);
    self.sim_read_pending_ = false;
    // A SIM change can queue behind this read. Completing the read must not
    // clear that command's pending state or overwrite its eventual result.
    if (!self.sim_cancelled_ && !self.snapshot_.sim_pending) {
        self.snapshot_.diagnostics = details;
        self.snapshot_.sim_slot = slot;
        self.snapshot_.sim_failed = slot == device::CellularSimSlot::kUnknown;
    }
    if (self.sink_ != nullptr) self.sink_(self.sink_context_);
}

void CellularController::SwitchSim(void* context) {
    auto& self = *static_cast<CellularController*>(context);
    std::lock_guard operation(self.operation_mutex_);
    if (self.stopping_ || self.paused_ || self.sim_cancelled_) {
        self.FinishSim(true, device::CellularSimSlot::kUnknown);
        return;
    }
    device::CellularSimSlot target;
    {
        std::lock_guard lock(self.snapshot_mutex_);
        target = self.requested_sim_;
    }
    // Quiesce the driver's registration/PDP controller before taking over RF.
    // AT remains available, including when initialization found no SIM.
    if (self.modem_.PrepareForShutdown() != ESP_OK) {
        self.ScheduleRecovery(true);
        self.FinishSim(true, device::CellularSimSlot::kUnknown);
        return;
    }
    std::string response;
    bool failed = self.modem_.SendAt("AT+CFUN=0", response, 8000) != ESP_OK;
    if (!failed) {
        vTaskDelay(pdMS_TO_TICKS(500));
        failed = self.modem_.SendAt(
                     target == device::CellularSimSlot::kExternal ? "AT+ECSIMCFG=SimSlot,0" : "AT+ECSIMCFG=SimSlot,1",
                     response, 5000) != ESP_OK;
        if (!failed) vTaskDelay(pdMS_TO_TICKS(500));
        // Factory policy: restore RF even when changing the slot failed. A
        // successful slot command persists in the modem; CFUN=1 may time out.
        (void)self.modem_.SendAt("AT+CFUN=1", response, failed ? 10000 : 15000);
    }
    // Read actual state after success and failure, never guess from a UI preference.
    const auto slot = self.QuerySimSlot();
    failed = failed || slot != target;
    // Reinitialize only the modem. The Host, Wi-Fi and app session stay alive.
    // A failed SIM write also needs a fresh controller after PrepareForShutdown.
    self.paused_ = true;
    esp_err_t status = self.modem_.Stop();
    if (status == ESP_OK) status = self.SetPower(false);
    if (status == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!self.stopping_) status = self.StartModem();
    } else {
        self.ScheduleRecovery(true);
    }
    {
        std::lock_guard lock(self.snapshot_mutex_);
        self.snapshot_.diagnostics = {};
    }
    ESP_LOGI(kTag, "SIM switch: slot=%u, write=%s, modem restart=%s", static_cast<unsigned>(slot),
             failed ? "failed" : "ok", esp_err_to_name(status));
    self.FinishSim(failed || status != ESP_OK, slot);
}

void CellularController::SwitchMode(void* context) {
    auto& self = *static_cast<CellularController*>(context);
    std::lock_guard operation(self.operation_mutex_);
    bool enabled;
    bool previous;
    bool stop_requested;
    const int64_t started_us = esp_timer_get_time();
    {
        std::lock_guard lock(self.snapshot_mutex_);
        if (self.stopping_ || self.switch_cancelled_) {
            self.snapshot_.switching = false;
            self.snapshot_.switch_failed = true;
            if (self.sink_ != nullptr) self.sink_(self.sink_context_);
            return;
        }
        enabled = self.requested_mode_;
        previous = self.snapshot_.enabled;
        stop_requested = self.modem_stop_requested_;
        ESP_LOGI(kTag, "switch to %s started after %lld ms", enabled ? "on" : "off",
                 (long long)((started_us - self.switch_requested_us_) / 1000));
    }
    // Keep the old key for upgrades, but it now controls only the cellular radio.
    const auto save = [](bool value) {
        nvs_handle_t settings{};
        esp_err_t status = nvs_open_from_partition("runtime_nvs", kNamespace, NVS_READWRITE, &settings);
        if (status == ESP_OK) {
            status = nvs_set_i32(settings, kModeKey, value ? 1 : 0);
            if (status == ESP_OK) status = nvs_commit(settings);
            nvs_close(settings);
        }
        return status;
    };
    esp_err_t status = save(enabled);
    if (status != ESP_OK && stop_requested) self.ScheduleRecovery(previous);
    if (status == ESP_OK) {
        // Publish the desired state before Start/Stop can emit asynchronous events.
        {
            std::lock_guard lock(self.snapshot_mutex_);
            self.snapshot_.enabled = enabled;
        }
        if (enabled) {
            status = self.StartModem();
        } else {
            self.sim_cancelled_ = true;
            status = self.modem_.Stop();
            if (status == ESP_OK) {
                self.paused_ = true;
                status = self.SetPower(false);
            }
        }
        if (status != ESP_OK) {
            // Restore the previous persisted switch; a failed stop never cuts power.
            const esp_err_t saved = save(previous);
            if (saved != ESP_OK) ESP_LOGE(kTag, "cellular setting rollback failed: %s", esp_err_to_name(saved));
            if (enabled) {
                if (self.modem_.Stop() == ESP_OK) {
                    self.paused_ = true;
                    (void)self.SetPower(false);
                } else
                    self.ScheduleRecovery(previous);
            } else if (self.paused_) {
                (void)self.StartModem();
            } else {
                self.ScheduleRecovery(previous);
            }
        }
    }
    {
        std::lock_guard lock(self.snapshot_mutex_);
        self.snapshot_.enabled = status == ESP_OK ? enabled : previous;
        self.snapshot_.switching = false;
        self.snapshot_.switch_failed = status != ESP_OK;
        if (status == ESP_OK && !enabled) {
            self.snapshot_.connected = false;
            self.snapshot_.state = device::CellularState::kOff;
            self.snapshot_.signal_bars = 0;
            self.snapshot_.diagnostics = {};
            self.snapshot_.sim_slot = device::CellularSimSlot::kUnknown;
            self.snapshot_.sim_failed = false;
        } else if (status != ESP_OK) {
            self.snapshot_.state = device::CellularState::kFailed;
            self.snapshot_.connected = false;
            self.snapshot_.signal_bars = 0;
        }
        if (self.sink_ != nullptr) self.sink_(self.sink_context_);
    }
    ESP_LOGI(kTag, "switch to %s finished in %lld ms: %s", enabled ? "on" : "off",
             (long long)((esp_timer_get_time() - started_us) / 1000), esp_err_to_name(status));
    if (status != ESP_OK) ESP_LOGW(kTag, "cellular switch failed: %s", esp_err_to_name(status));
}

void CellularController::OnModemEvent(UartEthModem::UartEthModemEvent event) {
    using Event = UartEthModem::UartEthModemEvent;
    using State = device::CellularState;
    switch (event) {
        case Event::Connected:
            Publish(State::kConnected);
            Poll();
            RequestSimRefresh();
            break;
        case Event::Connecting:
            Publish(State::kConnecting);
            break;
        case Event::Disconnected:
            Publish(Snapshot().enabled ? State::kUnregistered : State::kOff);
            break;
        case Event::ErrorNoSim:
        case Event::RegistrationLost:
        case Event::InFlightMode:
            Publish(State::kUnregistered);
            RequestSimRefresh();
            break;
        case Event::RequestingPdpContext:
            RequestSimRefresh();
            break;
        case Event::PlmnSearchFallback:
            break;
        case Event::ModemReset:
            ScheduleRecovery(true);
            break;
        case Event::ErrorInitFailed:
            ScheduleRecovery(false);
            break;
        default:
            Publish(State::kFailed);
            RequestSimRefresh();
            break;
    }
}

void CellularController::ScheduleRecovery(bool restart) {
    std::lock_guard lock(snapshot_mutex_);
    if (stopping_) return;
    paused_ = true;
    recovery_needed_ = true;
    recovery_restart_ = recovery_restart_ || restart;
    snapshot_.connected = false;
    snapshot_.state = device::CellularState::kFailed;
    snapshot_.signal_bars = 0;
    if (!recovery_pending_ && background_ != nullptr) recovery_pending_ = background_->Submit(Recover, this);
    if (sink_ != nullptr) sink_(sink_context_);
}

void CellularController::Recover(void* context) {
    auto& self = *static_cast<CellularController*>(context);
    std::lock_guard operation(self.operation_mutex_);
    {
        std::lock_guard lock(self.snapshot_mutex_);
        self.recovery_pending_ = false;
        if (!self.recovery_needed_ || self.stopping_) return;
    }
    // A timed-out Stop still owns live workers. Keep the object and radio powered;
    // retry at the queue tail so other background work can still run. Host refresh
    // also retries submission if the bounded queue was full.
    if (self.modem_.Stop(100) != ESP_OK || self.SetPower(false) != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));
        self.ScheduleRecovery(false);
        return;
    }
    bool restart;
    {
        std::lock_guard lock(self.snapshot_mutex_);
        restart = self.recovery_restart_ && self.snapshot_.enabled;
        self.recovery_needed_ = false;
        self.recovery_restart_ = false;
    }
    if (restart) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (self.StartModem() != ESP_OK) self.Publish(device::CellularState::kFailed);
    }
}

void CellularController::Poll() {
    std::lock_guard lock(snapshot_mutex_);
    if (recovery_needed_ && !recovery_pending_ && !stopping_ && background_ != nullptr)
        recovery_pending_ = background_->Submit(Recover, this);
    if (sim_refresh_needed_) SubmitSimRefreshLocked();
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
    if (!self.stopping_ && !self.sim_cancelled_ && self.Snapshot().connected)
        strength = self.modem_.GetSignalStrength();
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
    {
        std::lock_guard lock(snapshot_mutex_);
        if (configuration_held_ && !stopping_) return ESP_ERR_INVALID_STATE;
        // Block new SIM requests and invalidate the single pending job atomically.
        // Resume does not clear cancellation; only a new accepted request does.
        paused_ = true;
        sim_cancelled_ = true;
        switch_cancelled_ = true;
        snapshot_.diagnostics = {};
        sim_refresh_needed_ = false;
    }
    const esp_err_t status = modem_.Stop();
    if (status != ESP_OK) {
        ScheduleRecovery(Snapshot().enabled);
        return status;
    }
    {
        std::lock_guard lock(snapshot_mutex_);
        recovery_needed_ = false;
        recovery_restart_ = false;
    }
    const esp_err_t power = SetPower(false);
    if (power != ESP_OK && !stopping_ && Snapshot().enabled) {
        // Sleep is rejected by the caller, so restore service while the Host stays awake.
        const esp_err_t rollback = StartModem();
        if (rollback != ESP_OK) {
            Publish(device::CellularState::kFailed);
            ESP_LOGE(kTag, "cellular sleep rollback failed: %s", esp_err_to_name(rollback));
        }
    }
    return power;
}

esp_err_t CellularController::Resume() {
    std::lock_guard operation(operation_mutex_);
    {
        std::lock_guard lock(snapshot_mutex_);
        if (!snapshot_.enabled || stopping_) return ESP_OK;
        if (recovery_needed_) return ESP_ERR_INVALID_STATE;
    }
    return StartModem();
}

void CellularController::Shutdown() {
    stopping_ = true;
    const esp_err_t status = Pause();
    if (status != ESP_OK) ESP_LOGW(kTag, "modem shutdown: %s", esp_err_to_name(status));
}

}  // namespace micropixel::platform::metalio_claw4
