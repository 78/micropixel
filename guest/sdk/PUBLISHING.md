# Publish an app

[简体中文](PUBLISHING.zh-CN.md)

Test the app on a device and update `app.json` with its version and actual capability requirements.
Publishing builds both `riscv32-ilp32f` and `xtensa`; configure `WAMRC` and `XTENSA_WAMRC` accordingly.
Bundles must use AOT v6, a single thread, and memory checks, and must not exceed 8 MiB each.

## Upload

```sh
micropixel publish --dry-run
micropixel auth github
micropixel publish
```

`--dry-run` builds and validates without uploading. The first successful publication binds the App ID to your account.
The command returns the public `url` and an `editUrl`; uploading does not install the app on devices.

Optional release metadata:

```sh
micropixel publish --notes-file CHANGELOG.txt --tested-device metalio-claw4
```

Release notes are limited to 3000 UTF-8 bytes. Repeat `--tested-device` only for boards actually tested.

## Store page

Open `editUrl` with the same GitHub account and provide:

| Field | Content |
|---|---|
| Short description | Purpose or game concept, up to 240 characters |
| Description and instructions | Controls, goals, rules, and required hardware; up to 20,000 characters |
| Screenshots | At least two different runtime views; PNG/JPEG, at most 2 MB each, up to eight |
| Cover | Separate from screenshots; the Bundle's `launch_asset` supplies a default |

Capture screenshots with `micropixel --transport usb screenshot --output store/01-main.jpg`.
Change the app's visible state before taking the next screenshot.
The CLI does not upload descriptions or screenshots. Bundle publication is immediately visible;
there is no draft stage or enforced screenshot count.

## Updates

Increment the `major.minor.patch` version before publishing changed Bundles. An identical upload can be retried;
a version/architecture pair cannot be replaced with different content. Another architecture may be added to the same version.
Descriptions and screenshots can be edited without uploading a new Bundle.
Users confirm installation of updates; publishing does not trigger automatic installation.

Developer GitHub authentication is separate from device pairing. Check it with `micropixel auth status`;
use `micropixel auth logout` to revoke local publishing credentials.
