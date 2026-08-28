#!/usr/bin/env python3
"""Profile-driven ESP-IDF build, flash, and monitor entry point."""

from __future__ import annotations

import argparse
import glob
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import Any, Mapping, Sequence


WORKSPACE_ROOT = Path(__file__).resolve().parents[1]
FIRMWARE_DIR = WORKSPACE_ROOT / "firmware" / "espressif"
DEFAULT_PROFILES_PATH = Path(__file__).with_name("firmware_profiles.json")
SERIAL_GLOBS = (
    "/dev/cu.usbmodem*",
    "/dev/cu.usbserial*",
    "/dev/ttyACM*",
    "/dev/ttyUSB*",
)


class FirmwareToolError(RuntimeError):
    """A user-actionable firmware tooling error."""


@dataclass(frozen=True)
class Profile:
    name: str
    description: str
    target: str
    build_dir: Path
    sdkconfig: Path
    sdkconfig_defaults: tuple[Path, ...]
    preview: bool
    flash: bool
    monitor: bool
    chip_markers: tuple[str, ...]
    port_env: str | None
    baud_env: str | None
    default_baud: int | None
    flash_before: str
    application_usb_products: tuple[str, ...]
    rom_usb_products: tuple[str, ...]


@dataclass(frozen=True)
class SerialPortInfo:
    device: str
    product: str
    location: str


def _optional_env_name(raw: Mapping[str, Any], key: str) -> str | None:
    value = raw.get(key)
    if value is None:
        return None
    if not isinstance(value, str) or not re.fullmatch(r"[A-Z][A-Z0-9_]*", value):
        raise FirmwareToolError(f"profile field '{key}' must be an env name")
    return value


def _resolve_path(value: str, workspace_root: Path) -> Path:
    path = Path(value).expanduser()
    if not path.is_absolute():
        path = workspace_root / path
    return path.resolve()


def _env_or_default(
    raw: Mapping[str, Any], key: str, environ: Mapping[str, str]
) -> str:
    env_name = _optional_env_name(raw, f"{key}_env")
    if env_name and environ.get(env_name):
        return environ[env_name]
    value = raw.get(key)
    if not isinstance(value, str) or not value:
        raise FirmwareToolError(f"profile field '{key}' must be a non-empty string")
    return value


def load_profiles(
    profiles_path: Path = DEFAULT_PROFILES_PATH,
    *,
    workspace_root: Path = WORKSPACE_ROOT,
    environ: Mapping[str, str] | None = None,
) -> dict[str, Profile]:
    environ = os.environ if environ is None else environ
    try:
        raw_profiles = json.loads(profiles_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise FirmwareToolError(f"cannot read profile file {profiles_path}: {error}") from error
    if not isinstance(raw_profiles, dict) or not raw_profiles:
        raise FirmwareToolError("firmware profile file must contain a non-empty object")

    profiles: dict[str, Profile] = {}
    for name, raw in raw_profiles.items():
        if not isinstance(name, str) or not re.fullmatch(r"[a-z0-9][a-z0-9-]*", name):
            raise FirmwareToolError(f"invalid profile name: {name!r}")
        if not isinstance(raw, dict):
            raise FirmwareToolError(f"profile '{name}' must be an object")

        target = raw.get("target")
        if not isinstance(target, str) or not re.fullmatch(r"esp32[a-z0-9]+", target):
            raise FirmwareToolError(f"profile '{name}' has invalid ESP-IDF target")
        build_dir = _resolve_path(
            _env_or_default(raw, "build_dir", environ), workspace_root
        )
        sdkconfig_env = _optional_env_name(raw, "sdkconfig_env")
        sdkconfig_value = environ.get(sdkconfig_env, "") if sdkconfig_env else ""
        if sdkconfig_value:
            sdkconfig = _resolve_path(sdkconfig_value, workspace_root)
        else:
            sdkconfig_name = raw.get("sdkconfig_name", "sdkconfig")
            if not isinstance(sdkconfig_name, str) or not sdkconfig_name:
                raise FirmwareToolError(
                    f"profile '{name}' has invalid sdkconfig_name"
                )
            sdkconfig = build_dir / sdkconfig_name

        defaults_env = _optional_env_name(raw, "sdkconfig_defaults_env")
        defaults_value = environ.get(defaults_env, "") if defaults_env else ""
        if defaults_value:
            defaults_raw: Sequence[str] = tuple(
                item for item in defaults_value.split(";") if item
            )
        else:
            defaults_config = raw.get("sdkconfig_defaults")
            if not isinstance(defaults_config, list) or not defaults_config:
                raise FirmwareToolError(
                    f"profile '{name}' needs at least one sdkconfig default"
                )
            defaults_raw = defaults_config
        if not all(isinstance(item, str) and item for item in defaults_raw):
            raise FirmwareToolError(
                f"profile '{name}' has invalid sdkconfig default paths"
            )
        defaults = tuple(
            _resolve_path(item, workspace_root) for item in defaults_raw
        )

        flash = raw.get("flash", False)
        monitor = raw.get("monitor", False)
        preview = raw.get("preview", False)
        if not all(isinstance(value, bool) for value in (flash, monitor, preview)):
            raise FirmwareToolError(
                f"profile '{name}' preview/flash/monitor fields must be booleans"
            )
        markers_raw = raw.get("chip_markers", [])
        if not isinstance(markers_raw, list) or not all(
            isinstance(marker, str) and marker for marker in markers_raw
        ):
            raise FirmwareToolError(f"profile '{name}' has invalid chip_markers")
        chip_markers = tuple(markers_raw)
        if (flash or monitor) and not chip_markers:
            raise FirmwareToolError(
                f"serial-enabled profile '{name}' needs chip_markers"
            )

        default_baud = raw.get("default_baud")
        if default_baud is not None and (
            not isinstance(default_baud, int)
            or isinstance(default_baud, bool)
            or default_baud <= 0
        ):
            raise FirmwareToolError(f"profile '{name}' has invalid default_baud")
        if flash and default_baud is None:
            raise FirmwareToolError(
                f"flashable profile '{name}' needs a default_baud"
            )
        description = raw.get("description", "")
        if not isinstance(description, str):
            raise FirmwareToolError(f"profile '{name}' has invalid description")
        flash_before = raw.get("flash_before", "default-reset")
        if flash_before not in ("default-reset", "no-reset"):
            raise FirmwareToolError(f"profile '{name}' has invalid flash_before")
        products_raw = raw.get("application_usb_products", [])
        if not isinstance(products_raw, list) or not all(
            isinstance(product, str) and product for product in products_raw
        ):
            raise FirmwareToolError(
                f"profile '{name}' has invalid application_usb_products"
            )
        rom_products_raw = raw.get("rom_usb_products", [])
        if not isinstance(rom_products_raw, list) or not all(
            isinstance(product, str) and product for product in rom_products_raw
        ):
            raise FirmwareToolError(f"profile '{name}' has invalid rom_usb_products")
        if products_raw and not rom_products_raw:
            raise FirmwareToolError(
                f"profile '{name}' needs rom_usb_products for USB auto-download"
            )
        profiles[name] = Profile(
            name=name,
            description=description,
            target=target,
            build_dir=build_dir,
            sdkconfig=sdkconfig.resolve(),
            sdkconfig_defaults=defaults,
            preview=preview,
            flash=flash,
            monitor=monitor,
            chip_markers=chip_markers,
            port_env=_optional_env_name(raw, "port_env"),
            baud_env=_optional_env_name(raw, "baud_env"),
            default_baud=default_baud,
            flash_before=flash_before,
            application_usb_products=tuple(products_raw),
            rom_usb_products=tuple(rom_products_raw),
        )
    return profiles


def with_overrides(profile: Profile, args: argparse.Namespace) -> Profile:
    build_dir = (
        _resolve_path(args.build_dir, WORKSPACE_ROOT)
        if args.build_dir
        else profile.build_dir
    )
    if args.sdkconfig:
        sdkconfig = _resolve_path(args.sdkconfig, WORKSPACE_ROOT)
    elif args.build_dir and profile.sdkconfig.parent == profile.build_dir:
        sdkconfig = build_dir / profile.sdkconfig.name
    else:
        sdkconfig = profile.sdkconfig
    if args.sdkconfig_defaults:
        defaults = tuple(
            _resolve_path(item, WORKSPACE_ROOT)
            for item in args.sdkconfig_defaults.split(";")
            if item
        )
        if not defaults:
            raise FirmwareToolError("--sdkconfig-defaults cannot be empty")
    else:
        defaults = profile.sdkconfig_defaults
    return Profile(
        name=profile.name,
        description=profile.description,
        target=profile.target,
        build_dir=build_dir,
        sdkconfig=sdkconfig,
        sdkconfig_defaults=defaults,
        preview=profile.preview,
        flash=profile.flash,
        monitor=profile.monitor,
        chip_markers=profile.chip_markers,
        port_env=profile.port_env,
        baud_env=profile.baud_env,
        default_baud=profile.default_baud,
        flash_before=profile.flash_before,
        application_usb_products=profile.application_usb_products,
        rom_usb_products=profile.rom_usb_products,
    )


def locate_idf_py(environ: Mapping[str, str] | None = None) -> Path:
    environ = os.environ if environ is None else environ
    executable = shutil.which("idf.py")
    if not executable:
        raise FirmwareToolError(
            "idf.py is unavailable; activate ESP-IDF or use the board shell wrapper"
        )
    idf_path = environ.get("IDF_PATH")
    if idf_path:
        expected = (Path(idf_path) / "tools" / "idf.py").resolve()
        actual = Path(executable).resolve()
        if actual != expected:
            raise FirmwareToolError(
                f"idf.py comes from {actual}, not the active IDF_PATH {expected}"
            )
    return Path(executable).resolve()


def idf_command(
    profile: Profile, idf_py: Path, idf_arguments: Sequence[str]
) -> list[str]:
    command = [str(idf_py)]
    if profile.preview:
        command.append("--preview")
    command.extend(
        (
            "-C",
            str(FIRMWARE_DIR),
            "-B",
            str(profile.build_dir),
            "-D",
            f"SDKCONFIG={profile.sdkconfig}",
            "-D",
            "SDKCONFIG_DEFAULTS="
            + ";".join(str(path) for path in profile.sdkconfig_defaults),
            "-D",
            f"IDF_TARGET={profile.target}",
        )
    )
    command.extend(idf_arguments)
    return command


def chip_matches(profile: Profile, probe_output: str) -> bool:
    folded_output = probe_output.casefold()
    return any(marker.casefold() in folded_output for marker in profile.chip_markers)


def _reset_after_mismatched_probe(port: str) -> None:
    try:
        subprocess.run(
            [
                sys.executable,
                "-m",
                "esptool",
                "--port",
                port,
                "--before",
                "no-reset",
                "--after",
                "hard-reset",
                "run",
            ],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired):
        pass


def probe_port(
    profile: Profile, port: str, *, before: str = "default-reset"
) -> tuple[bool, str]:
    try:
        result = subprocess.run(
            [
                sys.executable,
                "-m",
                "esptool",
                "--port",
                port,
                "--before",
                before,
                "--after",
                "no-reset",
                "chip-id",
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=20,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        return False, str(error)
    matched = chip_matches(profile, result.stdout)
    if not matched and "esp32" in result.stdout.casefold():
        _reset_after_mismatched_probe(port)
    return matched, result.stdout


def serial_port_infos() -> list[SerialPortInfo]:
    try:
        from serial.tools import list_ports  # type: ignore[import-not-found]

        return [
            SerialPortInfo(
                device=port.device,
                product=port.product or "",
                location=port.location or "",
            )
            for port in list_ports.comports()
        ]
    except ImportError:
        return []


def candidate_ports() -> list[str]:
    candidates = {port.device for port in serial_port_infos()}
    for pattern in SERIAL_GLOBS:
        candidates.update(glob.glob(pattern))
    return sorted(candidates)


def _serial_port_info(port: str) -> SerialPortInfo | None:
    aliases = {port}
    # macOS exposes the same USB serial interface through paired callout (cu)
    # and terminal (tty) device nodes. Environment files often retain the tty
    # spelling while pyserial enumerates the cu spelling; preserve the selected
    # node for I/O but use either alias for USB identity matching.
    for source, destination in (("/dev/tty.", "/dev/cu."), ("/dev/cu.", "/dev/tty.")):
        if port.startswith(source):
            aliases.add(destination + port[len(source) :])
            break
    return next((info for info in serial_port_infos() if info.device in aliases), None)


def _port_has_product(port: str, expected_products: Sequence[str]) -> bool:
    info = _serial_port_info(port)
    if info is None:
        return False
    product = info.product.casefold()
    return any(
        product == expected.casefold()
        for expected in expected_products
    )


def _is_application_port(profile: Profile, port: str) -> bool:
    return _port_has_product(port, profile.application_usb_products)


def _is_rom_port(profile: Profile, port: str) -> bool:
    return _port_has_product(port, profile.rom_usb_products)


def _validate_requested_port(port: str) -> None:
    if os.name == "nt" and re.fullmatch(r"COM\d+", port, re.IGNORECASE):
        return
    if not Path(port).exists():
        raise FirmwareToolError(f"serial port not found: {port}")


def resolve_port(
    profile: Profile,
    requested: str | None,
    environ: Mapping[str, str] | None = None,
    *,
    probe_before: str = "default-reset",
) -> str:
    if not profile.chip_markers:
        raise FirmwareToolError(f"profile '{profile.name}' has no physical serial target")
    environ = os.environ if environ is None else environ
    selected = requested or (
        environ.get(profile.port_env, "") if profile.port_env else ""
    )
    if selected:
        _validate_requested_port(selected)
        if _is_application_port(profile, selected) or _is_rom_port(
            profile, selected
        ):
            return selected
        matched, output = probe_port(profile, selected, before=probe_before)
        if not matched:
            detail = output.strip() or "no response"
            raise FirmwareToolError(
                f"{selected} is not the {profile.name} target:\n{detail}"
            )
        return selected

    candidates = candidate_ports()
    port_hint = f" or set {profile.port_env}" if profile.port_env else ""
    if not candidates:
        raise FirmwareToolError(
            f"no connected {profile.target} device matched profile '{profile.name}'; "
            f"pass --port{port_hint}"
        )
    if len(candidates) > 1:
        joined = "\n  ".join(candidates)
        raise FirmwareToolError(
            "multiple serial devices are connected; pass --port so the selected "
            f"device can be chip-verified:\n  {joined}"
        )
    candidate = candidates[0]
    if _is_application_port(profile, candidate) or _is_rom_port(
        profile, candidate
    ):
        return candidate
    matched, output = probe_port(profile, candidate, before=probe_before)
    if not matched:
        detail = output.strip() or "no response"
        raise FirmwareToolError(
            f"{candidate} is not the {profile.name} target:\n{detail}"
        )
    return candidate


def _request_usb_auto_download(port: str) -> None:
    try:
        import serial  # type: ignore[import-not-found]

        with serial.Serial(port, 115200, timeout=0.2) as connection:
            connection.dtr = False
            connection.rts = True
            time.sleep(0.1)
            connection.dtr = True
            connection.rts = False
            time.sleep(0.2)
    except (ImportError, OSError) as error:
        raise FirmwareToolError(
            f"cannot request USB automatic download on {port}: {error}"
        ) from error


def _wait_for_rom_port(
    profile: Profile,
    application_port: str,
    location: str,
    *,
    timeout_seconds: float = 8.0,
) -> str:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        for info in serial_port_infos():
            if location and info.location != location:
                continue
            if info.product.casefold() in {
                product.casefold() for product in profile.rom_usb_products
            }:
                return info.device
        time.sleep(0.05)
    raise FirmwareToolError(
        "USB automatic download was requested, but the matching ROM port "
        f"did not appear within {timeout_seconds:g}s"
    )


def resolve_flash_port(
    profile: Profile,
    requested: str | None,
    environ: Mapping[str, str] | None = None,
) -> str:
    port = resolve_port(
        profile,
        requested,
        environ,
        probe_before=profile.flash_before,
    )
    if not _is_application_port(profile, port):
        return port

    info = _serial_port_info(port)
    location = info.location if info else ""
    print(f"==> Requesting USB automatic download on {port}", flush=True)
    _request_usb_auto_download(port)
    rom_port = _wait_for_rom_port(profile, port, location)
    print(f"==> ROM port ready: {rom_port}", flush=True)
    return rom_port


def resolve_monitor_port(
    profile: Profile,
    requested: str | None,
    environ: Mapping[str, str] | None = None,
) -> str:
    environ = os.environ if environ is None else environ
    selected = requested or (
        environ.get(profile.port_env, "") if profile.port_env else ""
    )
    if selected:
        _validate_requested_port(selected)
        return selected
    candidates = candidate_ports()
    if len(candidates) == 1:
        return candidates[0]
    if not candidates:
        raise FirmwareToolError("no serial device is connected; pass --port")
    joined = "\n  ".join(candidates)
    raise FirmwareToolError(f"multiple serial devices are connected; pass --port:\n  {joined}")


def resolve_baud(
    profile: Profile,
    requested: int | None,
    environ: Mapping[str, str] | None = None,
) -> int:
    if requested is not None:
        if requested <= 0:
            raise FirmwareToolError("--baud must be a positive integer")
        return requested
    environ = os.environ if environ is None else environ
    raw = environ.get(profile.baud_env, "") if profile.baud_env else ""
    if raw:
        try:
            baud = int(raw)
        except ValueError as error:
            raise FirmwareToolError(
                f"{profile.baud_env} must be a positive integer: {raw}"
            ) from error
        if baud <= 0:
            raise FirmwareToolError(
                f"{profile.baud_env} must be a positive integer: {raw}"
            )
        return baud
    if profile.default_baud is None:
        raise FirmwareToolError(f"profile '{profile.name}' has no default baud rate")
    return profile.default_baud


def ensure_action_allowed(profile: Profile, action: str) -> None:
    if action in ("flash", "flash-built") and not profile.flash:
        raise FirmwareToolError(
            f"profile '{profile.name}' is compile-only and cannot be flashed"
        )
    if action == "monitor" and not profile.monitor:
        raise FirmwareToolError(
            f"profile '{profile.name}' does not expose a hardware monitor"
        )


def validate_profile_files(profile: Profile) -> None:
    missing = [path for path in profile.sdkconfig_defaults if not path.is_file()]
    if missing:
        joined = "\n  ".join(str(path) for path in missing)
        raise FirmwareToolError(
            f"profile '{profile.name}' has missing sdkconfig defaults:\n  {joined}"
        )


def run_idf(command: Sequence[str], *, environ: Mapping[str, str] | None = None) -> None:
    print("==>", " ".join(command), flush=True)
    completed = subprocess.run(command, env=dict(environ) if environ else None, check=False)
    if completed.returncode:
        raise FirmwareToolError(f"ESP-IDF command failed with exit code {completed.returncode}")


def _load_flash_manifest(profile: Profile) -> tuple[list[str], list[tuple[str, str]], str, bool]:
    manifest_path = profile.build_dir / "flasher_args.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        write_args = manifest["write_flash_args"]
        flash_files = manifest["flash_files"]
        extra_args = manifest["extra_esptool_args"]
        after = extra_args["after"]
        use_stub = extra_args["stub"]
    except (OSError, json.JSONDecodeError, KeyError, TypeError) as error:
        raise FirmwareToolError(
            f"cannot read ESP-IDF flash manifest {manifest_path}: {error}"
        ) from error
    if not isinstance(write_args, list) or not all(
        isinstance(value, str) for value in write_args
    ):
        raise FirmwareToolError(f"invalid write_flash_args in {manifest_path}")
    if not isinstance(flash_files, dict) or not all(
        isinstance(offset, str) and isinstance(path, str)
        for offset, path in flash_files.items()
    ):
        raise FirmwareToolError(f"invalid flash_files in {manifest_path}")
    if not isinstance(after, str) or not isinstance(use_stub, bool):
        raise FirmwareToolError(f"invalid extra_esptool_args in {manifest_path}")
    return write_args, list(flash_files.items()), after, use_stub


def _flashed_reference(path: str) -> str:
    return str(Path(path).with_name(f"{Path(path).stem}_flashed.bin"))


def run_esptool_flash(profile: Profile, port: str, baud: int) -> None:
    write_args, flash_files, after, use_stub = _load_flash_manifest(profile)
    command = [
        sys.executable,
        "-m",
        "esptool",
        "--chip",
        profile.target,
        "--port",
        port,
        "--baud",
        str(baud),
        "--before",
        profile.flash_before,
        "--after",
        after,
    ]
    if not use_stub:
        command.append("--no-stub")
    command.extend(("write-flash", *write_args))
    for offset, path in flash_files:
        command.extend((offset, path))

    full_flash = os.environ.get("IDF_FLASH_FULL") == "1"
    if not full_flash:
        references = [_flashed_reference(path) for _, path in flash_files]
        if any((profile.build_dir / path).is_file() for path in references):
            command.append("--diff-with")
            command.extend(
                path if (profile.build_dir / path).is_file() else "skip"
                for path in references
            )
            if os.environ.get("IDF_TRUST_FLASH_CONTENT") == "1":
                command.append("--trust-flash-content")
        else:
            command.append("--skip-flashed")

    print("==>", " ".join(command), flush=True)
    completed = subprocess.run(command, cwd=profile.build_dir, check=False)
    if completed.returncode:
        raise FirmwareToolError(
            f"esptool flash failed with exit code {completed.returncode}"
        )
    for _, path in flash_files:
        source = profile.build_dir / path
        if source.is_file():
            shutil.copyfile(source, profile.build_dir / _flashed_reference(path))


def _positive_int(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be a positive integer") from error
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return parsed


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Profile-driven ESP-IDF Host build/flash helper",
        epilog=(
            "Examples: firmware.py metalio-claw4 build; "
            "firmware.py metalio-claw4 flash --port /dev/cu.usbmodemXXXX"
        ),
    )
    parser.add_argument("profile", help="profile name, or 'list'")
    parser.add_argument(
        "action",
        nargs="?",
        choices=("build", "flash", "flash-built", "monitor", "fullclean", "port"),
    )
    parser.add_argument("--port", help="serial port; otherwise use the profile env or auto-detect")
    parser.add_argument("--baud", type=_positive_int, help="flash baud rate")
    parser.add_argument(
        "--reset",
        action="store_true",
        help="reset the application when monitor starts (monitor only)",
    )
    parser.add_argument("--build-dir", help="override the profile build directory")
    parser.add_argument("--sdkconfig", help="override the generated sdkconfig path")
    parser.add_argument(
        "--sdkconfig-defaults",
        help="semicolon-separated sdkconfig defaults override",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    try:
        args = _parser().parse_args(argv)
        profiles = load_profiles()
        if args.profile == "list":
            if args.action is not None:
                raise FirmwareToolError("the list command does not accept an action")
            for profile in profiles.values():
                capabilities = ["build"]
                if profile.flash:
                    capabilities.append("flash")
                if profile.monitor:
                    capabilities.append("monitor")
                print(
                    f"{profile.name:16} {profile.target:10} "
                    f"{','.join(capabilities):20} {profile.description}"
                )
            return 0
        if args.action is None:
            raise FirmwareToolError("an action is required")
        if args.reset and args.action != "monitor":
            raise FirmwareToolError("--reset is only valid with the monitor action")
        if args.profile not in profiles:
            available = ", ".join(profiles)
            raise FirmwareToolError(
                f"unknown profile '{args.profile}'; available profiles: {available}"
            )
        profile = with_overrides(profiles[args.profile], args)
        ensure_action_allowed(profile, args.action)

        if args.action == "port":
            print(resolve_port(profile, args.port))
            return 0

        validate_profile_files(profile)
        idf_py = locate_idf_py()
        if args.action == "build":
            profile.build_dir.mkdir(parents=True, exist_ok=True)
            run_idf(idf_command(profile, idf_py, ("build",)))
            return 0
        if args.action == "fullclean":
            run_idf(idf_command(profile, idf_py, ("fullclean",)))
            return 0

        if args.action == "flash":
            profile.build_dir.mkdir(parents=True, exist_ok=True)
            run_idf(idf_command(profile, idf_py, ("build",)))
        if args.action in ("flash", "flash-built"):
            if args.action == "flash-built" and not (profile.build_dir / "micropixel.elf").is_file():
                raise FirmwareToolError(
                    f"Host ELF missing: {profile.build_dir / 'micropixel.elf'}; build the profile first"
                )
            port = resolve_flash_port(profile, args.port)
            baud = resolve_baud(profile, args.baud)
            run_esptool_flash(profile, port, baud)
            return 0

        elf = profile.build_dir / "micropixel.elf"
        if not elf.is_file():
            raise FirmwareToolError(f"Host ELF missing: {elf}; build the profile first")
        port = resolve_monitor_port(profile, args.port)
        monitor_environment = os.environ.copy()
        if args.reset:
            monitor_environment.pop("ESP_IDF_MONITOR_NO_RESET", None)
        else:
            monitor_environment["ESP_IDF_MONITOR_NO_RESET"] = "1"
        run_idf(
            idf_command(profile, idf_py, ("-p", port, "monitor")),
            environ=monitor_environment,
        )
        return 0
    except FirmwareToolError as error:
        print(f"firmware.py: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
