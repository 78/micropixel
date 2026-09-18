// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "cJSON.h"
#include "host/network/network.hpp"

namespace micropixel::host::remote {

inline void AddNetworkSnapshotJson(cJSON* network, const network::NetworkSnapshot& snapshot, uint64_t now_us) {
    if (!network) return;
    (void)cJSON_AddBoolToObject(network, "available", snapshot.available);
    (void)cJSON_AddBoolToObject(network, "enabled", snapshot.enabled);
    (void)cJSON_AddBoolToObject(network, "connected", snapshot.connected);
    (void)cJSON_AddStringToObject(network, "transport", network::TransportName(snapshot.route.transport));
    const auto add_text = [](cJSON* object, const char* key, const auto& text) {
        if (text[0]) (void)cJSON_AddStringToObject(object, key, text.data());
    };
    add_text(network, "macAddress", snapshot.route.mac);
    add_text(network, "ipAddress", snapshot.route.address);
    add_text(network, "gateway", snapshot.route.gateway);
    add_text(network, "netmask", snapshot.route.netmask);
    add_text(network, "hostname", snapshot.route.hostname);
    add_text(network, "dns", snapshot.route.dns);
    auto* wifi = cJSON_AddObjectToObject(network, "wifi");
    if (wifi) {
        (void)cJSON_AddBoolToObject(wifi, "available", snapshot.wifi.available);
        (void)cJSON_AddBoolToObject(wifi, "enabled", snapshot.wifi.enabled);
        (void)cJSON_AddBoolToObject(wifi, "connected", snapshot.wifi.connected);
        if (snapshot.wifi.connected) {
            add_text(wifi, "ssid", snapshot.wifi.ssid);
            (void)cJSON_AddNumberToObject(wifi, "rssi", snapshot.wifi.rssi);
        }
    }
    const auto& cell = snapshot.cellular;
    auto* cellular = cJSON_AddObjectToObject(network, "cellular");
    if (!cellular) return;
    (void)cJSON_AddBoolToObject(cellular, "available", cell.available);
    (void)cJSON_AddBoolToObject(cellular, "enabled", cell.enabled);
    (void)cJSON_AddBoolToObject(cellular, "connected", cell.connected);
    if (!cell.available || !cell.enabled) return;
    const auto& details = cell.diagnostics;
    const auto& telemetry = cell.telemetry;
    const bool sampled = telemetry.sampled_at_us > 0 && now_us >= telemetry.sampled_at_us;
    const uint64_t age_ms = sampled ? (now_us - telemetry.sampled_at_us) / 1000 : 0;
    const bool fresh = sampled && age_ms <= 60000 && !cell.sim_pending && !cell.switching;
    (void)cJSON_AddBoolToObject(cellular, "sampled", sampled);
    (void)cJSON_AddBoolToObject(cellular, "fresh", fresh);
    if (sampled) {
        (void)cJSON_AddNumberToObject(cellular, "sampledAtMs", telemetry.sampled_at_us / 1000);
        (void)cJSON_AddNumberToObject(cellular, "ageMs", age_ms);
    }
    (void)cJSON_AddNumberToObject(cellular, "signalBars", cell.signal_bars);
    if (fresh && details.signal_csq >= 0 && details.signal_csq <= 31)
        (void)cJSON_AddNumberToObject(cellular, "csq", details.signal_csq);
    else
        (void)cJSON_AddNullToObject(cellular, "csq");
    if (!fresh) return;
    add_text(cellular, "carrier", details.operator_name);
    add_text(cellular, "imei", telemetry.imei);
    add_text(cellular, "iccid", telemetry.iccid);
    add_text(cellular, "mcc", telemetry.mcc);
    add_text(cellular, "mnc", telemetry.mnc);
    if (details.registration >= 0) (void)cJSON_AddNumberToObject(cellular, "registration", details.registration);
    if ((details.registration == 1 || details.registration == 5) && telemetry.tac[0] && telemetry.cell_id[0] &&
        telemetry.access_technology >= 0) {
        auto* cereg = cJSON_AddObjectToObject(cellular, "cereg");
        if (cereg) {
            (void)cJSON_AddNumberToObject(cereg, "stat", details.registration);
            add_text(cereg, "tac", telemetry.tac);
            add_text(cereg, "ci", telemetry.cell_id);
            (void)cJSON_AddNumberToObject(cereg, "AcT", telemetry.access_technology);
        }
    }
}

}  // namespace micropixel::host::remote
