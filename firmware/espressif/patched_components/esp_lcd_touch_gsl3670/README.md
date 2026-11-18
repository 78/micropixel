# GSL3670 touch driver (MicroPixel)

Minimal, Apache-2.0 GSL3670 driver for the Seeed reTerminal D1001. It is a
MicroPixel component, not a copy of the Seeed BSP driver.

## Why this is not the vendor component

The vendor component (`Seeed-Studio/reTerminal-D1001`,
`components/esp_lcd_touch_gsl3670`) depends on `gsl_point_id.c`, which is
**GPL-2.0** Silead code, and on a large firmware blob in its public header.
MicroPixel is Apache-2.0 and cannot ship either. This component therefore
implements only:

* the I2C register interface (`0x80` report register, `0x88/0xE4/0xE0` bring-up),
* reset through a board callback (the D1001 reset is on the PCA9535 expander),
* raw report parsing for up to three fingers.

It does **not** load the Silead firmware or run Silead's coordinate/ID
post-processing. The controller is expected to run its factory firmware.

## Hardware validation status

Tested on a D1001 (P4 v3.2):

* I2C works: `0xF0` write/readback returns the written bytes.
* Loading the vendor firmware image succeeds and `0xB0` reads
  `5A 5A 5A 5A`.
* The `0x80` report register still reads all zeros, idle and while touching.
* The full Seeed driver (firmware **and** Silead's GPL algorithm) initializes,
  but no `vendor report` lines appeared in the captured test window. A
  confirmed physical tap during that build is still needed.

A redistribution-compatible GSL3670 firmware/driver must be obtained from Seeed
or Silead before touch can be enabled in this Apache-2.0 profile. Do not copy
the GPL `gsl_point_id.c` or the firmware blob into this repository.

## Original IDF

The Seeed reference was written for ESP-IDF v5.4.x/5.5. This component targets
MicroPixel's ESP-IDF 6.1 APIs (`esp_lcd_panel_io_i2c_config_t`,
`esp_lcd_touch_t`, `esp_lcd_panel_io_rx_param`).