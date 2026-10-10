# SPDX-License-Identifier: Apache-2.0
"""Remote Control endpoint injection for profile-driven firmware builds."""

from dataclasses import replace
from pathlib import Path
import tempfile
import unittest

from tools import firmware


ENDPOINT = {
    "MICROPIXEL_REMOTE_CONTROL_HOST": "quic.micropixel.ai",
    "MICROPIXEL_REMOTE_CONTROL_PORT": "8443",
}


class RemoteControlEnvTest(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        base = firmware.load_profiles(environ={})["metalio-claw4"]
        self.profile = replace(
            base,
            build_dir=self.root,
            sdkconfig=self.root / "sdkconfig.release",
        )

    def test_without_endpoint_nothing_is_written(self) -> None:
        self.assertIs(
            firmware.apply_remote_control_env(self.profile, environ={}), self.profile
        )
        self.assertEqual(list(self.root.iterdir()), [])

    def test_endpoint_updates_defaults_and_generated_config(self) -> None:
        self.profile.sdkconfig.write_text(
            'CONFIG_MICROPIXEL_REMOTE_CONTROL_HOST=""\n'
            "CONFIG_MICROPIXEL_REMOTE_CONTROL_PORT=9443\n"
            "# CONFIG_MICROPIXEL_REMOTE_CONTROL_ALLOW_UNVERIFIED_TLS is not set\n"
            "CONFIG_UNRELATED=y\n",
            encoding="utf-8",
        )

        updated = firmware.apply_remote_control_env(
            self.profile, environ={**ENDPOINT, firmware.REMOTE_CONTROL_TLS_ENV: "y"}
        )

        defaults_path = self.root / firmware.REMOTE_CONTROL_ENV_DEFAULTS_NAME
        defaults = defaults_path.read_text(encoding="utf-8")
        self.assertIn(
            'CONFIG_MICROPIXEL_REMOTE_CONTROL_HOST="quic.micropixel.ai"', defaults
        )
        self.assertIn("CONFIG_MICROPIXEL_REMOTE_CONTROL_PORT=8443", defaults)
        self.assertIn(
            "CONFIG_MICROPIXEL_REMOTE_CONTROL_ALLOW_UNVERIFIED_TLS=y", defaults
        )

        generated = self.profile.sdkconfig.read_text(encoding="utf-8")
        self.assertIn(
            'CONFIG_MICROPIXEL_REMOTE_CONTROL_HOST="quic.micropixel.ai"', generated
        )
        self.assertIn("CONFIG_MICROPIXEL_REMOTE_CONTROL_PORT=8443", generated)
        self.assertNotIn("is not set", generated)
        self.assertIn("CONFIG_UNRELATED=y", generated)
        self.assertEqual(updated.sdkconfig_defaults[-1], defaults_path.resolve())

        # Rebuilding must stay stable instead of accumulating duplicate entries.
        self.assertEqual(
            firmware.apply_remote_control_env(
                updated, environ={**ENDPOINT, firmware.REMOTE_CONTROL_TLS_ENV: "y"}
            ),
            updated,
        )

    def test_foreign_lines_in_the_environment_file_survive(self) -> None:
        defaults_path = self.root / firmware.REMOTE_CONTROL_ENV_DEFAULTS_NAME
        defaults_path.write_text("CONFIG_LV_MEM_SIZE=1048576\n", encoding="utf-8")

        firmware.apply_remote_control_env(self.profile, environ=dict(ENDPOINT))

        merged = defaults_path.read_text(encoding="utf-8")
        self.assertIn("CONFIG_LV_MEM_SIZE=1048576", merged)
        self.assertIn(
            'CONFIG_MICROPIXEL_REMOTE_CONTROL_HOST="quic.micropixel.ai"', merged
        )

    def test_invalid_values_are_rejected(self) -> None:
        cases = (
            ({"MICROPIXEL_REMOTE_CONTROL_HOST": "bad host"}, "unsupported characters"),
            ({**ENDPOINT, "MICROPIXEL_REMOTE_CONTROL_PORT": "0"}, "between 1 and 65535"),
            (
                {**ENDPOINT, "MICROPIXEL_REMOTE_CONTROL_ALLOW_UNVERIFIED_TLS": "maybe"},
                "must be y or n",
            ),
            (
                {**ENDPOINT, "MICROPIXEL_REMOTE_CONTROL_ALLOW_UNVERIFIED_TLS": "n"},
                "TRUSTED_CA_DER_BASE64",
            ),
        )
        for environ, message in cases:
            with self.subTest(environ=environ):
                with self.assertRaisesRegex(firmware.FirmwareToolError, message):
                    firmware.apply_remote_control_env(self.profile, environ=environ)
        self.assertEqual(list(self.root.iterdir()), [])
