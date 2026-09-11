#pragma once

#include "device/contracts/cellular.hpp"

namespace micropixel::host_ui {

struct CellularConnectionMessage final {
    const char* title;
    const char* hint;
};

inline CellularConnectionMessage DescribeCellularConnection(bool enabled, bool connected, device::CellularState state,
                                                            const device::CellularDiagnostics& details) {
    using Sim = device::CellularSimStatus;
    if (!enabled) return {"4G is off", "Wi-Fi mode is selected. Switch to 4G below to use a SIM card."};
    if (connected) return {"Connected", "The device has a mobile data connection."};
    if (!details.sampled) return {"Checking mobile network", "Reading SIM and network status from the modem..."};
    if (details.sim_status == Sim::kPinRequired)
        return {"SIM PIN required", "Unlock this SIM in another device, then insert it again."};
    if (details.sim_status == Sim::kPukRequired)
        return {"SIM PUK required", "Contact your SIM provider for the PUK. Do not guess the code."};
    if (details.sim_status == Sim::kAbsent)
        return {"No SIM detected", "Check the selected slot. Insert the external SIM or select the internal SIM."};
    if (details.sim_status == Sim::kNotReady)
        return {"SIM is not ready", "Check the selected SIM. Reinsert the external card or try the other slot."};
    if (details.radio_function == 0 || details.radio_function == 4)
        return {"Mobile radio is off", "Wait for SIM switching to finish. If it stays off, restart the device."};
    if (details.registration == 3)
        return {"Registration denied",
                "The network rejected registration. Check SIM activation and service with your provider."};
    if (details.registration == 2)
        return {"Searching for network",
                "Not registered yet. Check the 4G antenna and coverage, then refresh. This does not prove the SIM is "
                "inactive."};
    if (details.registration == 1 || details.registration == 5) {
        if (details.attached == 0)
            return {
                "Registered, no mobile data",
                "The SIM registered, but data is not attached. Check the SIM data service and APN with your provider."};
        return {"Waiting for data connection",
                "Registration succeeded. The device has not obtained a working data connection yet. Refresh or restart "
                "if it persists."};
    }
    if (details.incomplete || state == device::CellularState::kFailed)
        return {"Status unavailable",
                "Some modem queries failed. Refresh to retry; check modem power if this continues."};
    return {"Not registered", "The SIM has not registered on a network. Check the selected SIM, antenna and coverage."};
}

inline const char* CellularRegistrationText(int status) {
    switch (status) {
        case 0:
            return "Not registered";
        case 1:
            return "Registered (home)";
        case 2:
            return "Searching";
        case 3:
            return "Registration denied";
        case 4:
            return "Unknown to modem";
        case 5:
            return "Registered (roaming)";
        default:
            return "Unknown / unavailable";
    }
}

}  // namespace micropixel::host_ui
