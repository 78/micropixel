# Bouffalo FreeRTOS/WAMR foundation

The two build profiles compose the same platform port with the public SDK's
FreeRTOS kernel. BL616CL reserves its 4 MiB PSRAM region; BL618DG B0 uses the
AP core with NP disabled and 16 MiB PSRAM. The vendor startup, linker and
Picolibc TLS support remain responsible for board initialization.

## WAMR execution contract

- The source revision is the existing Espressif WAMR submodule pin, AOT format 6.
  No WAMR fork changes or Guest ABI changes are required.
- The target is RV32 ILP32F. Interpreter, JIT, WASI, libc builtins, pthreads,
  thread manager, shared memory and SIMD are disabled. This port does not
  implement WAMR thread creation, condition waits or native WASI descriptors.
- Bulk memory, reference types, custom names, memory profiling and stack dumps
  follow the Host configuration. Linear memory reserves its maximum capacity
  so existing Guest surface addresses remain stable while pages become visible.
- WAMR allocations and 64-byte-aligned, zero-filled anonymous mappings use
  PSRAM. Reallocation preserves the old block on allocation failure; in-place
  linear-memory growth zeroes newly exposed bytes. Fixed mappings are rejected.
- There is no per-mapping MMU protection. `os_mprotect` is a compatibility hook;
  hardware bounds checks are disabled and WAMR software bounds checks remain
  enabled. This does not provide hardware process isolation.
- Executable AOT writes clean the data cache before `fence.i`. Boot time is the
  SDK microsecond timer. The CPU-time hook reports elapsed time, not measured
  per-task CPU consumption.
- Recursive mutexes use native FreeRTOS semaphores, including WAMR's static
  mutex used before platform initialization. Stack boundaries come from the
  current task's native stack base. All hooks run in task context.

## Host compatibility surface

`common/include/` contains a deliberately limited compatibility layer for shared
Host code that currently names ESP allocation and FreeRTOS interfaces. Include
it only in Bouffalo targets. It does not replace ESP-IDF headers for Espressif
builds or advertise complete ESP-IDF API compatibility.

Task creation accepts byte-sized stacks and converts to native stack words,
rounding up. A requested core is ignored on these single-scheduler profiles;
stack high-water values are converted back to bytes. PSRAM allocations reject
internal/DMA combinations. Unsupported capability bits fail allocation.

## Validation executable

The build commands in [the platform guide](../README.md) link
`examples/runtime_smoke.c`. On a prepared board it checks aligned PSRAM
allocation and reallocation contents, recursive/lazy mutexes, native stack-base
lookup, anonymous zero-filled mappings, invalid-module rejection, and WAMR
initialization/destruction/reinitialization. It prints
`MicroPixel FreeRTOS/WAMR runtime smoke PASS` only after these checks succeed.

This target has no desktop, Bundle installer, storage, display, touch, audio,
Guest Service imports or execution watchdog. It does not claim full game support.
Compile/link validation of this split is distinct from hardware regression of
the downstream integrated firmware. Board release images and their runtime
acceptance remain follow-up work.
