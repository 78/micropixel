# SPDX-License-Identifier: Apache-2.0
"""Remote Control endpoint injection for profile-driven firmware builds."""

from dataclasses import replace
from pathlib import Path
import subprocess
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

    def test_without_overrides_uses_public_endpoint_and_existing_tls_policy(self) -> None:
        updated = firmware.apply_remote_control_env(self.profile, environ={})
        defaults_path = self.root / firmware.REMOTE_CONTROL_ENV_DEFAULTS_NAME
        defaults = defaults_path.read_text(encoding="utf-8")
        self.assertIn('CONFIG_MICROPIXEL_REMOTE_CONTROL_HOST="quic.micropixel.ai"', defaults)
        self.assertIn("CONFIG_MICROPIXEL_REMOTE_CONTROL_PORT=443\n", defaults)
        self.assertIn("CONFIG_MICROPIXEL_REMOTE_CONTROL_ALLOW_UNVERIFIED_TLS=y\n", defaults)
        self.assertIn('CONFIG_MICROPIXEL_REMOTE_CONTROL_TRUSTED_CA_DER_BASE64=""', defaults)
        self.assertIn(defaults_path.resolve(), updated.sdkconfig_defaults)

    def test_defaults_upgrade_an_existing_empty_endpoint(self) -> None:
        self.profile.sdkconfig.write_text(
            'CONFIG_MICROPIXEL_REMOTE_CONTROL_HOST=""\n'
            "CONFIG_MICROPIXEL_REMOTE_CONTROL_PORT=8443\n"
            "CONFIG_UNRELATED=y\n",
            encoding="utf-8",
        )
        firmware.apply_remote_control_env(self.profile, environ={})
        generated = self.profile.sdkconfig.read_text(encoding="utf-8")
        self.assertIn('CONFIG_MICROPIXEL_REMOTE_CONTROL_HOST="quic.micropixel.ai"', generated)
        self.assertIn("CONFIG_MICROPIXEL_REMOTE_CONTROL_PORT=443\n", generated)
        self.assertIn("CONFIG_UNRELATED=y\n", generated)

    def test_explicit_offline_override_stays_offline(self) -> None:
        firmware.apply_remote_control_env(
            self.profile, environ={firmware.REMOTE_CONTROL_HOST_ENV: ""}
        )
        defaults = (self.root / firmware.REMOTE_CONTROL_ENV_DEFAULTS_NAME).read_text()
        self.assertIn('CONFIG_MICROPIXEL_REMOTE_CONTROL_HOST=""', defaults)

    def test_partial_override_keeps_other_public_defaults(self) -> None:
        updates, summary = firmware.remote_control_env_defaults(
            {firmware.REMOTE_CONTROL_PORT_ENV: "9443"}
        )
        self.assertEqual(summary, "quic.micropixel.ai:9443")
        self.assertIn("CONFIG_MICROPIXEL_REMOTE_CONTROL_PORT=9443", updates)

    def test_local_endpoint_and_explicit_tls_policy_are_preserved(self) -> None:
        updates, summary = firmware.remote_control_env_defaults({
            firmware.REMOTE_CONTROL_HOST_ENV: "192.168.1.20",
            firmware.REMOTE_CONTROL_PORT_ENV: "8443",
            firmware.REMOTE_CONTROL_TLS_ENV: "n",
            firmware.REMOTE_CONTROL_CA_ENV: "dGVzdA==",
        })
        self.assertEqual(summary, "192.168.1.20:8443")
        self.assertIn("# CONFIG_MICROPIXEL_REMOTE_CONTROL_ALLOW_UNVERIFIED_TLS is not set", updates)
        self.assertIn('CONFIG_MICROPIXEL_REMOTE_CONTROL_TRUSTED_CA_DER_BASE64="dGVzdA=="', updates)

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


class RemoteControlShellTest(unittest.TestCase):
    def test_wrappers_refresh_stale_endpoints_and_keep_overrides(self) -> None:
        cases = (
            ({}, "quic.micropixel.ai", "443", "y", ""),
            ({firmware.REMOTE_CONTROL_HOST_ENV: ""}, "", "443", "y", ""),
            ({
                firmware.REMOTE_CONTROL_HOST_ENV: "local.example",
                firmware.REMOTE_CONTROL_PORT_ENV: "8443",
                firmware.REMOTE_CONTROL_TLS_ENV: "n",
                firmware.REMOTE_CONTROL_CA_ENV: "dGVzdA==",
            }, "local.example", "8443", "n", "dGVzdA=="),
        )
        for wrapper, next_function in (
            ("p4.sh", "idf_host"),
            ("s31.sh", "build_profile"),
            ("s3.sh", "resolve_profile_port"),
        ):
            script = (firmware.WORKSPACE_ROOT / "tools" / wrapper).read_text()
            function = "build_profile" if wrapper == "s3.sh" else "prepare_host_config"
            body = script.split(f"{function}() {{", 1)[1].split(f"\n{next_function}() {{", 1)[0]
            for environ, host, port, tls, ca in cases:
                with self.subTest(wrapper=wrapper, host=host), tempfile.TemporaryDirectory() as temporary:
                    root = Path(temporary)
                    build = root / "build"
                    build.mkdir()
                    config = build / "sdkconfig.release"
                    config.write_text(
                        'CONFIG_MICROPIXEL_REMOTE_CONTROL_HOST=""\n'
                        'CONFIG_MICROPIXEL_REMOTE_CONTROL_PORT=8443\n'
                        'CONFIG_UNRELATED=y\n'
                    )
                    (root / "sdkconfig.p4.defaults").write_text("CONFIG_LV_MEM_SIZE=1048576\n")
                    command = """set -euo pipefail
firmware_dir="$1"
host_build_dir="$1/build"
sdkconfig_path="$host_build_dir/sdkconfig.release"
sdkconfig_defaults="$1/sdkconfig.defaults"
host_config_prepared=false
common_max_task_name_len=32
lv_mem_size_bytes=1048576
profile_action() { :; }
""" + f"{function}() {{" + body + f'\n{function} test "$host_build_dir"\n'
                    subprocess.run(
                        ["bash", "-c", command, "test", temporary], check=True,
                        env={"PATH": "/usr/bin:/bin", **environ}, capture_output=True, text=True,
                    )
                    defaults_name = "sdkconfig.remote.defaults" if wrapper == "s3.sh" else "sdkconfig.env.defaults"
                    for path in (config, build / defaults_name):
                        generated = path.read_text()
                        self.assertIn(f'CONFIG_MICROPIXEL_REMOTE_CONTROL_HOST="{host}"\n', generated)
                        self.assertIn(f"CONFIG_MICROPIXEL_REMOTE_CONTROL_PORT={port}\n", generated)
                        tls_line = (
                            "CONFIG_MICROPIXEL_REMOTE_CONTROL_ALLOW_UNVERIFIED_TLS=y\n"
                            if tls == "y" else "# CONFIG_MICROPIXEL_REMOTE_CONTROL_ALLOW_UNVERIFIED_TLS is not set\n"
                        )
                        self.assertIn(tls_line, generated)
                        self.assertIn(f'CONFIG_MICROPIXEL_REMOTE_CONTROL_TRUSTED_CA_DER_BASE64="{ca}"\n', generated)
                    self.assertIn("CONFIG_UNRELATED=y\n", config.read_text())
