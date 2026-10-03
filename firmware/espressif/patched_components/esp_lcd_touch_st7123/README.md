# Patched copy of espressif/esp_lcd_touch_st7123

This directory is **not** upstream code. It is a copy of the component vendored in
`firmware/espressif/components/esp-iot-solution/components/display/lcd_touch/esp_lcd_touch_st7123`
(version 1.0.2, without `test_apps/`) with three changes in `esp_lcd_touch_st7123.c`, all marked
`LOCAL PATCH`. The header, `CMakeLists.txt`, `idf_component.yml` and license are unchanged.

## Change 1 - report a track ID per finger

Upstream fills only `x`, `y` and `strength` and registers no `get_track_id` callback, so
`esp_lcd_touch_get_data()` returns every point with `track_id == 0`.
`platform/input/esp_lcd_touch_input.cpp` pairs frames by track ID, so two simultaneous fingers on the
KSDIY P4C5 board collapsed into one pointer and the second finger never produced its own Down/Up.

`read_data()` now stores the controller's report slot index, which stays with a finger while it is
down, as `track_id`, and `get_track_id()` returns it. `esp_lcd_touch_get_data()` calls `get_xy()`,
which consumes `data.points`, before `get_track_id()`, so the latter copies the requested count from
`data.coords` instead of trusting the cleared point count.

## Change 2 - bound the report count

`read_data()` reads the controller's `MAX_TOUCHES` register into a ten-entry `touch_report[]` and
then reads that many reports. A corrupt or unexpected value overflowed the stack array; it is now
clamped to the array size.

## Change 3 - pull up the open-drain interrupt line

The ST7123 drives an active-low, open-drain INT. Upstream configures the GPIO without a pull-up, so
the line floated between reports and produced spurious edges. An active-low INT now enables the
internal pull-up.

## Dropping this copy

`firmware/espressif/main/idf_component.yml` points the dependency at this directory with
`override_path`. When upstream reports track IDs and bounds the report count, delete this directory
and point the dependency back at the submodule. `tools/tests/test_st7123_report.c` exercises the
copied source directly.
