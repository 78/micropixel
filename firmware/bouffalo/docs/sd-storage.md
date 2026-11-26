# SD application storage

BL616CL and BL618DG expose a 64 MiB file on a FAT or exFAT card as a
BundleFS block device. Both use one-bit SDH. The SDK configuration and
timeout patch are described in [SDK setup](sdk.md).

An owner opens an existing image at `/sd/micropixel/apps.bfs` and checks
the BundleFS catalog before use. A first-time create opens
`/sd/micropixel/apps.new` exclusively. The owner formats and validates that
pending image before calling `Publish()`, which syncs, closes and renames it.
An existing pending file is preserved on ordinary initialization.
Explicit recovery can reopen a pending image of the expected size.

Place the backend object in internal SRAM for its aligned DMA buffer. A single
owner serializes mount, image lifecycle and ejection. Close the image before
releasing the filesystem. An I/O error makes the backend unavailable until it
is closed and reopened. The application launcher will integrate this lifecycle
in a later change.
