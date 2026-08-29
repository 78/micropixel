#pragma once

#include <array>
#include <cstdint>

#include "host/ui/system_ui.hpp"

namespace micropixel::firmware::remote_control {

struct RemoteIdentity final {
    std::array<char, host_ui::kRemoteControlDeviceIdCapacity> device_id{};
    std::array<char, 1024U> credential{};
    uint32_t auth_epoch{1U};
};

class RemoteIdentityStore final {
   public:
    RemoteIdentityStore();
    ~RemoteIdentityStore();
    RemoteIdentityStore(const RemoteIdentityStore&) = delete;
    RemoteIdentityStore& operator=(const RemoteIdentityStore&) = delete;

    [[nodiscard]] bool Load(RemoteIdentity& identity) const;
    [[nodiscard]] bool Save(const RemoteIdentity& identity) const;
    [[nodiscard]] bool Clear() const;

   private:
    struct Record;
    Record* record_{};
};

}  // namespace micropixel::firmware::remote_control
