#ifndef MICROPIXEL_FIRMWARE_REMOTE_PAIRING_POLICY_HPP
#define MICROPIXEL_FIRMWARE_REMOTE_PAIRING_POLICY_HPP

#include <string_view>

#include "host/controller/remote/remote_control_protocol.hpp"

namespace micropixel::firmware::remote_control {

constexpr bool MatchesPairingConsumed(double version, std::string_view frame_session_id,
                                      std::string_view frame_pairing_id, std::string_view session_id,
                                      std::string_view pairing_id) {
    return version == protocol::kVersion && !session_id.empty() && !pairing_id.empty() &&
           frame_session_id == session_id && frame_pairing_id == pairing_id;
}

}  // namespace micropixel::firmware::remote_control

#endif
