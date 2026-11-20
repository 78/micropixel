# PCA9535 IO expander (vendored)

Apache-2.0 driver copied from `wngfra/esp_io_expander_pca9535` 1.0.1.

The reTerminal D1001 uses this expander for the 3V3 rail latch, LCD power and
reset, touch reset, backlight enable, battery sampling, charger enable and the
speaker amplifier. The register sequence is the standard PCA9535 one; the
defaults in `reset()` (`0xFAFF` direction, `0xFBFF` output) match the vendor
BSP and are documented here so a future board does not inherit them by
accident.

MicroPixel changes from the published component:

* added SPDX headers;
* removed a stray `void func(void);` declaration from the public header;
* changed `OUT_REG_DEFAULT_VAL` from `0xFBFF` to `0xFFFF` so P1.0 keeps the
  D1001 3V3 rail latched from the first write (the vendor value can cut power
  on a battery-backed unit before board code latches the rail).