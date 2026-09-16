# MicroPixel integration

Source: `78/uart-uhci` version 0.2.2 from the ESP Component Registry,
<https://github.com/78/uart-uhci>. Upstream package metadata declares Apache-2.0.
The package did not ship a license text; `LICENSE` supplies the standard Apache-2.0 text.

This local dependency is selected only for ESP32-P4. The factory NT26 modem uses
its fixed RX buffer pool and UART FIFO transmit API. ESP-IDF 6.1 compatibility changes:

- Require `esp_driver_dma`, which now owns the private GDMA headers.
- Use `UHCI_LL_NUM` instead of the removed `SOC_UHCI_NUM`.
- Allocate the RX channel with the new three-argument `gdma_new_ahb_channel` API;
  the removed `direction` field is replaced by a null TX output parameter.

The RX pool is bounded to 32 buffers. DMA remount uses a preallocated PSRAM workspace instead of a task/ISR stack
array or per-recovery allocation. RX data and DMA descriptors remain in internal
DMA-capable RAM. ReturnBuffer and remount are task-only, with context assertions;
ISR callbacks must defer even queue-full recovery to the owning task. The local
README examples document this stricter contract.

Trailing whitespace was removed for the repository diff check. Other upstream behavior is retained. Build compatibility does not
verify the modem handshake, DMA reception, overflow recovery or power lifecycle;
these still require integration and hardware acceptance.
