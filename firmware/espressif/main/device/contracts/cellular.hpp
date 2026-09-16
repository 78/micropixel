#pragma once

#include <array>
#include <cstdint>
#include <expected>

namespace micropixel::device {

enum class CellularState : uint8_t { kOff, kConnecting, kConnected, kUnregistered, kFailed };
enum class CellularSimSlot : uint8_t { kExternal, kInternal, kUnknown };

enum class CellularError : uint8_t { kUnavailable, kBusy, kStorage, kOperationFailed };

enum class CellularSimStatus : uint8_t { kUnknown, kReady, kPinRequired, kPukRequired, kNotReady, kAbsent };

struct CellularDiagnostics final {
    CellularSimStatus sim_status{CellularSimStatus::kUnknown};
    int8_t signal_csq{-1};  // 0..31 or 99 (unknown); -1 means query unavailable.
    int8_t registration{-1};
    int8_t radio_function{-1};
    int8_t attached{-1};
    std::array<char, 48> operator_name{};
    std::array<char, 64> apn{};
    std::array<char, 48> pdp_address{};
    bool sampled{};
    bool incomplete{};
};

struct CellularSnapshot final {
    CellularDiagnostics diagnostics{};
    uint8_t signal_bars{};
    bool available{};
    bool enabled{};
    bool connected{};
    bool switching{};
    bool switch_failed{};
    CellularSimSlot sim_slot{CellularSimSlot::kUnknown};
    bool sim_pending{};
    bool sim_failed{};
    uint8_t sim_restart_seconds{};
    CellularState state{CellularState::kOff};
};

using CellularStateChangeSink = void (*)(void* context);

// Host-only cellular control. SetEnabled asynchronously persists and applies the
// cellular switch independently of Wi-Fi, without restarting the device.
class Cellular {
   public:
    virtual ~Cellular() = default;
    [[nodiscard]] virtual std::expected<void, CellularError> Initialize() = 0;
    [[nodiscard]] virtual CellularSnapshot Snapshot() const = 0;
    // Atomically reserve a stable network configuration for OTA. A successful
    // caller must EndFirmwareUpdate on every return path; configuration changes
    // and sleep must reject while reserved. Unsupported cellular services allow OTA.
    [[nodiscard]] virtual bool TryBeginFirmwareUpdate() = 0;
    virtual void EndFirmwareUpdate() = 0;
    virtual void RequestSignalRefresh() {}
    virtual void RequestSimRefresh() {}
    [[nodiscard]] virtual std::expected<void, CellularError> SetSimSlot(CellularSimSlot) {
        return std::unexpected(CellularError::kUnavailable);
    }
    virtual void SetStateChangeSink(CellularStateChangeSink sink, void* context) = 0;
    [[nodiscard]] virtual std::expected<void, CellularError> SetEnabled(bool enabled) = 0;
};

}  // namespace micropixel::device
