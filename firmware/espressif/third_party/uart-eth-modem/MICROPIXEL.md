# NT26 factory driver port

Source: `main/boards/common/uart_eth_modem.{h,cc}` from
[MetalioClaw4](https://github.com/kalicyh/MetalioClaw4), revision
`ca3aa3fa027ff7dad2adf0c2d03c4f24aa838950`. The header identifies
`78/uart-eth-modem` 0.4.2; this copy includes the factory's additional AT collection
and SIM recovery behavior. Copyright 2025 Terrence, Apache-2.0.

The port retains the factory frame header, handshake bytes, MRDY/SRDY state machine,
baud detection, NAT/PDP configuration, registration sequence, flight mode and modem
sleep commands. `nt26_frame.h` extracts the original wire layout for Host tests.

Local changes:

- ESP-IDF 6.1 UART configuration and explicit component dependencies.
- A start-time PSRAM workspace owns the AT command/response buffers and the
  32-slot, 1600-byte TX pool. Allocation failure aborts startup; shutdown joins
  workers and AT callers before release, and a stop timeout retains the workspace.
  Semaphore storage and ISR-visible control state stay in internal RAM. A size
  assertion guards against embedding large buffers in the board object again.
- The TX pool replaces per-send payload allocations and stack
  completion pointers. Timeout releases only the caller's reference; the worker
  retains the slot until completion. Queue-full sends return an error immediately.
- AT responses use a synchronized 4096-byte buffer, preserve fragmented results,
  recognize complete result lines and reject overflow. Collection markers are
  bounded to 63 bytes. APN/PDP parameters cannot inject an AT command.
- Partial startup failure joins all workers that were actually created. Stop wakes
  every worker, preserves its stop bit, checks all completion bits and retains
  resources on timeout. A timer-task barrier drains deferred ISR event-bit updates.
- GPIO interrupts start only after worker creation and stop before queue cleanup.
  Dropped RX queue entries return to the DMA pool from task context.
- Netif publication is synchronized; failed attachment is cleaned up. SIM/registration
  recovery keeps the AT channel without creating an Ethernet interface prematurely.
  Only matching netif IP events establish or remove IP connectivity; registration
  changes also update the Ethernet link state.
- Trailing whitespace is removed for the repository diff check.

The iot_eth RX boundary still explicitly allocates a bounded Ethernet payload:
its glue transfers ownership to ESP-NETIF and frees it through `driver_free_rx_buffer`.
TX and AT cross-task storage does not allocate per frame.

Ownership contract: one board control worker serializes Start, Stop, configuration
and information queries. Install the network callback before Start and keep it
unchanged until Stop completes. The callback must enqueue a state update and return;
it must not synchronously query AT or join workers. The factory PDP callback may
set the APN synchronously. Stop may return a timeout and must then be retried before
destroying the object. Destruction with live workers is a fatal contract violation.
The GPIO ISR service and ESP-NETIF/default event loop must be initialized by the board.

Current integration: the Claw4 `CellularController` starts this driver when the
saved cellular switch is enabled and owns power, signal queries and shutdown.
The Host UI switches cellular independently of Wi-Fi without rebooting. Cellular
route priority is 50, below the default Wi-Fi station priority of 100. ESP-NETIF selects the default interface automatically through iot_eth start,
link and DHCP events, and removes it on destruction. No manual default-route
override is needed. No new Guest ABI is introduced. See `../../main/platform/boards/README.zh-CN.md` for the integration and target
acceptance requirements.

Validation: `tools/tests/test_firmware_host.sh` exercises the actual wire header,
TX pool timeout/completion ordering, exhaustion and cancellation, and bounded AT
fragment/marker/error handling. These tests do not simulate the entire FreeRTOS
driver. Modem handshake, DMA overflow, task-creation failure, shutdown under traffic,
SIM recovery, DHCP and sleep/wakeup still require target acceptance.
