# Create your first app

[简体中文](QUICKSTART.zh-CN.md)

## Setup

Install the [MicroPixel SDK and toolchain](https://micropixel.ai/docs/environment/).
Connect one device running [MicroPixel firmware](https://micropixel.ai/flash/) and close other serial tools.
App development does not require ESP-IDF.

For an extracted SDK on macOS/Linux, run from its root:

```sh
export PATH="$PWD:$PATH"
export WASI_SDK_PATH=/path/to/wasi-sdk
export WAMRC=/absolute/path/to/micropixel-wamrc
python3 -m pip install "pyserial>=3.5,<4"
micropixel --transport usb device status
```

Replace the toolchain paths with your installed locations. WAMRC must be the MicroPixel fork producing AOT v6.
Windows users: see [managed installation](AI.md).

## Create and run

```sh
micropixel init my-app --app-id com.example.my-app --title "My App"
cd my-app
micropixel --transport usb run
```

Choose a unique, stable App ID. The generated `src/main.cpp` displays Hello World.
Edit it and run the same command to rebuild and reinstall. Ctrl-C stops logs without stopping the app;
use `--no-follow` to return immediately after startup.

With multiple devices, specify `--port <port>` before `run`.

## Publish

After testing the app, follow the [publishing guide](PUBLISHING.md):

```sh
micropixel publish --dry-run
micropixel auth github
micropixel publish
```

Open the returned `editUrl` to add a description, instructions, and at least two runtime screenshots.
Store details are edited separately from the Bundle upload.
