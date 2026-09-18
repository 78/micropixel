#pragma once

#include "device/contracts/cellular.hpp"
#include "host_strings.hpp"

namespace micropixel::host_ui {

struct CellularConnectionMessage final {
    const char* title;
    const char* hint;
};

inline CellularConnectionMessage DescribeCellularConnection(
    bool enabled, bool connected, device::CellularState state, const device::CellularDiagnostics& details,
    const host_strings::Catalog& strings = host_strings::ForTag("en")) {
    using Sim = device::CellularSimStatus;
    if (!enabled)
        return {strings.Get(host_strings::Id::kCellularDisabled), strings.Get(host_strings::Id::kCellularDisabledHint)};
    if (connected)
        return {strings.Get(host_strings::Id::kCellularConnected),
                strings.Get(host_strings::Id::kCellularConnectedHint)};
    if (!details.sampled)
        return {strings.Get(host_strings::Id::kCellularChecking), strings.Get(host_strings::Id::kCellularCheckingHint)};
    if (details.sim_status == Sim::kPinRequired)
        return {strings.Get(host_strings::Id::kCellularSimPin), strings.Get(host_strings::Id::kCellularSimPinHint)};
    if (details.sim_status == Sim::kPukRequired)
        return {strings.Get(host_strings::Id::kCellularSimPuk), strings.Get(host_strings::Id::kCellularSimPukHint)};
    if (details.sim_status == Sim::kAbsent)
        return {strings.Get(host_strings::Id::kCellularAbsent), strings.Get(host_strings::Id::kCellularAbsentHint)};
    if (details.sim_status == Sim::kNotReady)
        return {strings.Get(host_strings::Id::kCellularSimNotReady),
                strings.Get(host_strings::Id::kCellularSimNotReadyHint)};
    if (details.radio_function == 0 || details.radio_function == 4)
        return {strings.Get(host_strings::Id::kCellularRadioOff), strings.Get(host_strings::Id::kCellularRadioOffHint)};
    if (details.registration == 3)
        return {strings.Get(host_strings::Id::kCellularDenied), strings.Get(host_strings::Id::kCellularDeniedHint)};
    if (details.registration == 2)
        return {strings.Get(host_strings::Id::kCellularSearching),
                strings.Get(host_strings::Id::kCellularSearchingHint)};
    if (details.registration == 1 || details.registration == 5) {
        if (details.attached == 0)
            return {strings.Get(host_strings::Id::kCellularNoData), strings.Get(host_strings::Id::kCellularNoDataHint)};
        return {strings.Get(host_strings::Id::kCellularWaitingData),
                strings.Get(host_strings::Id::kCellularWaitingDataHint)};
    }
    if (details.incomplete || state == device::CellularState::kFailed)
        return {strings.Get(host_strings::Id::kCellularStatusUnavailable),
                strings.Get(host_strings::Id::kCellularStatusUnavailableHint)};
    return {strings.Get(host_strings::Id::kCellularNotRegistered),
            strings.Get(host_strings::Id::kCellularNotRegisteredHint)};
}

inline const char* CellularRegistrationText(int status,
                                            const host_strings::Catalog& strings = host_strings::ForTag("en")) {
    switch (status) {
        case 0:
            return strings.Get(host_strings::Id::kCellularNotRegistered);
        case 1:
            return strings.Get(host_strings::Id::kCellularRegisteredHome);
        case 2:
            return strings.Get(host_strings::Id::kCellularSearchingValue);
        case 3:
            return strings.Get(host_strings::Id::kCellularDenied);
        case 4:
            return strings.Get(host_strings::Id::kCellularUnknownModem);
        case 5:
            return strings.Get(host_strings::Id::kCellularRegisteredRoaming);
        default:
            return strings.Get(host_strings::Id::kCellularUnknownUnavailable);
    }
}

}  // namespace micropixel::host_ui
