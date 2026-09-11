#pragma once

#include <cstdint>
#include <expected>

namespace micropixel::device {

enum class CellularState : uint8_t { kOff, kConnecting, kConnected, kUnregistered, kFailed };
enum class CellularSimSlot : uint8_t { kExternal, kInternal, kUnknown };

enum class CellularError : uint8_t { kUnavailable, kBusy, kStorage, kOperationFailed };

struct CellularSnapshot final {
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

// Host-only cellular control. The board retains its factory network selection
// policy: SetEnabled persists the selected mode and restarts the device.
class Cellular {
   public:
    virtual ~Cellular() = default;
    [[nodiscard]] virtual std::expected<void, CellularError> Initialize() = 0;
    [[nodiscard]] virtual CellularSnapshot Snapshot() const = 0;
    virtual void RequestSignalRefresh() {}
    virtual void RequestSimRefresh() {}
    [[nodiscard]] virtual std::expected<void, CellularError> SetSimSlot(CellularSimSlot) {
        return std::unexpected(CellularError::kUnavailable);
    }
    virtual void SetStateChangeSink(CellularStateChangeSink sink, void* context) = 0;
    [[nodiscard]] virtual std::expected<void, CellularError> SetEnabled(bool enabled) = 0;
};

}  // namespace micropixel::device
