# Patched copy of espressif/esp_lcd_touch_spd2010

This directory is **not** upstream code. It is a copy of the component vendored in
`firmware/espressif/components/esp-iot-solution/components/display/lcd_touch/esp_lcd_touch_spd2010`
with two changes, both marked `LOCAL PATCH` in the source, because that component cannot read
registers under ESP-IDF 6.1 and because it reports an empty poll as a finger lift. The header,
`CMakeLists.txt` and `idf_component.yml` are identical to the originals.

## Change 1 - a register read must not be preceded by a command byte

In `esp_lcd_touch_spd2010.c`, the read helper used to be:

```c
#define i2c_read(data_p, len)  ESP_RETURN_ON_ERROR(esp_lcd_panel_io_rx_param(tp->io, 0, data_p, len), TAG, "Rx failed");
```

and is now:

```c
#define i2c_read(data_p, len)  ESP_RETURN_ON_ERROR(esp_lcd_panel_io_rx_param(tp->io, -1, data_p, len), TAG, "Rx failed");
```

### Why change 1 is needed

`esp_lcd_panel_io_rx_param()`'s `lcd_cmd` argument selects whether a command phase is emitted:

```c
/* ESP-IDF 6.1, components/esp_lcd/i2c/esp_lcd_panel_io_i2c.c */
bool send_param = (lcd_cmd != -1);
if (send_param) {
    ...
    size_t cmds_size = i2c_panel_io->lcd_cmd_bits / 8;   /* 0 for this component */
    i2c_master_transmit_receive(handle, write_buffer, write_size /* 0 */, buffer, buffer_size, ...);
}
```

The component's own configuration macro sets `lcd_cmd_bits = 0`, so passing `0` produced a
**zero-length write phase**, which ESP-IDF 6.1 rejects:

```
E i2c.master: i2c_master_transmit_receive(1318): i2c transmit buffer or size invalid
E lcd_panel.io.i2c: panel_io_i2c_rx_buffer(149): i2c transaction failed
E SPD2010: read_fw_version(419): Rx failed
E SPD2010: esp_lcd_touch_new_i2c_spd2010(139): Read version failed
```

Passing `-1` takes the other branch, `i2c_master_receive()`: a read with no preceding byte, which is
exactly what this controller needs. The driver writes the two-byte register address first and then reads
the answer, so the read must not be preceded by anything. This is also what ESP-IDF 5.x did — the
component worked there and stopped working when 6.1 changed the branch condition, so this patch restores
the intended behaviour rather than adding new behaviour.

No configuration of `esp_lcd_panel_io_i2c` can substitute for this change. `lcd_cmd_bits = 8` or `16`
writes stray command bytes before the read and corrupts the register address; enabling the control phase
writes a stray control byte for the same reason; and a custom panel IO is not an option either, because
`struct esp_lcd_panel_io_t` is not a public type (only forward-declared in `esp_lcd_types.h`).

## Change 2 - an empty poll is not a lift, and a zero-weight frame is

### What the driver used to publish

`read_data()` ended the same way on every path:

```c
tp_touch_t touch = {0};
tp_read_data(tp, &touch);          /* several branches read no frame at all */
tp->data.points = touch.touch_num; /* 0 when nothing was pending */
```

`tp_read_data()` reads the controller status first, and only one of its branches consumes a report:

| status seen | frame read | meaning |
|---|---|---|
| `tic_in_bios` | no | controller housekeeping |
| `tic_in_cpu` | no | controller housekeeping |
| `cpu_run && read_len == 0` | no | nothing pending |
| `pt_exist \|\| gesture` | yes | a report is waiting |
| `cpu_run && aux` | no | nothing pending |
| anything else | no | nothing pending |

Every "nothing pending" row still fell through with `touch_num == 0`, and `read_data()` published that
as `tp->data.points = 0`. To `esp_lcd_touch` and to everything above it, zero points *is* a release:
`esp_lcd_touch_get_data()` returns `ESP_OK` with `point_cnt == 0`.

### Why that breaks this board

The component is written for an interrupt-driven reader: read only when the controller says it has
data, and "no data" never becomes a frame. The SenseCAP Watcher cannot do that. Its touch interrupt
line is `EXPANDER P0.5`, not a GPIO, so `platform/boards/sensecap-watcher/touch_hardware.cpp` passes
`int_gpio_num = GPIO_NUM_NC` and `platform/input/esp_lcd_touch_input.cpp` falls back to a 10 ms poll
timer - 100 polls per second against a controller that reports at roughly 60 Hz. The empty polls are
the common case, not the rare one.

Two consequences, which the reader sees as a single symptom:

1. A poll landing between two reports published `points = 0`, a spurious **release**; the next poll
   with data published points again, a spurious **press**. One physical tap arrived as Up/Down/Up, and
   the system UI counts that as two taps, so tapping *Settings* also activated the first row of the
   settings menu.
2. A lift normally arrives as a frame whose points all carry `weight == 0` - exactly what this
   driver's own swipe tracking tests, `else if ((touch->rpt[0].weight == 0) && (touch->down == 1))`.
   That frame has `touch_num == 1`, so upstream published `points = 1` for it. It only became a
   release one poll later, when an *empty* poll published zero points.

So the release was taken from the polls that must not carry one, while the frame that does carry one
was published as a touch. The two errors masked each other, which is why the board appeared to work
while double-tapping.

### The change

Two edits in `esp_lcd_touch_spd2010.c`:

1. `read_data()` returns `ESP_ERR_INVALID_RESPONSE` and leaves `tp->data` untouched when
   `tp_read_data()` consumed no report frame. `ESP_ERR_INVALID_RESPONSE` is the contract already used
   in this component family for "no frame was available, keep the state you already have":
   `platform/input/esp_lcd_touch_input.cpp` maps it to exactly that and synthesizes neither Up nor
   Cancel. Any other error is still reported as an error.
2. `read_tp_hdp()` publishes a frame whose points all carry `weight == 0` as zero points, so the real
   lift report reaches the reader immediately instead of being borrowed from the next empty poll.

`tp_touch_t` gained one `bool frame_valid` field to carry step 1's decision from `tp_read_data()` to
`read_data()`. A lift is now only ever published from controller data, and no data is never published
as a state change.

This is deliberately not a debounce. A time-based filter would hide the symptom by suppressing
legitimate rapid taps and would leave `tp->data` misrepresenting the panel; here the state machine is
simply told which polls actually had a frame.

### The two changes are a pair

Change 2 is what keeps a lift reachable once change 1 stops inventing one. Taking change 1 without
change 2 would hold the finger down until an unrelated frame arrived. Take them together.

## Dropping this copy

`firmware/espressif/main/idf_component.yml` points the dependency at this directory with
`override_path`. When the upstream component is fixed - or when this board can read the touch
interrupt through the I/O expander instead of polling - delete this directory and the `override_path`
line so the dependency resolves to the vendored submodule again. The two differ only by the changes
documented above.

Both changes deserve upstream reports; this copy exists so the board works now. If the shared input
layer ever grows a "the controller has data" hook for expander-backed interrupt lines, change 1
becomes unnecessary - but change 2 stays correct, because it is about what a frame means, not about
how often one is read.
