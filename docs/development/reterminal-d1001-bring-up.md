# Seeed reTerminal D1001 bring-up

The D1001 profile is an ESP32-P4 board with an 8-inch 800x1280 MIPI-DSI panel,
GSL3670 touch, an ESP32-C6 hosted over SDIO, and a PCA9535 rail expander.

Seeed's BSP repository (<https://github.com/Seeed-Studio/reTerminal-D1001>) is a
hardware reference only. It targets ESP-IDF v5.4.x/5.5; MicroPixel targets
ESP-IDF 6.1, so every adapted file lists its divergence in a README or SPDX
header.

## Pin map

| Function | Pins |
|---|---|
| Panel | MIPI-DSI 2 lanes, 1500 Mbps, DPI 60 MHz; 800x1280, RGB565 |
| DSI PHY LDO | channel 3, 2500 mV |
| Backlight | LEDC PWM GPIO14; enable PCA9535 P0.7 |
| Panel reset | PCA9535 P0.2 |
| Touch | GSL3670, I2C0 SCL 38 / SDA 37, address 0x40, INT GPIO16, reset PCA9535 P1.4 |
| System I2C | I2C1 SCL 21 / SDA 20: PCA9535 0x20, ES8311 0x18, PCF8563 0x51, LSM6DS3TR-C |
| PCA9535 rails | P0.0 LCD power, P0.2 LCD reset, P0.6 battery read, P0.7 backlight; P1.0 3V3 hold, P1.2 charge, P1.3 PA, P1.4 touch reset |
| Audio | ES8311 I2S0 MCLK 33 / BCLK 32 / LRCLK 31 / SDOUT 30; PA PCA9535 P1.3 |
| Wi-Fi C6 | SDIO CMD 6, CLK 11, D0 7, D1 8, D2 9, D3 10, CHIP_PU 13; UART TX 48 / RX 47, BOOT 12 |
| SD card | SDMMC slot 0, CLK 43 / CMD 44 / D0 39 / D1 40 / D2 41 / D3 42; detect 45, power 46, LDO 4 |
| Power key | GPIO3 active-low; long press 1 s |
| Battery | ADC GPIO18; charger status GPIO15 |

## Power and reset order

1. Create I2C1, create the PCA9535 at 0x20, set all 16 pins as outputs.
2. P1.0 (`PWR_HOLD`) high, wait 50 ms.
3. P0.7 (backlight enable) and P0.0 (LCD power) high, wait 50 ms.
4. P0.2 (LCD reset) high, then pulse 1 -> 0 (5/10 ms) -> 1 (120 ms).
5. Pulse P1.4 (touch reset); the touch driver pulses it again during startup.
6. Bring up the DSI LDO and bus, run the JD9365 init table, then start LVGL.
   The backlight duty stays 0 until the first UI frame.

The PCA9535 power-up default is `0xFFFF` in the vendored driver, so P1.0 keeps
the 3V3 rail latched from the first register write. The green key is disabled in
the MVP until its polarity is validated: if it idles low, the polling driver
would see a one-second hold and power the board off immediately. Idle power-off
is likewise `kDisabled` during bring-up; the System menu still powers off.

## Display

`display/jd9365_init_data.c` copies only the vendor init table; the driver is
the registry `espressif/esp_lcd_jd9365` component. Timing from the vendor BSP:
`hbp 20 / hpw 20 / hfp 40`, `vbp 30 / vpw 4 / vfp 30`, DPI 60 MHz. The resulting
scanout is about 50.7 Hz; raise the DPI clock to about 71 MHz only after
measuring the module.

The 800 px RGB565 row is 1600 bytes, which is 32-byte but not 48-byte aligned.
`sdkconfig.p4-d1001.defaults` therefore sets `CONFIG_LV_DRAW_BUF_STRIDE_ALIGN=32`
so LVGL's stride matches the DPI framebuffers.

On P4 v3.x the DSI PHY reference clock must be
`MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT` (XTAL). The legacy
`MIPI_DSI_PHY_CLK_SRC_DEFAULT` still resolves to PLL_F20M and the v3 HAL aborts
inside `esp_lcd_new_dsi_bus()`. Production D1001 units are P4 v3.x, so the board
uses that source directly.

## Touch

The D1001 profile depends on the external `wvirgil123/esp_lcd_touch_gsl3670`
component. It contains Silead's coordinate correction and point-tracking
algorithm (GPL-2.0-or-later) plus the controller firmware image Seeed supplies
under Apache-2.0. The component stays outside this Apache-2.0 repository; the
manifest selects it only for the D1001 profile.

The component wraps the algorithm in `esp_lcd_touch`: reset through the board's
PCA9535 callback, a correct 44-byte report buffer, a contact-count clamp, and
`get_track_id` so multi-touch contacts keep stable IDs. It verifies the `0xB0`
firmware signature during initialization.

The board enables it by default:
`CONFIG_MICROPIXEL_D1001_ENABLE_GSL3670=y`. Enabling it makes the resulting
D1001 firmware a combined GPL work; the MicroPixel repository itself remains
Apache-2.0. Set the option to `n` to build a touch-free image without the GPL
dependency.

During local development the manifest uses an `override_path` pointing at a
sibling component checkout. Remove that override once the component is
published in the ESP Component Registry.

Initialization is best-effort: a firmware or first-probe failure logs a warning,
drops the touch backend and keeps the display, backlight and USB development
transport running. USB capture and App installation never depend on touch being
present.

## Build and flash

```sh
bash tools/d1001.sh build-host
bash tools/d1001.sh flash-host "$D1001_PORT"
bash tools/d1001.sh monitor "$D1001_PORT"
bash tools/d1001.sh flash-all "$D1001_PORT"   # first flash: Host + app_store
```

Before the first flash, confirm the unit with esptool:

```sh
python -m esptool --chip esp32p4 --port <PORT> chip_id
python -m esptool --chip esp32p4 --port <PORT> flash_id
```

Production D1001 units are P4 v3.x with 32 MiB flash using `partitions.p4.csv`
(24 MiB app_store). `CONFIG_BOOTLOADER_CACHE_32BIT_ADDR_QUAD_FLASH` and
`CONFIG_IDF_EXPERIMENTAL_FEATURES` must stay enabled: without 32-bit cache
mapping, BundleFS cannot map app_store blocks above the 16 MiB physical flash
boundary.

## Power-up order

The PCA9535 driver writes a safe output latch (only P1.0, the 3V3 hold) while
the rail pins are still inputs, then switches directions. `ConfigureRails()`
raises LCD power, backlight enable and the resets in order, and leaves the
speaker amplifier off until ES8311 is configured. Do not restore the vendor
`0xffff` / `0xfbff` latch: both assert every output at the first direction
write, which makes the rail delays ineffective and enables the amplifier for
roughly 100 ms before it is cleared.

## Acceptance

- Cold boot: correct colors, no shift or tearing, backlight dark before the
  first UI frame.
- Touch (with `MICROPIXEL_D1001_ENABLE_GSL3670`): four corners, edges, drag,
  lift; no X/Y swap; distinct track IDs for two fingers.
- Hall -> Guest -> back; `sdk-demo` and `tilt` use direct scanout (check logs).
- USB local control and screenshots match the panel.
- Wi-Fi: ESP-Hosted initializes and the C6 slave firmware matches the host's
  `espressif/esp_hosted ~2`; a missing C6 only disables the network.
- 10 minutes of interaction without watchdog resets or declining free PSRAM.

Audio is implemented through ES8311 on I2S0 (MCLK 33 / BCLK 32 / LRCLK 31 /
SDOUT 30, codec on I2C1 at 0x18) with the speaker amplifier on PCA9535 P1.3.
SD card, battery, RTC and the IMU are follow-up milestones; the MVP registers
no battery/storage peripheral and `Platform::Publish()` fills them with
unavailable implementations.