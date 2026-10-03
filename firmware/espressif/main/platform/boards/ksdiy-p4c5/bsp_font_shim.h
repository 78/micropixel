// SPDX-License-Identifier: Apache-2.0
#pragma once

// Force-included only into kevincoooool/ksdiy_p4c5_bsp. Its LVGL port, which
// MicroPixel does not call, draws a watermark with lv_font_montserrat_14, while
// MicroPixel compiles that font out of LVGL. Name the system font proxy instead
// so the unused function compiles. Drop this once the BSP guards the watermark
// with LV_FONT_MONTSERRAT_14.
#define lv_font_montserrat_14 micropixel_system_font_default
