// SPDX-License-Identifier: Apache-2.0
#pragma once

// Process-lifetime Host BSS. Expands to EXT_RAM_BSS_ATTR when
// CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY is set; empty on targets
// without PSRAM BSS (Null compile gate).

#include "sdkconfig.h"

#if CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
#include "esp_attr.h"
#define MICROPIXEL_EXT_RAM_BSS EXT_RAM_BSS_ATTR
#else
#define MICROPIXEL_EXT_RAM_BSS
#endif
