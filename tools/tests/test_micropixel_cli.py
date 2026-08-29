from __future__ import annotations

import base64
import argparse
import importlib.machinery
import importlib.util
import io
import json
import os
import stat
import struct
import sys
import tempfile
import threading
import unittest
from contextlib import redirect_stderr, redirect_stdout
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from unittest.mock import patch


WORKSPACE_ROOT = Path(__file__).resolve().parents[2]
CLI_PATH = WORKSPACE_ROOT / "tools" / "micropixel"
LOADER = importlib.machinery.SourceFileLoader("micropixel_cli", str(CLI_PATH))
SPEC = importlib.util.spec_from_loader(LOADER.name, LOADER)
assert SPEC is not None
CLI = importlib.util.module_from_spec(SPEC)
sys.modules[LOADER.name] = CLI
LOADER.exec_module(CLI)


def api_token(device_id: str) -> str:
    encoded = base64.urlsafe_b64encode(
        json.dumps({"typ": "api", "device_id": device_id}, separators=(",", ":")).encode("utf-8")
    ).rstrip(b"=").decode("ascii")
    return f"header.{encoded}.signature"


class MicroPixelCliTest(unittest.TestCase):
    def test_usb_screenshot_uses_binary_safe_display_framing(self) -> None:
        class FakeSerial:
            def __init__(self) -> None:
                self.responses = bytearray()

            def write(self, data: bytes) -> int:
                if data == b"MICROPIXEL_CAPTURE\n":
                    jpeg = b"\xff\xd8binary\x00\nMICROPIXEL_CAPTURE_END 999\n\xff\xd9"
                    self.responses.extend(
                        b"\nMICROPIXEL_CAPTURE_BEGIN 7 480 480 JPEG "
                        + str(len(jpeg)).encode("ascii")
                        + b" DISPLAY\n"
                        + jpeg
                        + b"\nMICROPIXEL_CAPTURE_END 7\n"
                    )
                else:
                    fields = data.decode("ascii").strip().split()
                    if fields[2] != "HELLO":
                        raise AssertionError(f"unexpected operation {fields[2]}")
                    self.responses.extend(f"\nMPX1 {fields[1]} OK HELLO 1 3072 8388608\n".encode("ascii"))
                return len(data)

            def read(self, size: int) -> bytes:
                data = bytes(self.responses[:size])
                del self.responses[:size]
                return data

            def close(self) -> None:
                pass

        with patch.object(CLI, "discover_usb_port", return_value="/dev/fake"), patch.object(
            CLI, "open_usb_serial", return_value=FakeSerial()
        ):
            with CLI.UsbControlClient(None, 1.0) as client:
                jpeg, width, height = client.capture_screen()
        self.assertEqual((width, height), (480, 480))
        self.assertTrue(jpeg.startswith(b"\xff\xd8"))
        self.assertTrue(jpeg.endswith(b"\xff\xd9"))

    def test_usb_transport_lists_controls_and_installs_apps(self) -> None:
        class FakeSerial:
            def __init__(self) -> None:
                self.responses = bytearray()
                self.installed = bytearray()
                self.expected_size = 0
                self.closed = False

            def write(self, data: bytes) -> int:
                fields = data.decode("ascii").strip().split()
                self.assert_prefix = fields[:1]
                request_id = fields[1]
                operation = fields[2]
                if operation == "HELLO":
                    detail = "HELLO 1 3072 8388608"
                elif operation == "APP_LIST":
                    detail = (
                        "APP_LIST 1 1 65536 25165824 1 "
                        "vendor.demo,65536,RGVtbw==,0,not_running"
                    )
                elif operation == "APP_START":
                    detail = "RESULT app_started"
                elif operation == "APP_LAST_ERROR":
                    diagnostic_detail = base64.b64encode(b"Exception: out of bounds memory access").decode("ascii")
                    detail = f"APP_ERROR micropixel.demo run guest_trap 0 0 {diagnostic_detail}"
                elif operation == "LOG_READ":
                    message = base64.b64encode(b"guest ready").decode("ascii")
                    detail = f"LOG_PAGE 0000000000000001 1 0 0 1 1 12345 2 vendor.demo {message}"
                elif operation == "INPUT_SEQUENCE":
                    operations = json.loads(base64.b64decode(fields[3], validate=True))
                    if len(operations) != 2 or operations[0].get("type") != "key":
                        raise AssertionError("unexpected input sequence")
                    detail = "RESULT input_sequence_completed"
                elif operation == "FIRMWARE_STATUS":
                    detail = "RESULT firmware_status 3 1 1 0 0.2.5"
                elif operation == "DEVICE_STATUS":
                    version = base64.b64encode(b"0.2.4").decode("ascii")
                    board = base64.b64encode(b"ESP-Mosaico").decode("ascii")
                    chip = base64.b64encode(b"ESP32-S31").decode("ascii")
                    detail = (
                        f"DEVICE_STATUS {version} {board} {chip} 1234 - not_running 1 65536 25165824 "
                        "100000 200000 480 480 1 1 1"
                    )
                elif operation == "DEVICE_TASKS":
                    name = base64.b64encode(b"main").decode("ascii")
                    detail = f"TASK_PAGE 1 1 1 1 0 999 1 {name},7,0,5,5,123,4096"
                elif operation == "DEVICE_REBOOT":
                    detail = "RESULT rebooting"
                elif operation == "APP_INSTALL_BEGIN":
                    self.expected_size = int(fields[4])
                    self.installed.clear()
                    detail = "INSTALL_READY 3072"
                elif operation == "APP_INSTALL_CHUNK":
                    offset = int(fields[3])
                    self.assert_offset = offset
                    if offset != len(self.installed):
                        raise AssertionError("unexpected chunk offset")
                    self.installed.extend(base64.b64decode(fields[4], validate=True))
                    detail = f"INSTALL_CHUNK {len(self.installed)}"
                elif operation == "APP_INSTALL_COMMIT":
                    if len(self.installed) != self.expected_size:
                        raise AssertionError("incomplete install")
                    detail = "RESULT app_installed"
                else:
                    raise AssertionError(f"unexpected operation {operation}")
                self.responses.extend(f"\nMPX1 {request_id} OK {detail}\n".encode("ascii"))
                return len(data)

            def read(self, size: int) -> bytes:
                data = bytes(self.responses[:size])
                del self.responses[:size]
                return data

            def close(self) -> None:
                self.closed = True

        fake = FakeSerial()
        with patch.object(CLI, "discover_usb_port", return_value="/dev/fake"), patch.object(
            CLI, "open_usb_serial", return_value=fake
        ):
            with CLI.UsbControlClient(None, 1.0) as client:
                catalog = client.list_apps()
                self.assertEqual(catalog["apps"][0]["displayName"], "Demo")
                self.assertEqual(client.action("APP_START", "vendor.demo")["result"]["message"], "app_started")
                self.assertEqual(client.last_app_error()["code"], "guest_trap")
                cursor, has_more, truncated, entries = client.read_log(None)
                self.assertEqual(cursor, "0000000000000001:1")
                self.assertFalse(has_more)
                self.assertFalse(truncated)
                self.assertEqual(entries[0]["message"], "guest ready")
                input_result = client.input_sequence(
                    [
                        {"type": "key", "code": "confirm", "phase": "down"},
                        {"type": "key", "code": "confirm", "phase": "up", "delayMs": 80},
                    ]
                )
                self.assertEqual(input_result["result"]["message"], "input_sequence_completed")
                self.assertEqual(client.firmware_status()["latestVersion"], "0.2.5")
                self.assertEqual(client.device_status()["hardware"]["board"], "ESP-Mosaico")
                self.assertEqual(client.task_diagnostics()["tasks"][0]["name"], "main")
                self.assertEqual(client.action("DEVICE_REBOOT")["result"]["message"], "rebooting")
                with tempfile.TemporaryDirectory() as directory:
                    bundle = bytearray(CLI.BUNDLE_ALIGNMENT)
                    bundle[:8] = CLI.BUNDLE_MAGIC
                    struct.pack_into("<III", bundle, 8, 1, CLI.BUNDLE_HEADER_SIZE, len(bundle))
                    bundle[24:35] = b"vendor.demo"
                    struct.pack_into("<I", bundle, 88, 11)
                    path = Path(directory) / "demo.bundle.bin"
                    path.write_bytes(bundle)
                    result = client.install(path)
                self.assertEqual(result["result"]["message"], "app_installed")
                self.assertEqual(bytes(fake.installed), bytes(bundle))
        self.assertTrue(fake.closed)

    def test_usb_transport_options_are_explicit(self) -> None:
        args = CLI.parser().parse_args(
            ["--transport", "usb", "--port", "/dev/cu.usbmodem1101", "app", "list"]
        )
        self.assertEqual(args.transport, "usb")
        self.assertEqual(args.port, "/dev/cu.usbmodem1101")

    def test_usb_serial_omits_posix_exclusive_option_on_windows(self) -> None:
        class FakeDevice:
            def __init__(self, **options: object) -> None:
                self.options = options
                self.dtr = False
                self.rts = False
                self.port = None

            def open(self) -> None:
                pass

            def close(self) -> None:
                pass

        class FakeSerialModule:
            Serial = FakeDevice

        with patch.dict(sys.modules, {"serial": FakeSerialModule()}), patch.object(CLI.os, "name", "nt"):
            device = CLI.open_usb_serial("COM7")
        self.assertEqual(device.port, "COM7")
        self.assertNotIn("exclusive", device.options)

    def test_manifest_driven_package_generates_catalog_and_bundle_v1(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "i18n").mkdir()
            (root / "main.cpp").write_text("int fixture = 1;\n", encoding="utf-8")
            (root / "i18n/en.json").write_text('{"app.title":"Fixture"}\n', encoding="utf-8")
            (root / "app.json").write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "app_id": "vendor.fixture",
                        "display": "landscape",
                        "display_name": {"default": "en", "values": {"en": "Fixture"}},
                        "localization": {
                            "default": "en",
                            "translations": {"en": "i18n/en.json"},
                        },
                        "source": "main.cpp",
                        "sources": ["main.cpp"],
                    }
                ),
                encoding="utf-8",
            )
            output_dir = root / "out"

            def fake_build(args: argparse.Namespace) -> None:
                self.assertEqual(args.display_profile, "landscape")
                Path(args.output_dir_override).mkdir(parents=True, exist_ok=True)
                (Path(args.output_dir_override) / "fixture.aot").write_bytes(b"fixture-aot")

            args = argparse.Namespace(
                project=str(root),
                profile="release",
                output_dir=str(output_dir),
                output=None,
                raw=None,
            )
            with patch.object(CLI, "_run_build_sources", side_effect=fake_build):
                with redirect_stdout(io.StringIO()):
                    bundle = CLI.run_package(args)

            self.assertEqual(CLI.validate_bundle(bundle)["appId"], "vendor.fixture")
            self.assertTrue((output_dir / "generated/fixture_strings.hpp").is_file())
            self.assertEqual(
                json.loads((output_dir / "localization-report.json").read_text(encoding="utf-8"))["schema_version"],
                1,
            )

    def test_project_manifest_rejects_paths_outside_project(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "outside.cpp").write_text("int outside;\n", encoding="utf-8")
            project = root / "project"
            project.mkdir()
            manifest = project / "app.json"
            manifest.write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "app_id": "vendor.fixture",
                        "display": "square",
                        "display_name": "Fixture",
                        "source": "../outside.cpp",
                        "sources": ["../outside.cpp"],
                    }
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(CLI.CliError, "escapes the project directory"):
                CLI.load_project_manifest(manifest)

    def test_build_package_and_install_default_to_current_project(self) -> None:
        self.assertEqual(CLI.parser().parse_args(["build"]).source, ".")
        self.assertEqual(CLI.parser().parse_args(["package"]).project, ".")
        run = CLI.parser().parse_args(["run"])
        self.assertEqual(run.project, ".")
        self.assertEqual(run.profile, "development")
        self.assertTrue(run.follow)
        self.assertFalse(CLI.parser().parse_args(["run", "--no-follow"]).follow)
        self.assertEqual(CLI.parser().parse_args(["app", "install"]).project_or_bundle, ".")
        self.assertIsNone(CLI.parser().parse_args(["app", "start", "--follow"]).app_id)
        with self.assertRaises(SystemExit):
            CLI.parser().parse_args(["install"])

    def test_current_project_supplies_app_id_for_start(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "main.cpp").write_text("int fixture = 1;\n", encoding="utf-8")
            (root / "app.json").write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "app_id": "vendor.fixture",
                        "display": "square",
                        "display_name": "Fixture",
                        "source": "main.cpp",
                        "sources": ["main.cpp"],
                    }
                ),
                encoding="utf-8",
            )
            self.assertEqual(CLI.current_project_app_id(root), "vendor.fixture")

    def test_canonical_and_legacy_command_names_are_both_parsed(self) -> None:
        canonical = CLI.parser().parse_args(["bundle", "validate", "demo.bundle.bin"])
        self.assertEqual((canonical.command, canonical.bundle_command), ("bundle", "validate"))
        paired = CLI.parser().parse_args(["auth", "pair", "--connection-code", "ABCD-1234"])
        self.assertEqual((paired.command, paired.auth_command), ("auth", "pair"))
        artifact = CLI.parser().parse_args(["artifact", "upload", "demo.bundle.bin"])
        self.assertEqual((artifact.command, artifact.artifact_command), ("artifact", "upload"))
        self.assertEqual(CLI.parser().parse_args(["validate", "demo.bundle.bin"]).command, "validate")
        self.assertEqual(
            CLI.parser().parse_args(["auth", "login", "--connection-code", "ABCD-1234"]).auth_command,
            "login",
        )

    def test_process_environment_overrides_local_dotenv(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / ".env").write_text(
                "MICROPIXEL_CONTROL_URL=https://from-file.example\n"
                "MICROPIXEL_DEVICE_ID=11111111-1111-1111-1111-111111111111\n"
                f"MICROPIXEL_API_TOKEN={api_token('11111111-1111-1111-1111-111111111111')}\n",
                encoding="utf-8",
            )
            with patch.dict(
                os.environ,
                {
                    "MICROPIXEL_CONTROL_URL": "https://micropixel.ai",
                    "MICROPIXEL_DEVICE_ID": "22222222-2222-2222-2222-222222222222",
                    "MICROPIXEL_API_TOKEN": api_token("22222222-2222-2222-2222-222222222222"),
                },
                clear=True,
            ):
                config = CLI.ControlConfig.load(root)
            self.assertEqual(config.base_url, "https://micropixel.ai")
            self.assertEqual(config.device_id, "22222222-2222-2222-2222-222222222222")
            self.assertEqual(config.token, api_token("22222222-2222-2222-2222-222222222222"))

    def test_device_id_is_derived_from_api_token(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            with patch.dict(
                os.environ,
                {"MICROPIXEL_API_TOKEN": api_token("33333333-3333-3333-3333-333333333333")},
                clear=True,
            ):
                config = CLI.ControlConfig.load(Path(directory))
            self.assertEqual(config.device_id, "33333333-3333-3333-3333-333333333333")

    def test_dotenv_does_not_execute_shell_syntax(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            env_file = Path(directory) / ".env"
            env_file.write_text("MICROPIXEL_API_TOKEN=$(echo unsafe)\n", encoding="utf-8")
            self.assertEqual(CLI.dotenv_values(env_file)["MICROPIXEL_API_TOKEN"], "$(echo unsafe)")

    def test_validates_bundle_identity(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            bundle = bytearray(CLI.BUNDLE_ALIGNMENT)
            bundle[:8] = CLI.BUNDLE_MAGIC
            struct.pack_into("<III", bundle, 8, 1, CLI.BUNDLE_HEADER_SIZE, len(bundle))
            bundle[24:36] = b"example.demo"
            struct.pack_into("<I", bundle, 88, 12)
            path = Path(directory) / "demo.bundle.bin"
            path.write_bytes(bundle)
            self.assertEqual(CLI.validate_bundle(path), {"appId": "example.demo", "sizeBytes": len(bundle)})

    def test_control_client_sends_bearer_token_and_reads_json(self) -> None:
        class Handler(BaseHTTPRequestHandler):
            def do_GET(self) -> None:
                if (
                    self.headers.get("Authorization") != "Bearer test-token"
                    or not self.headers.get("User-Agent", "").startswith("MicroPixel-CLI/")
                ):
                    self.send_response(401)
                    self.end_headers()
                    return
                body = b'{"online":true}'
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, _format: str, *_args: object) -> None:
                return

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            client = CLI.ControlClient(
                CLI.ControlConfig(
                    base_url=f"http://127.0.0.1:{server.server_port}",
                    device_id="22222222-2222-2222-2222-222222222222",
                    token="test-token",
                )
            )
            self.assertEqual(client.json("GET", client.device_path()), {"online": True})
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_control_client_sends_stable_job_identity_and_device_deadline(self) -> None:
        received: dict[str, str | None] = {}

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self) -> None:
                received["idempotency_key"] = self.headers.get("Idempotency-Key")
                received["command_timeout_ms"] = self.headers.get(
                    "X-MicroPixel-Command-Timeout-Ms"
                )
                length = int(self.headers.get("Content-Length", "0"))
                self.rfile.read(length)
                body = b'{"id":"55555555-5555-4555-8555-555555555555"}'
                self.send_response(202)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, _format: str, *_args: object) -> None:
                return

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            client = CLI.ControlClient(
                CLI.ControlConfig(
                    base_url=f"http://127.0.0.1:{server.server_port}",
                    device_id="22222222-2222-4222-8222-222222222222",
                    token="test-token",
                )
            )
            client.json(
                "POST",
                client.device_path("/screenshots"),
                {},
                idempotency_key="retry-key-123",
                command_timeout_ms=12_345,
            )
            self.assertEqual(received["idempotency_key"], "retry-key-123")
            self.assertEqual(received["command_timeout_ms"], "12345")
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_auth_login_exchanges_code_and_saves_permanent_token_in_dotenv(self) -> None:
        device_id = "44444444-4444-4444-8444-444444444444"
        issued_token = api_token(device_id)

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self) -> None:
                length = int(self.headers.get("Content-Length", "0"))
                body = json.loads(self.rfile.read(length))
                if (
                    self.path != "/api/v1/pairings/exchange"
                    or body != {"code": "ABCD-1234"}
                    or self.headers.get("Authorization") is not None
                ):
                    self.send_response(400)
                    self.end_headers()
                    return
                payload = json.dumps(
                    {
                        "deviceId": device_id,
                        "token": issued_token,
                        "tokenKind": "api",
                        "scopes": list(CLI.ALL_API_SCOPES),
                        "lifetime": "permanent",
                        "expiresAt": None,
                    }
                ).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            def log_message(self, _format: str, *_args: object) -> None:
                return

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                env_file = root / ".env"
                env_file.write_text("# keep me\nexport MICROPIXEL_API_TOKEN=old\n", encoding="utf-8")
                args = argparse.Namespace(
                    connection_code="ABCD-1234",
                    env_file=str(env_file),
                    control_url=f"http://127.0.0.1:{server.server_port}",
                )
                output = io.StringIO()
                with patch.dict(os.environ, {}, clear=True), patch.object(CLI.Path, "cwd", return_value=root):
                    with redirect_stdout(output):
                        CLI.run_auth_login(args)
                self.assertEqual(
                    env_file.read_text(encoding="utf-8"),
                    f"# keep me\nMICROPIXEL_API_TOKEN={issued_token}\n",
                )
                self.assertEqual(stat.S_IMODE(env_file.stat().st_mode), 0o600)
                self.assertNotIn(issued_token, output.getvalue())
                self.assertIn("Permanent API token saved", output.getvalue())
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_stop_without_app_id_stops_the_runtime_owner(self) -> None:
        class Client:
            def __init__(self) -> None:
                self.request: tuple[str, str, object] | None = None

            def device_path(self, suffix: str = "") -> str:
                return f"/device{suffix}"

            def json(self, method: str, path: str, body: object = None, timeout: float = 30.0) -> dict[str, object]:
                self.request = (method, path, body)
                return {"id": "job-id"}

            def wait_job(self, job: dict[str, object], timeout: float) -> dict[str, object]:
                self.assert_job = (job, timeout)
                return {"status": "succeeded"}

        client = Client()
        args = argparse.Namespace(command="app", app_command="stop", app_id=None, timeout=15.0)
        with redirect_stdout(io.StringIO()):
            CLI.execute_network(args, client)
        self.assertEqual(client.request, ("POST", "/device/runtime/foreground/stop", {}))
        self.assertEqual(client.assert_job, ({"id": "job-id"}, 15.0))

    def test_start_can_attach_to_guest_logs(self) -> None:
        class Client:
            def __init__(self) -> None:
                self.requests: list[tuple[str, str, object]] = []

            def device_path(self, suffix: str = "") -> str:
                return f"/device{suffix}"

            def json(
                self,
                method: str,
                path: str,
                body: object = None,
                timeout: float = 30.0,
                **_options: object,
            ) -> dict[str, object]:
                self.requests.append((method, path, body))
                if path == "/device":
                    return {"online": True, "status": {"firmwareVersion": "0.2.2"}}
                return {"id": "start-job"}

            def wait_job(self, job: dict[str, object], timeout: float) -> dict[str, object]:
                return {"status": "succeeded"}

        client = Client()
        args = argparse.Namespace(
            command="app",
            app_command="start",
            app_id="vendor.demo",
            follow=True,
            interval=0.25,
            timeout=15.0,
            command_timeout_ms=10_000,
            idempotency_key=None,
        )
        with patch.object(CLI, "stream_network_logs") as stream, redirect_stdout(io.StringIO()):
            CLI.execute_network(args, client)
        self.assertIn(("POST", "/device/apps/vendor.demo/actions/start", {}), client.requests)
        stream.assert_called_once_with(
            client,
            cursor=None,
            lines=None,
            follow=True,
            interval=0.25,
            timeout=15.0,
            command_timeout_ms=10_000,
        )

    def test_install_can_start_and_attach_using_bundle_app_id(self) -> None:
        class Client:
            def __init__(self) -> None:
                self.requests: list[tuple[str, str, object]] = []

            def device_path(self, suffix: str = "") -> str:
                return f"/device{suffix}"

            def json(self, method: str, path: str, body: object = None, timeout: float = 30.0) -> dict[str, object]:
                self.requests.append((method, path, body))
                if path == "/device":
                    return {"online": True, "status": {"firmwareVersion": "0.2.2"}}
                return {"id": path}

            def wait_job(self, job: dict[str, object], timeout: float) -> dict[str, object]:
                return {"status": "succeeded"}

        client = Client()
        args = argparse.Namespace(
            command="app",
            app_command="install",
            bundle="demo.bundle.bin",
            app_id="vendor.demo",
            start=True,
            follow=True,
            interval=0.5,
            timeout=20.0,
            command_timeout_ms=None,
            idempotency_key=None,
        )
        with patch.object(CLI, "upload", return_value={"packageId": "package-id"}), patch.object(
            CLI, "stream_network_logs"
        ) as stream, redirect_stdout(io.StringIO()):
            CLI.execute_network(args, client)
        self.assertIn(("POST", "/device/apps/install", {"packageId": "package-id"}), client.requests)
        self.assertIn(("POST", "/device/apps/vendor.demo/actions/start", {}), client.requests)
        stream.assert_called_once()

    def test_run_stops_installs_starts_and_attaches_over_remote(self) -> None:
        class Client:
            def __init__(self) -> None:
                self.requests: list[tuple[str, str, object]] = []

            def device_path(self, suffix: str = "") -> str:
                return f"/device{suffix}"

            def json(
                self,
                method: str,
                path: str,
                body: object = None,
                timeout: float = 30.0,
                **_options: object,
            ) -> dict[str, object]:
                self.requests.append((method, path, body))
                if method == "GET":
                    return {
                        "online": True,
                        "status": {
                            "firmwareVersion": "0.2.2",
                            "runtime": {
                                "foregroundSessionId": "session-old",
                                "runtimeSessions": [
                                    {
                                        "sessionId": "session-old",
                                        "appId": "vendor.old",
                                        "foreground": True,
                                    }
                                ],
                            },
                        },
                    }
                return {"id": path}

            def wait_job(self, job: dict[str, object], timeout: float) -> dict[str, object]:
                return {"status": "succeeded"}

        client = Client()
        args = argparse.Namespace(
            bundle="demo.bundle.bin",
            app_id="vendor.demo",
            follow=True,
            interval=0.25,
            timeout=20.0,
            command_timeout_ms=None,
            idempotency_key=None,
        )
        with patch.object(CLI, "upload", return_value={"packageId": "package-id"}), patch.object(
            CLI, "stream_network_logs"
        ) as stream, redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            CLI.run_network_app(args, client)

        mutating_requests = [request for request in client.requests if request[0] == "POST"]
        self.assertEqual(
            mutating_requests,
            [
                ("POST", "/device/runtime/foreground/stop", {}),
                ("POST", "/device/apps/install", {"packageId": "package-id"}),
                ("POST", "/device/apps/vendor.demo/actions/start", {}),
            ],
        )
        stream.assert_called_once()

    def test_run_restores_previous_remote_app_when_install_fails(self) -> None:
        class Client:
            def __init__(self) -> None:
                self.requests: list[tuple[str, str, object]] = []

            def device_path(self, suffix: str = "") -> str:
                return f"/device{suffix}"

            def json(
                self,
                method: str,
                path: str,
                body: object = None,
                timeout: float = 30.0,
                **_options: object,
            ) -> dict[str, object]:
                self.requests.append((method, path, body))
                if method == "GET":
                    return {
                        "online": True,
                        "status": {
                            "firmwareVersion": "0.2.2",
                            "runtime": {
                                "foregroundSessionId": "session-old",
                                "runtimeSessions": [{"sessionId": "session-old", "appId": "vendor.old"}],
                            },
                        },
                    }
                return {"id": path}

            def wait_job(self, job: dict[str, object], timeout: float) -> dict[str, object]:
                if job["id"] == "/device/apps/install":
                    raise CLI.CliError("install failed")
                return {"status": "succeeded"}

        client = Client()
        args = argparse.Namespace(
            bundle="demo.bundle.bin",
            app_id="vendor.demo",
            follow=False,
            interval=2.0,
            timeout=20.0,
            command_timeout_ms=None,
            idempotency_key=None,
        )
        with patch.object(CLI, "upload", return_value={"packageId": "package-id"}), redirect_stderr(io.StringIO()):
            with self.assertRaisesRegex(CLI.CliError, "install failed"):
                CLI.run_network_app(args, client)
        self.assertIn(("POST", "/device/apps/vendor.old/actions/start", {}), client.requests)

    def test_run_stops_installs_starts_and_attaches_over_usb(self) -> None:
        class Client:
            def __init__(self) -> None:
                self.operations: list[tuple[str, object]] = []

            def device_status(self) -> dict[str, object]:
                return {"runtime": {"activeAppId": "vendor.old"}}

            def action(self, operation: str, app_id: str | None = None) -> dict[str, object]:
                self.operations.append((operation, app_id))
                return {"status": "succeeded"}

            def install(self, bundle: Path) -> dict[str, object]:
                self.operations.append(("INSTALL", bundle))
                return {"status": "succeeded"}

        client = Client()
        args = argparse.Namespace(
            bundle="demo.bundle.bin",
            app_id="vendor.demo",
            follow=True,
            interval=0.5,
        )
        with patch.object(CLI, "stream_usb_logs") as stream, redirect_stderr(io.StringIO()):
            CLI.run_usb_app(args, client)
        self.assertEqual(
            client.operations,
            [
                ("APP_STOP", None),
                ("INSTALL", Path("demo.bundle.bin")),
                ("APP_START", "vendor.demo"),
            ],
        )
        stream.assert_called_once()

    def test_job_failure_formats_app_diagnostic_for_humans_and_agents(self) -> None:
        message = CLI.job_failure_message(
            "job-id",
            "failed",
            {
                "code": "app_launch_failed",
                "message": "The App could not be launched",
                "retryable": False,
                "details": {
                    "diagnostic": {
                        "appId": "vendor.demo",
                        "phase": "load",
                        "code": "aot_load_failed",
                        "detail": "invalid relocation type",
                    }
                }
            },
        )
        self.assertEqual(
            message,
            "App vendor.demo failed during load: aot_load_failed: invalid relocation type",
        )

    def test_last_error_reads_the_dedicated_device_endpoint(self) -> None:
        class Client:
            def device_path(self, suffix: str = "") -> str:
                return f"/device{suffix}"

            def json(self, method: str, path: str, body: object = None, timeout: float = 30.0) -> dict[str, object]:
                self.request = (method, path, body)
                return {"error": {"code": "guest_trap"}}

        client = Client()
        args = argparse.Namespace(command="app", app_command="last-error")
        output = io.StringIO()
        with redirect_stdout(output):
            CLI.execute_network(args, client)
        self.assertEqual(client.request, ("GET", "/device/app-errors/latest", None))
        rendered = json.loads(output.getvalue())
        self.assertEqual(rendered["code"], "guest_trap")
        self.assertIn("rebuild this App with SDK 0.9.4", rendered["recommendation"])

    def test_firmware_preflight_requires_0_2_2_before_install_or_start(self) -> None:
        class Client:
            def __init__(self, version: str, online: bool = True) -> None:
                self.version = version
                self.online = online

            def device_path(self, suffix: str = "") -> str:
                return f"/device{suffix}"

            def json(self, method: str, path: str, body: object = None, timeout: float = 30.0) -> dict[str, object]:
                self.request = (method, path, body)
                return {"online": self.online, "status": {"firmwareVersion": self.version}}

        current = Client("0.2.2")
        CLI.require_compatible_firmware(current)
        self.assertEqual(current.request, ("GET", "/device", None))

        with self.assertRaisesRegex(CLI.CliError, "Upgrade the device to firmware 0.2.2 or later"):
            CLI.require_compatible_firmware(Client("0.2.1"))
        with self.assertRaisesRegex(CLI.CliError, "device is offline"):
            CLI.require_compatible_firmware(Client("0.2.2", online=False))

    def test_reboot_and_firmware_commands_use_explicit_endpoints(self) -> None:
        class Client:
            def __init__(self) -> None:
                self.requests: list[tuple[str, str, object]] = []

            def device_path(self, suffix: str = "") -> str:
                return f"/device{suffix}"

            def json(
                self, method: str, path: str, body: object = None, timeout: float = 30.0
            ) -> dict[str, object]:
                self.requests.append((method, path, body))
                if path == "/device":
                    return {"online": True, "status": {"firmwareVersion": "0.2.2"}}
                if method == "GET":
                    return {"currentVersion": "0.2.2", "latestVersion": "0.2.3"}
                return {"id": "job-id"}

            def wait_job(self, job: dict[str, object], timeout: float) -> dict[str, object]:
                self.waited = (job, timeout)
                return {"status": "succeeded"}

        client = Client()
        with redirect_stdout(io.StringIO()):
            CLI.execute_network(
                argparse.Namespace(command="device", device_command="reboot", timeout=12.0), client
            )
        self.assertEqual(client.requests, [("POST", "/device/reboot", {})])

        client = Client()
        output = io.StringIO()
        with redirect_stdout(output):
            CLI.execute_network(argparse.Namespace(command="firmware", firmware_command="status"), client)
        self.assertEqual(
            client.requests,
            [("GET", "/device", None), ("GET", "/device/firmware/latest?currentVersion=0.2.2", None)],
        )
        self.assertEqual(json.loads(output.getvalue())["latestVersion"], "0.2.3")

        client = Client()
        with redirect_stdout(io.StringIO()):
            CLI.execute_network(
                argparse.Namespace(command="firmware", firmware_command="update", timeout=45.0), client
            )
        self.assertEqual(
            client.requests,
            [("GET", "/device", None), ("POST", "/device/firmware/update", {"currentVersion": "0.2.2"})],
        )
        self.assertEqual(client.waited, ({"id": "job-id"}, 45.0))

    def test_builds_bounded_key_tap_and_swipe_sequences(self) -> None:
        key = argparse.Namespace(input_command="press", code="confirm", hold_ms=80, screenshot="after.jpg")
        self.assertEqual(
            CLI.input_operations(key),
            [
                {"type": "key", "code": "confirm", "phase": "down"},
                {"type": "key", "code": "confirm", "phase": "up", "delayMs": 80},
                {"type": "screenshot", "id": "after-input", "delayMs": 120},
            ],
        )
        tap = argparse.Namespace(
            input_command="tap", x=360, y=240, pressure=500, hold_ms=40, screenshot=None
        )
        self.assertEqual(CLI.input_operations(tap)[-1]["phase"], "up")
        swipe = argparse.Namespace(
            input_command="swipe",
            x1=0,
            y1=10,
            x2=120,
            y2=70,
            duration_ms=240,
            steps=13,
            pressure=500,
            screenshot="after.jpg",
        )
        operations = CLI.input_operations(swipe)
        self.assertEqual(len(operations), 15)
        self.assertEqual(operations[0]["phase"], "down")
        self.assertEqual(operations[-2]["phase"], "up")
        self.assertEqual(operations[-1]["type"], "screenshot")

    def test_apps_refreshes_catalog_and_can_filter_running_apps(self) -> None:
        class Client:
            def device_path(self, suffix: str = "") -> str:
                return f"/device{suffix}"

            def json(self, method: str, path: str, body: object = None, timeout: float = 30.0) -> dict[str, object]:
                if path == "/device/apps/refresh":
                    self.refresh = (method, path, body)
                    return {"id": "job-id"}
                self.catalog_request = (method, path)
                return {
                    "apps": [
                        {"appId": "stopped", "active": False, "lifecycle": "not_running"},
                        {"appId": "running", "active": True, "lifecycle": "foreground"},
                    ],
                    "count": 2,
                }

            def wait_job(self, job: dict[str, object], timeout: float) -> dict[str, object]:
                self.waited = (job, timeout)
                return {"status": "succeeded"}

        client = Client()
        args = argparse.Namespace(
            command="app", app_command="list", cached=False, running=True, timeout=20.0
        )
        output = io.StringIO()
        with redirect_stdout(output):
            CLI.execute_network(args, client)
        self.assertEqual(client.refresh, ("POST", "/device/apps/refresh", {}))
        self.assertEqual(client.catalog_request, ("GET", "/device/apps"))
        self.assertEqual(json.loads(output.getvalue())["apps"][0]["appId"], "running")

    def test_diagnostics_refreshes_system_and_tasks_then_merges_output(self) -> None:
        class Client:
            def __init__(self) -> None:
                self.requests: list[tuple[str, str, object]] = []
                self.waited: list[tuple[dict[str, object], float]] = []
                self.options: list[dict[str, object]] = []

            def device_path(self, suffix: str = "") -> str:
                return f"/device{suffix}"

            def json(
                self, method: str, path: str, body: object = None, timeout: float = 30.0, **options: object
            ) -> dict[str, object]:
                self.requests.append((method, path, body))
                self.options.append(options)
                if path == "/device/system-info":
                    return {"memory": {"internalSram": {"freeBytes": 1234}}}
                if path == "/device/task-diagnostics":
                    return {"available": True, "taskCount": 2, "tasks": [{"name": "main"}]}
                return {"id": path}

            def wait_job(self, job: dict[str, object], timeout: float) -> dict[str, object]:
                self.waited.append((job, timeout))
                return {"status": "succeeded"}

        client = Client()
        args = argparse.Namespace(
            command="device",
            device_command="diagnostics",
            cached=False,
            timeout=20.0,
            idempotency_key="diagnostics-run",
            command_timeout_ms=10_000,
        )
        output = io.StringIO()
        with redirect_stdout(output):
            CLI.execute_network(args, client)

        self.assertEqual(
            client.requests,
            [
                ("POST", "/device/system-info/refresh", {}),
                ("POST", "/device/task-diagnostics/refresh", {}),
                ("GET", "/device/system-info", None),
                ("GET", "/device/task-diagnostics", None),
            ],
        )
        self.assertEqual(len(client.waited), 2)
        self.assertNotEqual(client.options[0]["idempotency_key"], client.options[1]["idempotency_key"])
        self.assertEqual(client.options[0]["command_timeout_ms"], 10_000)
        self.assertEqual(client.options[1]["command_timeout_ms"], 10_000)
        diagnostics = json.loads(output.getvalue())
        self.assertEqual(diagnostics["memory"]["internalSram"]["freeBytes"], 1234)
        self.assertEqual(diagnostics["taskDiagnostics"]["taskCount"], 2)

    def test_logs_lines_prints_only_latest_retained_entries(self) -> None:
        class Client:
            def __init__(self) -> None:
                self.snapshots = [
                    {
                        "entries": [
                            {"sequence": 1, "level": "info", "appId": "demo", "message": "one"},
                            {"sequence": 2, "level": "info", "appId": "demo", "message": "two"},
                        ],
                        "nextCursor": "after-two",
                        "hasMore": True,
                    },
                    {
                        "entries": [
                            {"sequence": 3, "level": "warn", "appId": "demo", "message": "three"},
                            {"sequence": 4, "level": "error", "appId": "demo", "message": "four"},
                        ],
                        "nextCursor": "after-four",
                        "hasMore": False,
                    },
                ]
                self.read_cursors: list[object] = []

            def device_path(self, suffix: str = "") -> str:
                return f"/device{suffix}"

            def json(self, method: str, path: str, body: object = None, timeout: float = 30.0) -> dict[str, object]:
                if method == "POST":
                    assert isinstance(body, dict)
                    self.read_cursors.append(body.get("cursor"))
                    return {"id": "job-id"}
                return self.snapshots.pop(0)

            def wait_job(self, job: dict[str, object], timeout: float) -> dict[str, object]:
                return {"status": "succeeded"}

        client = Client()
        args = argparse.Namespace(
            command="logs", cursor=None, lines=2, follow=False, interval=0.0, timeout=15.0
        )
        output = io.StringIO()
        with redirect_stdout(output):
            CLI.execute_network(args, client)

        self.assertEqual(client.read_cursors, [None, "after-two"])
        self.assertEqual(
            output.getvalue().splitlines(),
            ["     3 WARN  demo: three", "     4 ERROR demo: four"],
        )

        client = Client()
        args.lines = None
        output = io.StringIO()
        with redirect_stdout(output):
            CLI.execute_network(args, client)
        self.assertEqual(
            [line.rsplit(": ", 1)[-1] for line in output.getvalue().splitlines()],
            ["one", "two", "three", "four"],
        )

    def test_logs_lines_with_follow_prints_new_entries_after_initial_tail(self) -> None:
        class Client:
            def __init__(self) -> None:
                self.snapshots = [
                    {
                        "entries": [
                            {"sequence": 1, "level": "info", "appId": "demo", "message": "old"},
                            {"sequence": 2, "level": "info", "appId": "demo", "message": "recent"},
                        ],
                        "nextCursor": "initial",
                        "hasMore": False,
                    },
                    {
                        "entries": [
                            {"sequence": 3, "level": "info", "appId": "demo", "message": "new"},
                        ],
                        "nextCursor": "followed",
                        "hasMore": False,
                    },
                ]

            def device_path(self, suffix: str = "") -> str:
                return f"/device{suffix}"

            def json(self, method: str, path: str, body: object = None, timeout: float = 30.0) -> dict[str, object]:
                return {"id": "job-id"} if method == "POST" else self.snapshots.pop(0)

            def wait_job(self, job: dict[str, object], timeout: float) -> dict[str, object]:
                return {"status": "succeeded"}

        args = argparse.Namespace(
            command="logs", cursor=None, lines=1, follow=True, interval=0.0, timeout=15.0
        )
        output = io.StringIO()
        with patch.object(CLI.time, "sleep", side_effect=[None, KeyboardInterrupt]), redirect_stdout(output):
            CLI.execute_network(args, Client())

        self.assertEqual(
            [line.rsplit(": ", 1)[-1] for line in output.getvalue().splitlines()],
            ["recent", "new"],
        )

    def test_logs_lines_accepts_short_and_long_options(self) -> None:
        self.assertEqual(CLI.parser().parse_args(["logs", "-n", "0"]).lines, 0)
        self.assertEqual(CLI.parser().parse_args(["logs", "--lines", "12"]).lines, 12)
        with self.assertRaises(SystemExit):
            CLI.parser().parse_args(["logs", "-n", "-1"])

    def test_job_options_separate_device_deadline_from_wait_deadline(self) -> None:
        args = CLI.parser().parse_args(
            [
                "screenshot",
                "--wait-timeout",
                "45",
                "--command-timeout-ms",
                "12000",
                "--idempotency-key",
                "retry-key-123",
            ]
        )
        self.assertEqual(args.timeout, 45.0)
        self.assertEqual(args.command_timeout_ms, 12_000)
        self.assertEqual(args.idempotency_key, "retry-key-123")

        job_id = "66666666-6666-4666-8666-666666666666"
        resumed = CLI.parser().parse_args(["job", "wait", job_id, "--timeout", "30"])
        self.assertEqual(resumed.job_id, job_id)
        self.assertEqual(resumed.timeout, 30.0)


if __name__ == "__main__":
    unittest.main()
