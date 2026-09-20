// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <map>
#include <string>
#include <vector>

#include "nvs.h"

namespace fake_nvs {
struct Value {
    nvs_type_t type;
    std::vector<uint8_t> bytes;
};
using Namespace = std::map<std::string, Value>;
inline std::map<std::string, Namespace> data;
inline std::string fail_commit_partition;
inline bool fail_erase{};
inline bool fail_write{};
inline bool partition_present{true};
inline uint32_t partition_bytes{65536U};
inline nvs_stats_t stats{};
inline esp_err_t stats_status{ESP_OK};
}  // namespace fake_nvs
