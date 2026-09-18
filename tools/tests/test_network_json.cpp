// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "host/controller/remote/network_snapshot_json.hpp"

namespace {
void Check(bool value, const char* message) {
    if (value) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
}
cJSON* Item(cJSON* object, const char* key) { return cJSON_GetObjectItemCaseSensitive(object, key); }
void Text(cJSON* object, const char* key, const char* expected) {
    auto* item = Item(object, key);
    Check(cJSON_IsString(item) && !std::strcmp(item->valuestring, expected), key);
}
cJSON* Serialize(const micropixel::host::network::NetworkSnapshot& snapshot, uint64_t now) {
    auto* original = cJSON_CreateObject();
    micropixel::host::remote::AddNetworkSnapshotJson(original, snapshot, now);
    auto* text = cJSON_PrintUnformatted(original);
    auto* parsed = cJSON_Parse(text);
    cJSON_free(text);
    cJSON_Delete(original);
    Check(parsed != nullptr, "serialized network is valid JSON");
    return parsed;
}
}  // namespace
int main() {
    using namespace micropixel;
    host::network::NetworkSnapshot snapshot{};
    snapshot.available = snapshot.enabled = snapshot.connected = true;
    snapshot.route.transport = device::NetworkTransport::kWifi;
    snapshot.wifi.available = snapshot.wifi.enabled = snapshot.wifi.connected = true;
    std::snprintf(snapshot.wifi.ssid.data(), snapshot.wifi.ssid.size(), "test-network");
    snapshot.wifi.rssi = -54;
    auto& cell = snapshot.cellular;
    cell.available = cell.enabled = cell.connected = true;
    cell.diagnostics.registration = 5;
    cell.diagnostics.signal_csq = 18;
    std::snprintf(cell.diagnostics.operator_name.data(), cell.diagnostics.operator_name.size(), "Test \"Carrier\"");
    std::snprintf(cell.telemetry.imei.data(), cell.telemetry.imei.size(), "000000000000000");
    std::snprintf(cell.telemetry.iccid.data(), cell.telemetry.iccid.size(), "00000000000000000000");
    std::snprintf(cell.telemetry.tac.data(), cell.telemetry.tac.size(), "00AB");
    std::snprintf(cell.telemetry.cell_id.data(), cell.telemetry.cell_id.size(), "00123456");
    cell.telemetry.access_technology = 7;
    cell.telemetry.sampled_at_us = 1000000;
    auto* result = Serialize(snapshot, 2000000);
    Text(result, "transport", "wifi");
    Text(Item(result, "wifi"), "ssid", "test-network");
    Check(!Item(result, "ssid") && !Item(result, "rssi"), "Wi-Fi fields exist only in the nested interface");
    auto* cellular = Item(result, "cellular");
    Text(cellular, "carrier", "Test \"Carrier\"");
    Text(cellular, "imei", "000000000000000");
    Text(cellular, "iccid", "00000000000000000000");
    Text(Item(cellular, "cereg"), "tac", "00AB");
    Check(Item(cellular, "csq")->valueint == 18 && Item(cellular, "ageMs")->valueint == 1000,
          "4G telemetry is reported while Wi-Fi is the default route");
    cJSON_Delete(result);
    snapshot.route.transport = device::NetworkTransport::kCellular;
    result = Serialize(snapshot, 2000000);
    Text(result, "transport", "cellular");
    Text(Item(result, "wifi"), "ssid", "test-network");
    Check(!Item(result, "ssid"), "Wi-Fi fields remain nested with a cellular default route");
    cJSON_Delete(result);
    cell.diagnostics.signal_csq = 99;
    cell.diagnostics.registration = 2;
    result = Serialize(snapshot, 2000000);
    cellular = Item(result, "cellular");
    Check(cJSON_IsNull(Item(cellular, "csq")) && !Item(cellular, "cereg"),
          "unknown signal/searching has no usable cell");
    cJSON_Delete(result);
    cell.diagnostics.registration = 1;
    for (const auto now : {999999ULL, 62000000ULL}) {
        result = Serialize(snapshot, now);
        cellular = Item(result, "cellular");
        Check(!Item(cellular, "cereg") && !Item(cellular, "iccid") && cJSON_IsFalse(Item(cellular, "fresh")),
              "future or expired snapshots cannot provide location");
        cJSON_Delete(result);
    }
    cell.sim_pending = true;
    result = Serialize(snapshot, 2000000);
    Check(!Item(Item(result, "cellular"), "cereg"), "SIM switch suppresses previous location");
    cJSON_Delete(result);
    cell.enabled = false;
    result = Serialize(snapshot, 2000000);
    cellular = Item(result, "cellular");
    Check(cJSON_IsFalse(Item(cellular, "enabled")) && !Item(cellular, "imei") && !Item(cellular, "cereg"),
          "disabled radio emits status without old identity/location");
    cJSON_Delete(result);
    std::puts(
        "Network JSON: simultaneous links, nested interfaces, identity, cell freshness and unknown signal passed");
}
