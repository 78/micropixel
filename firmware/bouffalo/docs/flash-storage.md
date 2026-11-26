# Bouffalo Flash storage

The Flash adapter exposes the SDK MTD partition named appstore through the
shared BlockStorage contract. It uses 4 KiB erase sectors, supports byte reads
and programs, and does not expose an XIP mapping. The shared BundleFS remains
responsible for catalog and Bundle updates.

Guest KV uses LittleFS on the MTD partition named media. Initializing KV
mounts the existing volume first. It formats only after a corrupt-volume
result and only when the entire partition reads back as erased. Read errors and
nonblank foreign contents fail initialization without a format. Each value
write syncs a temporary file before renaming it over the previous value.

This layer compiles with both board profiles. The runtime smoke executable
continues to run the WAMR checks without mounting storage. Board partition
layouts and the Host that mounts BundleFS and exposes Guest KV are integrated
with the board firmware. An existing partition table is not migrated here.

## Validation

With BL_SDK_BASE pointing to the pinned SDK from [SDK setup](sdk.md), run:

    MICROPIXEL_HOST_TEST_PLATFORM=bouffalo bash tools/tests/test_firmware_host.sh

The Flash backend test compiles the SDK's LittleFS implementation against a
simulated NOR MTD. It checks byte storage and bounds, Guest KV reads after a
fresh process and remount, and preservation of foreign data and read failures.
The [build steps](../README.md#build) compile the adapters for both boards.
