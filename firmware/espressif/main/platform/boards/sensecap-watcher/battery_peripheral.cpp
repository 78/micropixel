// SPDX-FileCopyrightText: 2026 wamr-host contributors
// SPDX-License-Identifier: Apache-2.0

#include "platform/boards/sensecap-watcher/battery_peripheral.hpp"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "platform/boards/sensecap-watcher/board_config.hpp"
#include "platform/boards/sensecap-watcher/board_power.hpp"
#include "platform/buses/i2c_executor.hpp"

namespace micropixel::platform::sensecap_watcher {
namespace {

constexpr char kTag[] = "watcher_battery";
// The divider brings a full pack to roughly 1.0 V, which the smallest
// attenuation covers with the most resolution; the vendor configures the same.
constexpr adc_atten_t kAdcAttenuation = ADC_ATTEN_DB_2_5;
constexpr adc_unit_t kAdcUnit = ADC_UNIT_1;
constexpr adc_bitwidth_t kAdcBitWidth = ADC_BITWIDTH_DEFAULT;
// Averaging takes the jitter off a single conversion, as the vendor firmware
// does for the same divider.
constexpr uint32_t kSampleCount = 10U;
// The pack moves slowly, so a two-second period keeps the control bus quiet
// while the Host's own one-second poll still reads fresh data.
constexpr uint64_t kRefreshIntervalUs = 2U * 1000U * 1000U;
// A pack under load can dip below the vendor curve's 3.4 V zero point as it
// empties, so the floor only rejects readings no cell could produce: an empty
// divider sits near zero, while the ceiling rejects a divider or ADC fault.
constexpr uint32_t kMinimumPackMillivolts = 2000U;
constexpr uint32_t kMaximumPackMillivolts = 4400U;

// State of charge from the pack voltage, in millivolts. This is the quadratic
// the vendor fits for the on-board 400 mAh cell: it returns 0 % at about 3.4 V,
// 100 % at 4.2 V, and is monotonic across that range. All arithmetic is integer,
// and the intermediate products need 64 bits because the squared term reaches
// about 1.9e7 before the coefficients are applied.
[[nodiscard]] uint32_t PercentFromPackMillivolts(uint32_t millivolts) {
    const int64_t voltage = static_cast<int64_t>(millivolts);
    const int64_t percent = ((-voltage * voltage) + (9016 * voltage) - 19189000) / 10000;
    if (percent <= 0) {
        return 0U;
    }
    return percent >= 100 ? 100U : static_cast<uint32_t>(percent);
}

}  // namespace

BatteryPeripheral::~BatteryPeripheral() {
    if (refresh_timer_ != nullptr) {
        (void)esp_timer_stop_blocking(refresh_timer_, portMAX_DELAY);
        (void)esp_timer_delete(refresh_timer_);
    }
    if (calibration_ != nullptr) {
        (void)adc_cali_delete_scheme_curve_fitting(calibration_);
    }
    if (unit_ != nullptr) {
        (void)adc_oneshot_del_unit(unit_);
    }
}

esp_err_t BatteryPeripheral::Initialize(BoardPower& power, buses::I2cExecutor& executor) {
    power_ = &power;
    executor_ = &executor;

    adc_oneshot_unit_init_cfg_t unit_config{};
    unit_config.unit_id = kAdcUnit;
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_config, &unit_), kTag, "create ADC unit failed");

    const auto channel = static_cast<adc_channel_t>(board::kBatteryAdcChannel);
    adc_oneshot_chan_cfg_t channel_config{};
    channel_config.atten = kAdcAttenuation;
    channel_config.bitwidth = kAdcBitWidth;
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(unit_, channel, &channel_config), kTag,
                        "configure the battery sense channel failed");

    // The curve-fitting scheme is what turns a raw conversion into millivolts;
    // without it the reading cannot be scaled to a pack voltage at all.
    adc_cali_curve_fitting_config_t calibration_config{};
    calibration_config.unit_id = kAdcUnit;
    calibration_config.chan = channel;
    calibration_config.atten = kAdcAttenuation;
    calibration_config.bitwidth = kAdcBitWidth;
    const esp_err_t calibration_status = adc_cali_create_scheme_curve_fitting(&calibration_config, &calibration_);
    if (calibration_status != ESP_OK) {
        ESP_LOGW(kTag, "curve-fitting calibration unavailable: %s", esp_err_to_name(calibration_status));
        return calibration_status;
    }

    const esp_err_t initial_status = executor.Invoke(buses::I2cExecutor::Priority::kLow, RefreshEntry, this);
    if (initial_status != ESP_OK) {
        ESP_LOGW(kTag, "initial sample failed: %s", esp_err_to_name(initial_status));
    }

    esp_timer_create_args_t arguments{};
    arguments.callback = RefreshTimer;
    arguments.arg = this;
    arguments.dispatch_method = ESP_TIMER_TASK;
    arguments.name = "watcher_battery";
    arguments.skip_unhandled_events = true;
    if (esp_timer_create(&arguments, &refresh_timer_) != ESP_OK ||
        esp_timer_start_periodic(refresh_timer_, kRefreshIntervalUs) != ESP_OK) {
        ESP_LOGW(kTag, "periodic refresh unavailable; the level only follows Host polls");
    }
    return ESP_OK;
}

device::BatterySnapshot BatteryPeripheral::Snapshot() {
    if (executor_ == nullptr) {
        return {};
    }
    struct Request final {
        BatteryPeripheral* peripheral;
        device::BatterySnapshot snapshot;
    } request{this, last_snapshot_};
    const esp_err_t status = executor_->Invoke(
        buses::I2cExecutor::Priority::kLow,
        [](void* context) {
            auto& requested = *static_cast<Request*>(context);
            requested.snapshot = requested.peripheral->Sample();
            return ESP_OK;
        },
        &request);
    return status == ESP_OK ? request.snapshot : last_snapshot_;
}

device::BatterySnapshot BatteryPeripheral::Sample() {
    const device::BatterySnapshot previous = last_snapshot_;
    if (power_ == nullptr) {
        return last_snapshot_;
    }
    // VBUS_IN_DET is active low, so external power is present when the line
    // reads low. This is the line the vendor UI and xiaozhi both use for the
    // charge state; the vendor's separate charge-detect pin feeds
    // bsp_system_is_charging(), whose polarity contradicts its own UI.
    const bool external_power = power_->IsCharging();
    // BAT_DET is active low as well, but neither the vendor UI nor xiaozhi gates
    // the reported level on it: one posts bsp_battery_get_percent() on a timer,
    // the other returns the curve unconditionally. It is sampled for the log
    // only, so a unit whose detect line never asserts still reports a level.
    const bool battery_present = power_->IsBatteryPresent();
    // The charge state does not depend on the pack reading, so it is refreshed on
    // every pass: a device running from the charger keeps reporting external
    // power even while the divider reads nothing.
    last_snapshot_.charging = external_power;
    last_snapshot_.discharging = !external_power;
    last_snapshot_.charging_available = true;
    last_snapshot_.external_power_connected = external_power;
    last_snapshot_.external_power_available = true;
    uint32_t millivolts = 0U;
    const bool level_read = ReadPackMillivolts(millivolts) == ESP_OK;
    if (level_read) {
        // A reading inside the pack's range is what makes the level available,
        // and it is the only thing that is. A rejected reading leaves the
        // previous level and its availability alone rather than blanking a level
        // that a momentary dip should not erase.
        last_snapshot_.percent = static_cast<uint8_t>(PercentFromPackMillivolts(millivolts));
        last_snapshot_.available = true;
    }
    if (!sample_logged_ || previous.percent != last_snapshot_.percent ||
        previous.available != last_snapshot_.available || previous.charging != last_snapshot_.charging ||
        previous.discharging != last_snapshot_.discharging ||
        previous.external_power_connected != last_snapshot_.external_power_connected) {
        ESP_LOGI(kTag, "sample: pack=%umV level=%u%% available=%s present=%s external=%s charging=%s%s", millivolts,
                 static_cast<unsigned>(last_snapshot_.percent), last_snapshot_.available ? "yes" : "no",
                 battery_present ? "yes" : "no", external_power ? "yes" : "no", last_snapshot_.charging ? "yes" : "no",
                 level_read ? "" : " (pack rejected, previous level kept)");
        sample_logged_ = true;
    }
    NotifyIfChanged(previous, last_snapshot_);
    return last_snapshot_;
}

esp_err_t BatteryPeripheral::ReadPackMillivolts(uint32_t& millivolts) {
    if (unit_ == nullptr || calibration_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const auto channel = static_cast<adc_channel_t>(board::kBatteryAdcChannel);
    uint32_t total_millivolts = 0U;
    for (uint32_t sample = 0U; sample < kSampleCount; ++sample) {
        int raw = 0;
        int sense_millivolts = 0;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(unit_, channel, &raw), kTag, "read the battery sense channel failed");
        ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(calibration_, raw, &sense_millivolts), kTag,
                            "convert the battery sense reading failed");
        if (sense_millivolts <= 0) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        total_millivolts += static_cast<uint32_t>(sense_millivolts);
    }
    const uint32_t sense_average = total_millivolts / kSampleCount;
    // Undo the divider the vendor fits to this board.
    const auto pack = static_cast<uint32_t>(static_cast<float>(sense_average) * board::kBatteryVoltageRatio);
    if (pack < kMinimumPackMillivolts || pack > kMaximumPackMillivolts) {
        // A divider that keeps reading outside the pack's range would otherwise
        // repeat this warning on every refresh, so it is reported once per
        // episode and the voltage that caused it is kept for the log.
        if (!range_warned_) {
            ESP_LOGW(kTag, "pack voltage out of range: %umV", pack);
            range_warned_ = true;
        }
        return ESP_ERR_INVALID_RESPONSE;
    }
    range_warned_ = false;
    millivolts = pack;
    return ESP_OK;
}

void BatteryPeripheral::RefreshTimer(void* context) {
    auto* battery = static_cast<BatteryPeripheral*>(context);
    if (battery == nullptr || battery->executor_ == nullptr ||
        battery->refresh_pending_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (!battery->executor_->Post(buses::I2cExecutor::Priority::kLow, RefreshEntry, battery)) {
        battery->refresh_pending_.store(false, std::memory_order_release);
    }
}

esp_err_t BatteryPeripheral::RefreshEntry(void* context) {
    auto* battery = static_cast<BatteryPeripheral*>(context);
    if (battery == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    battery->refresh_pending_.store(false, std::memory_order_release);
    (void)battery->Sample();
    return ESP_OK;
}

void BatteryPeripheral::NotifyIfChanged(const device::BatterySnapshot& previous,
                                        const device::BatterySnapshot& current) {
    if (previous.percent == current.percent && previous.available == current.available &&
        previous.charging == current.charging && previous.discharging == current.discharging &&
        previous.external_power_connected == current.external_power_connected) {
        return;
    }
    device::BatteryStateChangeSink sink = state_change_sink_.load(std::memory_order_acquire);
    if (sink != nullptr) {
        sink(state_change_context_.load(std::memory_order_acquire));
    }
}

void BatteryPeripheral::SetStateChangeSink(device::BatteryStateChangeSink sink, void* context) {
    if (sink == nullptr) {
        state_change_sink_.store(nullptr, std::memory_order_release);
        state_change_context_.store(nullptr, std::memory_order_release);
        return;
    }
    state_change_context_.store(context, std::memory_order_release);
    state_change_sink_.store(sink, std::memory_order_release);
}

}  // namespace micropixel::platform::sensecap_watcher
