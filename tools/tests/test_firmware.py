import argparse
import contextlib
import fcntl
from dataclasses import replace
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from tools import firmware


class FirmwareProfileTest(unittest.TestCase):
    def setUp(self) -> None:
        self.profiles = firmware.load_profiles(environ={})

    def test_expected_board_profiles_are_declared(self) -> None:
        self.assertEqual(
            set(self.profiles),
            {
                "metalio-claw4",
                "p4-null",
                "esp-mosaico",
                "s31-null",
                "s3-null",
                "esp-box-3",
                "szpi-esp32s3",
                "m5stack-cores3",
            },
        )
        self.assertTrue(self.profiles["metalio-claw4"].flash)
        self.assertTrue(self.profiles["esp-mosaico"].flash)
        self.assertTrue(self.profiles["esp-mosaico"].monitor)
        self.assertEqual(self.profiles["esp-mosaico"].flash_before, "no-reset")
        self.assertEqual(
            self.profiles["esp-mosaico"].application_usb_products,
            ("MicroPixel ESP-Mosaico",),
        )
        self.assertEqual(
            self.profiles["esp-mosaico"].rom_usb_products,
            ("ESP32-S31",),
        )
        self.assertFalse(self.profiles["p4-null"].monitor)
        self.assertTrue(self.profiles["esp-box-3"].flash)
        self.assertTrue(self.profiles["esp-box-3"].monitor)
        self.assertTrue(self.profiles["szpi-esp32s3"].flash)
        self.assertTrue(self.profiles["szpi-esp32s3"].monitor)
        self.assertTrue(self.profiles["m5stack-cores3"].flash)
        self.assertTrue(self.profiles["m5stack-cores3"].monitor)

    def test_p4_command_uses_non_preview_target_and_defaults(self) -> None:
        profile = self.profiles["metalio-claw4"]
        command = firmware.idf_command(profile, Path("/idf/idf.py"), ("build",))
        self.assertNotIn("--preview", command)
        self.assertIn("IDF_TARGET=esp32p4", command)
        defaults = next(item for item in command if item.startswith("SDKCONFIG_DEFAULTS="))
        self.assertIn("sdkconfig.p4.defaults", defaults)
        self.assertEqual(command[-1], "build")

    def test_every_profile_layers_shared_defaults_first(self) -> None:
        shared_defaults = firmware.FIRMWARE_DIR / "sdkconfig.defaults"
        shared_config = {
            line.split("=", 1)[0]: line.split("=", 1)[1]
            for line in shared_defaults.read_text(encoding="utf-8").splitlines()
            if line.startswith("CONFIG_") and "=" in line
        }
        shared_keys = set(shared_config)
        shared_keys.update(
            line.removeprefix("# ").removesuffix(" is not set")
            for line in shared_defaults.read_text(encoding="utf-8").splitlines()
            if line.startswith("# CONFIG_") and line.endswith(" is not set")
        )
        self.assertEqual(
            shared_config["CONFIG_FREERTOS_MAX_TASK_NAME_LEN"],
            "32",
        )
        self.assertEqual(shared_config["CONFIG_WAMR_AOT_CODE_IN_PSRAM"], "y")
        self.assertEqual(shared_config["CONFIG_MICROPIXEL_MAX_BITMAPS"], "128")
        self.assertEqual(
            shared_config["CONFIG_OPUS_NONTHREADSAFE_PSEUDOSTACK"],
            "y",
        )
        for name, profile in self.profiles.items():
            with self.subTest(profile=name):
                self.assertEqual(profile.sdkconfig_defaults[0], shared_defaults)

        for target_defaults in (
            "sdkconfig.p4.defaults",
            "sdkconfig.s31.defaults",
            "sdkconfig.s3.defaults",
        ):
            target_keys = {
                line.split("=", 1)[0]
                for line in (firmware.FIRMWARE_DIR / target_defaults)
                .read_text(encoding="utf-8")
                .splitlines()
                if line.startswith("CONFIG_") and "=" in line
            }
            target_keys.update(
                line.removeprefix("# ").removesuffix(" is not set")
                for line in (firmware.FIRMWARE_DIR / target_defaults)
                .read_text(encoding="utf-8")
                .splitlines()
                if line.startswith("# CONFIG_") and line.endswith(" is not set")
            )
            self.assertTrue(shared_keys.isdisjoint(target_keys))

    def test_s31_command_uses_preview_and_composed_null_defaults(self) -> None:
        profile = self.profiles["s31-null"]
        command = firmware.idf_command(profile, Path("/idf/idf.py"), ("build",))
        self.assertEqual(command[1], "--preview")
        self.assertIn("IDF_TARGET=esp32s31", command)
        defaults = next(item for item in command if item.startswith("SDKCONFIG_DEFAULTS="))
        self.assertIn("sdkconfig.s31.defaults", defaults)
        self.assertIn("sdkconfig.s31-null.defaults", defaults)

    def test_s3_profiles_use_xtensa_target_and_preview_defaults(self) -> None:
        compile_profile = self.profiles["s3-null"]
        compile_command = firmware.idf_command(
            compile_profile, Path("/idf/idf.py"), ("build",)
        )
        self.assertNotIn("--preview", compile_command)
        self.assertIn("IDF_TARGET=esp32s3", compile_command)
        compile_defaults = next(
            item for item in compile_command if item.startswith("SDKCONFIG_DEFAULTS=")
        )
        self.assertIn("sdkconfig.s3-null.defaults", compile_defaults)

        preview_defaults = ";".join(
            str(path) for path in self.profiles["esp-box-3"].sdkconfig_defaults
        )
        self.assertIn("sdkconfig.s3-box-3.defaults", preview_defaults)
        self.assertNotIn("sdkconfig.s3-null.defaults", preview_defaults)
        szpi_defaults = ";".join(
            str(path) for path in self.profiles["szpi-esp32s3"].sdkconfig_defaults
        )
        self.assertIn("sdkconfig.s3-szpi.defaults", szpi_defaults)
        self.assertNotIn("sdkconfig.s3-null.defaults", szpi_defaults)
        cores3_defaults = ";".join(
            str(path) for path in self.profiles["m5stack-cores3"].sdkconfig_defaults
        )
        self.assertIn("sdkconfig.s3-cores3.defaults", cores3_defaults)
        self.assertNotIn("sdkconfig.s3-null.defaults", cores3_defaults)

    def test_environment_can_override_profile_paths(self) -> None:
        profile = firmware.load_profiles(
            environ={
                "P4_HOST_BUILD_DIR": "/tmp/micropixel-p4",
                "P4_SDKCONFIG_DEFAULTS": "/tmp/base;/tmp/board",
            }
        )["metalio-claw4"]
        self.assertEqual(profile.build_dir, Path("/tmp/micropixel-p4").resolve())
        self.assertEqual(
            profile.sdkconfig, Path("/tmp/micropixel-p4/sdkconfig.release").resolve()
        )
        self.assertEqual(
            profile.sdkconfig_defaults,
            (Path("/tmp/base").resolve(), Path("/tmp/board").resolve()),
        )

    def test_build_dir_cli_override_moves_default_sdkconfig(self) -> None:
        args = argparse.Namespace(
            build_dir="build/alternate",
            sdkconfig=None,
            sdkconfig_defaults=None,
        )
        profile = firmware.with_overrides(self.profiles["metalio-claw4"], args)
        self.assertEqual(
            profile.sdkconfig,
            firmware.WORKSPACE_ROOT / "build" / "alternate" / "sdkconfig.release",
        )

    def test_compile_only_profiles_reject_flash_and_monitor(self) -> None:
        for name in ("p4-null", "s31-null", "s3-null"):
            with self.subTest(profile=name):
                with self.assertRaises(firmware.FirmwareToolError):
                    firmware.ensure_action_allowed(self.profiles[name], "flash")
                with self.assertRaises(firmware.FirmwareToolError):
                    firmware.ensure_action_allowed(self.profiles[name], "monitor")

    def test_chip_probe_matches_only_declared_soc(self) -> None:
        profile = self.profiles["metalio-claw4"]
        self.assertTrue(firmware.chip_matches(profile, "Chip is ESP32-P4 (revision v1.3)"))
        self.assertFalse(firmware.chip_matches(profile, "Chip is ESP32-S31"))

    def test_explicit_port_is_still_chip_verified(self) -> None:
        profile = self.profiles["metalio-claw4"]
        with tempfile.NamedTemporaryFile() as serial_port:
            with mock.patch.object(
                firmware, "probe_port", return_value=(False, "Chip is ESP32-S31")
            ):
                with self.assertRaisesRegex(
                    firmware.FirmwareToolError, "not the metalio-claw4 target"
                ):
                    firmware.resolve_port(profile, serial_port.name, environ={})

    def test_known_application_cdc_port_does_not_run_destructive_probe(self) -> None:
        profile = self.profiles["esp-mosaico"]
        with tempfile.NamedTemporaryFile() as serial_port:
            with (
                mock.patch.object(firmware, "_is_application_port", return_value=True),
                mock.patch.object(firmware, "probe_port") as probe,
            ):
                selected = firmware.resolve_port(
                    profile, serial_port.name, environ={}
                )
        self.assertEqual(selected, serial_port.name)
        probe.assert_not_called()

    def test_successful_probe_follows_usb_node_at_same_location(self) -> None:
        profile = self.profiles["esp-box-3"]
        with tempfile.NamedTemporaryFile() as serial_port:
            info = firmware.SerialPortInfo(
                device=serial_port.name,
                product="USB JTAG/serial debug unit",
                location="usb-1",
            )
            with (
                mock.patch.object(firmware, "_serial_port_info", return_value=info),
                mock.patch.object(firmware, "_is_application_port", return_value=False),
                mock.patch.object(firmware, "_is_rom_port", return_value=False),
                mock.patch.object(
                    firmware, "probe_port", return_value=(True, "Chip is ESP32-S3")
                ),
                mock.patch.object(
                    firmware,
                    "_settle_probed_port",
                    return_value="/dev/cu.usbmodem-new",
                ) as settle,
            ):
                selected = firmware.resolve_port(
                    profile, serial_port.name, environ={}
                )
        self.assertEqual(selected, "/dev/cu.usbmodem-new")
        settle.assert_called_once_with(serial_port.name, "usb-1")

    def test_macos_tty_profile_port_matches_enumerated_cu_usb_identity(self) -> None:
        info = firmware.SerialPortInfo(
            device="/dev/cu.usbmodem1234",
            product="MicroPixel ESP-Mosaico",
            location="usb-1",
        )
        with mock.patch.object(firmware, "serial_port_infos", return_value=[info]):
            matched = firmware._serial_port_info("/dev/tty.usbmodem1234")
            is_application = firmware._is_application_port(
                self.profiles["esp-mosaico"], "/dev/tty.usbmodem1234"
            )
        self.assertEqual(matched, info)
        self.assertTrue(is_application)

    def test_flash_port_hands_application_cdc_over_to_rom(self) -> None:
        profile = self.profiles["esp-mosaico"]
        application_port = "/dev/application"
        rom_port = "/dev/rom"
        info = firmware.SerialPortInfo(
            device=application_port,
            product="MicroPixel ESP-Mosaico",
            location="usb-1",
        )
        with (
            mock.patch.object(
                firmware, "resolve_port", return_value=application_port
            ) as resolve,
            mock.patch.object(
                firmware, "_is_application_port", return_value=True
            ),
            mock.patch.object(firmware, "_serial_port_info", return_value=info),
            mock.patch.object(firmware, "_request_usb_auto_download") as request,
            mock.patch.object(
                firmware, "_wait_for_rom_port", return_value=rom_port
            ) as wait,
        ):
            selected = firmware.resolve_flash_port(profile, application_port, {})
        self.assertEqual(selected, rom_port)
        resolve.assert_called_once_with(
            profile, application_port, {}, probe_before="no-reset"
        )
        request.assert_called_once_with(application_port)
        wait.assert_called_once_with(profile, application_port, "usb-1")

    def test_probe_can_preserve_one_shot_rom_download_state(self) -> None:
        profile = self.profiles["esp-mosaico"]
        result = mock.Mock(returncode=0, stdout="Chip is ESP32-S31")
        with mock.patch.object(firmware.subprocess, "run", return_value=result) as run:
            matched, _ = firmware.probe_port(
                profile, "/dev/rom", before="no-reset"
            )
        self.assertTrue(matched)
        command = run.call_args.args[0]
        self.assertEqual(command[command.index("--before") + 1], "no-reset")

    def test_rom_handoff_uses_usb_identity_without_opening_port(self) -> None:
        profile = self.profiles["esp-mosaico"]
        rom_info = firmware.SerialPortInfo(
            device="/dev/rom", product="ESP32-S31", location="usb-1"
        )
        with (
            mock.patch.object(
                firmware, "serial_port_infos", return_value=[rom_info]
            ),
            mock.patch.object(firmware, "probe_port") as probe,
        ):
            selected = firmware._wait_for_rom_port(
                profile, "/dev/application", "usb-1", timeout_seconds=0.1
            )
        self.assertEqual(selected, "/dev/rom")
        probe.assert_not_called()

    def test_direct_flash_uses_profile_reset_mode_and_fast_reflash(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            build_dir = Path(directory)
            (build_dir / "bootloader").mkdir()
            (build_dir / "bootloader" / "bootloader.bin").write_bytes(b"new")
            (build_dir / "bootloader" / "bootloader_flashed.bin").write_bytes(
                b"old"
            )
            (build_dir / "flasher_args.json").write_text(
                json.dumps(
                    {
                        "write_flash_args": ["--flash-size", "16MB"],
                        "flash_files": {
                            "0x2000": "bootloader/bootloader.bin"
                        },
                        "extra_esptool_args": {
                            "after": "hard-reset",
                            "stub": False,
                        },
                    }
                ),
                encoding="utf-8",
            )
            profile = replace(
                self.profiles["esp-mosaico"], build_dir=build_dir
            )
            result = mock.Mock(returncode=0)
            with mock.patch.object(
                firmware.subprocess, "run", return_value=result
            ) as run:
                firmware.run_esptool_flash(profile, "/dev/rom", 460800)
        command = run.call_args.args[0]
        self.assertEqual(command[command.index("--before") + 1], "no-reset")
        self.assertIn("--diff-with", command)
        self.assertIn("bootloader/bootloader_flashed.bin", command)

    def test_monitor_does_not_reset_or_probe_an_explicit_port(self) -> None:
        profile = self.profiles["metalio-claw4"]
        with tempfile.NamedTemporaryFile() as serial_port:
            with mock.patch.object(firmware, "probe_port") as probe:
                selected = firmware.resolve_monitor_port(
                    profile, serial_port.name, environ={}
                )
        self.assertEqual(selected, serial_port.name)
        probe.assert_not_called()

    def test_monitor_reset_is_explicit(self) -> None:
        default_args = firmware._parser().parse_args(["esp-mosaico", "monitor"])
        reset_args = firmware._parser().parse_args(
            ["esp-mosaico", "monitor", "--reset"]
        )
        self.assertFalse(default_args.reset)
        self.assertTrue(reset_args.reset)

    def test_flash_built_is_a_distinct_no_build_action(self) -> None:
        args = firmware._parser().parse_args(["esp-mosaico", "flash-built"])
        self.assertEqual(args.action, "flash-built")
        firmware.ensure_action_allowed(self.profiles["esp-mosaico"], args.action)
        with self.assertRaises(firmware.FirmwareToolError):
            firmware.ensure_action_allowed(self.profiles["s31-null"], args.action)

    def test_flash_baud_must_be_positive(self) -> None:
        with self.assertRaisesRegex(firmware.FirmwareToolError, "positive integer"):
            firmware.resolve_baud(self.profiles["metalio-claw4"], 0, environ={})

    def test_profile_listing_needs_no_esp_idf_environment(self) -> None:
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            result = firmware.main(["list"])
        self.assertEqual(result, 0)
        self.assertIn("metalio-claw4", output.getvalue())
        self.assertIn("esp-mosaico", output.getvalue())

    def test_shared_idf_lock_serializes_managed_component_mutations(self) -> None:
        profile = self.profiles["esp-mosaico"]
        with tempfile.TemporaryDirectory() as directory:
            lock_path = Path(directory) / "firmware.lock"
            with firmware.shared_idf_lock(
                profile, "build", lock_path=lock_path
            ):
                self.assertIn("profile=esp-mosaico", lock_path.read_text())
                with lock_path.open("a+") as contender:
                    with self.assertRaises(BlockingIOError):
                        fcntl.flock(
                            contender.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB
                        )
            with lock_path.open("a+") as contender:
                fcntl.flock(contender.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                fcntl.flock(contender.fileno(), fcntl.LOCK_UN)


if __name__ == "__main__":
    unittest.main()
