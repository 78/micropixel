#!/usr/bin/env python3
"""Per-user SDK manager. Release metadata is data, never executable shell input."""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import importlib.machinery
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath

VERSION = '0.1.0'
_BUILD_FILE = Path(__file__).with_name('build.json')
BUILD_ID = json.loads(_BUILD_FILE.read_text(encoding='utf-8'))['build_id'] if _BUILD_FILE.exists() else 'source'
INDEX_URL = 'https://raw.githubusercontent.com/78/micropixel/sdk-channel/index.json'
LOCK_NAME = 'micropixel.lock.json'
IDENTIFIER = re.compile(r'[A-Za-z0-9][A-Za-z0-9._+-]{0,127}\Z')
SHA256 = re.compile(r'[0-9a-f]{64}\Z')


class Failure(Exception):
    def __init__(self, code: str, message: str, exit_code: int = 1):
        super().__init__(message)
        self.code, self.exit_code = code, exit_code


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def file_digest(path: Path) -> str:
    sha = hashlib.sha256()
    with path.open('rb') as stream:
        while block := stream.read(1024 * 1024):
            sha.update(block)
    return sha.hexdigest()


def newer(candidate: str, current: str | None) -> bool:
    def version(value):
        match = re.fullmatch(r'(\d+)\.(\d+)\.(\d+)(?:-(.+))?', value)
        if not match:
            return None
        return (*map(int, match.group(1, 2, 3)), match.group(4) is None)
    return current is None or (version(candidate) is not None and version(current) is not None and version(candidate) > version(current))


def identifier(value: str) -> str:
    if not isinstance(value, str) or not IDENTIFIER.fullmatch(value):
        raise Failure('invalid_manifest', 'Invalid release identifier', 4)
    return value


def validate_index(value: dict) -> bool:
    if not isinstance(value, dict) or value.get('schema_version') != 1 or not isinstance(value.get('versions'), dict):
        return False
    versions = value['versions']
    for channel in ('stable', 'preview'):
        if value.get(channel) is not None and (not isinstance(value[channel], str) or value[channel] not in versions):
            return False
    for version, entry in versions.items():
        if not isinstance(version, str) or not IDENTIFIER.fullmatch(version) or not isinstance(entry, dict):
            return False
        if not isinstance(entry.get('sha256'), str) or not SHA256.fullmatch(entry['sha256']):
            return False
        if not isinstance(entry.get('url'), str) or not entry['url'].startswith('https://'):
            return False
        if not isinstance(entry.get('performance', []), list):
            return False
    manager = value.get('manager')
    if manager is not None and (not isinstance(manager, dict) or not isinstance(manager.get('version'), str)):
        return False
    return True


def read_json(path: Path) -> dict:
    value = json.loads(path.read_text(encoding='utf-8'))
    if not isinstance(value, dict):
        raise Failure('invalid_manifest', f'Expected an object: {path.name}', 4)
    return value


def atomic_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix='.' + path.name, dir=path.parent)
    try:
        with os.fdopen(fd, 'w', encoding='utf-8') as stream:
            json.dump(value, stream, ensure_ascii=False, indent=2)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(name, path)
    finally:
        Path(name).unlink(missing_ok=True)


@contextlib.contextmanager
def install_lock(root: Path):
    """OS-owned lock releases even on interruption; no stale PID lock deletion."""
    root.mkdir(parents=True, exist_ok=True)
    with (root / 'install.lock').open('a+b') as stream:
        if os.fstat(stream.fileno()).st_size == 0:
            stream.write(b'0')
            stream.flush()
        stream.seek(0)
        try:
            if os.name == 'nt':
                import msvcrt
                msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            raise Failure('installation_busy', 'Another installation is running; retry after it exits') from error
        try:
            yield
        finally:
            stream.seek(0)
            if os.name == 'nt':
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(stream.fileno(), fcntl.LOCK_UN)


def fetch(url: str, *, timeout: float = 5, limit: int = 4 * 1024 * 1024) -> bytes:
    if urllib.parse.urlsplit(url).scheme != 'https':
        raise Failure('insecure_download', 'Release downloads require HTTPS', 4)
    request = urllib.request.Request(url, headers={'User-Agent': 'MicroPixel-Manager/' + VERSION})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        if urllib.parse.urlsplit(response.url).scheme != 'https':
            raise Failure('insecure_download', 'HTTPS downgrade rejected', 4)
        content = response.read(limit + 1)
    if len(content) > limit:
        raise Failure('download_too_large', 'Download exceeds declared size')
    return content


def safe_path(name: str) -> Path:
    # Reject Windows aliases, drive names, ADS and traversal on every platform.
    if '\\' in name or ':' in name or name.startswith('/'):
        raise Failure('unsafe_archive', 'Unsafe archive path', 4)
    parts = PurePosixPath(name).parts
    reserved = {'CON', 'PRN', 'AUX', 'NUL', *(f'COM{i}' for i in range(1, 10)), *(f'LPT{i}' for i in range(1, 10))}
    if not parts or any(p in ('.', '..') or p.endswith((' ', '.')) or p.split('.')[0].upper() in reserved for p in parts):
        raise Failure('unsafe_archive', 'Unsafe archive member', 4)
    return Path(*parts)


def extract(archive: Path, destination: Path) -> None:
    """No links/devices; cap expanded size; reject case-folded duplicate filenames."""
    seen: set[str] = set()
    total = 0

    def copy(name, size, is_dir, source, mode=0o644):
        nonlocal total
        relative = safe_path(name)
        key = relative.as_posix().casefold()
        if key in seen:
            raise Failure('unsafe_archive', 'Duplicate archive member', 4)
        seen.add(key)
        total += size
        if total > 8 * 1024 ** 3:
            raise Failure('unsafe_archive', 'Expanded archive exceeds 8 GiB', 4)
        output = destination / relative
        if is_dir:
            output.mkdir(parents=True, exist_ok=True)
        else:
            output.parent.mkdir(parents=True, exist_ok=True)
            with output.open('xb') as target:
                shutil.copyfileobj(source, target)
            output.chmod(mode & 0o777)

    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as package:
            for info in package.infolist():
                mode = info.external_attr >> 16
                if (mode & 0o170000) not in (0, 0o100000, 0o040000):
                    raise Failure('unsafe_archive', 'Archive links and special files are forbidden', 4)
                with package.open(info) as source:
                    copy(info.filename, info.file_size, info.is_dir(), source, mode or 0o644)
    else:
        with tarfile.open(archive) as package:
            for info in package:
                if not (info.isfile() or info.isdir()):
                    raise Failure('unsafe_archive', 'Archive links and special files are forbidden', 4)
                source = package.extractfile(info) if info.isfile() else None
                try:
                    copy(info.name, info.size, info.isdir(), source, info.mode)
                finally:
                    if source:
                        source.close()


class Manager:
    def __init__(self, root: Path, *, offline: bool = False):
        self.root, self.offline = root, offline
        self.warnings: list[dict] = []

    def index(self, ttl: int = 86400, force: bool = False) -> tuple[dict, str, int | None]:
        cache_path = self.root / 'index-cache.json'
        try:
            cached = read_json(cache_path) if cache_path.exists() else {}
        except (OSError, ValueError, Failure):
            cached = {}
        url = os.environ.get('MICROPIXEL_SDK_INDEX_URL', INDEX_URL)
        if cached.get('url', INDEX_URL) != url:
            cached = {}
        if not validate_index(cached.get('index', {})):
            cached = {}
        checked = cached.get('checked_at')
        if not isinstance(checked, int):
            checked = None
        if self.offline:
            return cached.get('index', {}), 'offline', checked
        if not force and checked and 0 <= time.time() - checked < ttl:
            return cached['index'], 'cached', checked
        try:
            data = json.loads(fetch(url))
            if not validate_index(data):
                raise Failure('incompatible_index', 'Unsupported SDK index schema', 4)
            checked = int(time.time())
            atomic_json(cache_path, {'url': url, 'checked_at': checked, 'index': data})
            return data, 'checked', checked
        except (OSError, ValueError, Failure) as error:
            self.warnings.append({'code': 'update_check_failed', 'message': str(error)})
            return cached.get('index', {}), 'unavailable', checked

    def manifest(self, version: str, index: dict | None = None, expected: str | None = None) -> tuple[dict, str]:
        identifier(version)
        path = self.root / 'manifests' / (version + '.json')
        if path.exists():
            raw = path.read_bytes()
        else:
            if self.offline:
                raise Failure('dependency_missing', f'SDK {version} manifest is not cached', 4)
            index = index if index is not None else self.index()[0]
            entry = index.get('versions', {}).get(version)
            if not entry:
                raise Failure('version_unavailable', f'SDK {version} is absent from the release index', 4)
            expected = expected or entry['sha256']
            raw = fetch(entry['url'])
            if digest(raw) != expected:
                raise Failure('checksum_mismatch', 'SDK manifest checksum mismatch', 4)
            path.parent.mkdir(parents=True, exist_ok=True)
            # Only verified immutable data enters the cache.
            fd, temporary = tempfile.mkstemp(dir=path.parent)
            try:
                with os.fdopen(fd, 'wb') as stream:
                    stream.write(raw)
                os.replace(temporary, path)
            finally:
                Path(temporary).unlink(missing_ok=True)
        checksum = digest(raw)
        if expected and checksum != expected:
            raise Failure('checksum_mismatch', 'Cached manifest differs from the project lock', 4)
        manifest = json.loads(raw)
        if not isinstance(manifest, dict) or manifest.get('schema_version') != 1 or manifest.get('sdk_version') != version:
            raise Failure('incompatible_manifest', 'Unsupported or mismatched SDK manifest', 4)
        identifier(manifest['toolchain_id'])
        return manifest, checksum

    def asset(self, spec: dict, install: bool = False) -> Path:
        if not isinstance(spec, dict):
            raise Failure('invalid_manifest', 'Asset descriptor must be an object', 4)
        checksum = spec.get('sha256', '')
        if not SHA256.fullmatch(checksum):
            raise Failure('invalid_manifest', 'Asset requires a SHA-256 checksum', 4)
        location = self.root / 'packages' / checksum
        relative = safe_path(spec['root'])
        receipt = location / '.complete.json'
        if receipt.is_file() and read_json(receipt).get('sha256') == checksum:
            return location / relative
        if not install or self.offline:
            raise Failure('dependency_missing', f"Dependency is not cached: {spec.get('name', checksum)}", 4)
        size = spec.get('size_bytes')
        if not isinstance(size, int) or not 0 < size <= 2 * 1024 ** 3:
            raise Failure('invalid_manifest', 'Invalid archive size', 4)
        archives = self.root / 'downloads'
        archives.mkdir(parents=True, exist_ok=True)
        archive = archives / checksum
        if not archive.exists() or file_digest(archive) != checksum:
            print(f"Downloading {spec.get('name', checksum)} ({size} bytes)…", file=sys.stderr)
            # Stream large SDK/LLVM archives instead of holding them in memory.
            url = spec['url']
            if urllib.parse.urlsplit(url).scheme != 'https':
                raise Failure('insecure_download', 'Release downloads require HTTPS', 4)
            temporary = archive.with_suffix('.part')
            try:
                request = urllib.request.Request(url, headers={'User-Agent': 'MicroPixel-Manager/' + VERSION})
                with urllib.request.urlopen(request, timeout=30) as response, temporary.open('wb') as output:
                    if urllib.parse.urlsplit(response.url).scheme != 'https':
                        raise Failure('insecure_download', 'HTTPS downgrade rejected', 4)
                    sha, count, last_progress = hashlib.sha256(), 0, 0.0
                    while block := response.read(1024 * 1024):
                        count += len(block)
                        if count > size:
                            raise Failure('checksum_mismatch', 'Download exceeds declared size', 4)
                        output.write(block)
                        sha.update(block)
                        if time.monotonic() - last_progress >= 1:
                            print(f'Downloaded {count}/{size} bytes ({100 * count // size}%)', file=sys.stderr)
                            last_progress = time.monotonic()
                    if count != size or sha.hexdigest() != checksum:
                        raise Failure('checksum_mismatch', 'Download size or SHA-256 mismatch', 4)
                os.replace(temporary, archive)
            finally:
                temporary.unlink(missing_ok=True)
        packages = self.root / 'packages'
        packages.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='.extract-', dir=packages) as temporary:
            staging = Path(temporary) / 'content'
            staging.mkdir()
            extract(archive, staging)
            if not (staging / relative).is_dir():
                raise Failure('invalid_archive', 'Declared package root is missing', 4)
            atomic_json(staging / '.complete.json', {'sha256': checksum})
            if location.exists():
                shutil.rmtree(location)
            os.replace(staging, location)
        return location / relative

    def compose_wasi(self, wasi: Path, crt: Path, platform: dict, install: bool) -> Path:
        identity = digest((platform['wasi']['sha256'] + platform['msvc_crt']['sha256']).encode())
        destination = self.root / 'environments' / identity
        receipt = destination / '.complete.json'
        if receipt.is_file() and read_json(receipt).get('identity') == identity:
            return destination / 'wasi'
        if not install:
            raise Failure('dependency_missing', 'Prepared WASI runtime is missing; rerun setup --yes', 4)
        destination.parent.mkdir(parents=True, exist_ok=True)
        def link_or_copy(source, target):
            try:
                os.link(source, target)
            except OSError:
                shutil.copy2(source, target)
        with tempfile.TemporaryDirectory(prefix='.compose-', dir=destination.parent) as temporary:
            staging = Path(temporary) / 'content'
            shutil.copytree(wasi, staging / 'wasi', copy_function=link_or_copy)
            required = ('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
            if not all((crt / name).is_file() for name in required):
                raise Failure('dependency_corrupt', 'App-local MSVC runtime is incomplete', 4)
            for source in crt.iterdir():
                if source.is_file():
                    target = staging / 'wasi/bin' / source.name
                    # Unlink first: never modify the immutable cache through a hard link.
                    target.unlink(missing_ok=True)
                    shutil.copy2(source, target)
            atomic_json(staging / '.complete.json', {'identity': identity})
            if destination.exists():
                shutil.rmtree(destination)
            os.replace(staging, destination)
        return destination / 'wasi'

    def prepare(self, manifest: dict, install: bool = False) -> dict[str, str]:
        platform = manifest.get('platforms', {}).get('windows-x64')
        if not platform:
            raise Failure('unsupported_platform', 'SDK has no Windows x64 toolchain', 4)
        sdk = self.asset(manifest['sdk'], install)
        wasi = self.asset(platform['wasi'], install)
        if platform.get('msvc_crt'):
            crt = self.asset(platform['msvc_crt'], install)
            wasi = self.compose_wasi(wasi, crt, platform, install)
        riscv = self.asset(platform['wamrc_riscv'], install)
        xtensa = self.asset(platform['wamrc_xtensa'], install)
        paths = {'sdk': str(sdk), 'WASI_SDK_PATH': str(wasi),
                 'WAMRC': str(riscv / 'wamrc.exe'), 'XTENSA_WAMRC': str(xtensa / 'wamrc.exe')}
        for path in (sdk / 'micropixel', wasi / 'bin/clang++.exe', Path(paths['WAMRC']), Path(paths['XTENSA_WAMRC'])):
            if not path.is_file():
                raise Failure('dependency_corrupt', f'Required tool is missing: {path.name}', 4)
        return paths

    def lock(self, project: Path) -> dict:
        path = project / LOCK_NAME
        if not path.is_file():
            raise Failure('project_unlocked', 'Project needs an explicit version: micropixel sdk use <version> --yes', 3)
        lock = read_json(path)
        if lock.get('schema_version') != 1:
            raise Failure('incompatible_lock', 'Unsupported project lock format; --yes cannot override it', 4)
        identifier(lock.get('sdk_version'))
        identifier(lock.get('toolchain_id'))
        if not SHA256.fullmatch(lock.get('manifest_sha256', '')):
            raise Failure('invalid_lock', 'Project lock requires a manifest digest', 4)
        return lock

    def switch(self, project: Path, version: str) -> dict:
        # Validate an existing lock before downloads; never overwrite unknown formats.
        if (project / LOCK_NAME).exists():
            self.lock(project)
        with install_lock(self.root):
            manifest, checksum = self.manifest(version)
            self.prepare(manifest, install=True)
            lock = {'schema_version': 1, 'sdk_version': version, 'toolchain_id': manifest['toolchain_id'],
                    'manifest_sha256': checksum, 'external_toolchain': False}
            atomic_json(project / LOCK_NAME, lock)
        return lock

    def status(self, current: str | None, *, ttl: int = 86400, force: bool = False) -> dict:
        index, state, checked = self.index(ttl, force)
        candidate = index.get('stable')
        result = {'current_version': current, 'candidate_version': candidate, 'check_status': state,
                  'checked_at': checked, 'release_notes_url': None, 'next_command': None}
        if isinstance(candidate, str) and newer(candidate, current):
            entry = index.get('versions', {}).get(candidate, {})
            result.update(release_notes_url=entry.get('release_notes_url'), next_command='micropixel sdk upgrade --yes --json')
            self.warnings.append({'code': 'sdk_update_available', **result})
            for improvement in entry.get('performance', []):
                if isinstance(improvement, dict) and all(improvement.get(k) for k in ('description', 'devices', 'firmware', 'evidence_url')):
                    self.warnings.append({'code': 'sdk_performance_improvement', **improvement})
        return result


def load_cli(path: Path):
    loader = importlib.machinery.SourceFileLoader('micropixel_pinned_cli', str(path))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    loader.exec_module(module)
    return module


def required_yes(yes: bool) -> None:
    if not yes:
        raise Failure('input_required', 'This command requires explicit --yes; no hidden prompts are used', 3)


def execute(manager: Manager, arguments: list[str], yes: bool, json_mode: bool) -> tuple[int, dict]:
    if not arguments:
        raise Failure('invalid_arguments', 'Expected setup, doctor, sdk, update, or an SDK command', 2)
    command = arguments[0]
    if command == 'manager-version':
        import serial
        return 0, {'manager_version': VERSION, 'build_id': BUILD_ID, 'python_version': sys.version.split()[0], 'pyserial_version': serial.__version__}
    project = Path.cwd()
    if command in ('setup', 'doctor', 'sdk', 'update'):
        parser = argparse.ArgumentParser(prog='micropixel ' + command)
        parser.add_argument('--project', type=Path, default=project)
        parser.add_argument('--check', action='store_true')
        parser.add_argument('--version')
        parser.add_argument('--external-toolchain', action='store_true')
        if command == 'sdk':
            parser.add_argument('action', choices=['status', 'use', 'upgrade'])
            parser.add_argument('version', nargs='?')
        args = parser.parse_args(arguments[1:])
        project = args.project.resolve()
        if command == 'setup':
            required_yes(yes)
            index = manager.index()[0]
            version = args.version or index.get('stable') or index.get('preview')
            if not version:
                raise Failure('version_unavailable', 'No SDK release is available; retry with connectivity', 4)
            with install_lock(manager.root):
                manifest, checksum = manager.manifest(version, index)
                paths = manager.prepare(manifest, install=True)
                atomic_json(manager.root / 'default.json', {'sdk_version': version, 'manifest_sha256': checksum})
            return 0, {'dependencies_prepared': True, 'sdk_version': version, 'toolchain_id': manifest['toolchain_id'], 'paths': paths}
        if command == 'sdk':
            lock = manager.lock(project) if (project / LOCK_NAME).exists() else None
            if args.action == 'status':
                return 0, {'lock': lock, **manager.status(lock['sdk_version'] if lock else None, force=args.check)}
            required_yes(yes)
            version = args.version if args.action == 'use' else manager.index(force=True)[0].get('stable')
            if not version:
                raise Failure('input_required', 'Specify an SDK version or configure a stable release', 3)
            if args.external_toolchain:
                if not lock or version != lock['sdk_version']:
                    raise Failure('invalid_arguments', 'External mode requires the currently locked version', 2)
                lock['external_toolchain'] = True
                atomic_json(project / LOCK_NAME, lock)
                return 0, {'lock': lock}
            return 0, {'lock': manager.switch(project, version)}
        if command == 'doctor':
            if not (project / LOCK_NAME).exists() and not (manager.root / 'default.json').exists():
                raise Failure('environment_not_ready', 'Run micropixel setup --yes first', 4)
            lock = manager.lock(project) if (project / LOCK_NAME).exists() else read_json(manager.root / 'default.json')
            manifest, _ = manager.manifest(lock['sdk_version'], expected=lock['manifest_sha256'])
            if lock.get('external_toolchain'):
                if not all(os.environ.get(k) for k in ('WASI_SDK_PATH', 'WAMRC', 'XTENSA_WAMRC')):
                    raise Failure('dependency_missing', 'External toolchain variables are incomplete', 4)
                paths = {k: os.environ[k] for k in ('WASI_SDK_PATH', 'WAMRC', 'XTENSA_WAMRC')}
            else:
                paths = manager.prepare(manifest)
            if lock.get('toolchain_id', manifest['toolchain_id']) != manifest['toolchain_id']:
                raise Failure('incompatible_lock', 'Lock toolchain ID differs from manifest', 4)
            try:
                import serial
                serial_version = serial.__version__
            except ImportError as error:
                raise Failure('dependency_missing', 'Bundled pyserial is missing', 4) from error
            probes = {}
            for name, path in [('clang', str(Path(paths['WASI_SDK_PATH']) / 'bin/clang++.exe')), ('riscv', paths['WAMRC']), ('xtensa', paths['XTENSA_WAMRC'])]:
                probe = subprocess.run([path, '--version'], capture_output=True, text=True, timeout=15)
                if probe.returncode:
                    raise Failure('tool_unusable', f'{name} could not execute', 4)
                probes[name] = (probe.stdout or probe.stderr).splitlines()[0]
            return 0, {'ready': True, 'sdk_version': lock['sdk_version'], 'toolchain_id': 'external' if lock.get('external_toolchain') else manifest['toolchain_id'], 'tools': probes, 'pyserial': serial_version, 'manager_version': VERSION, 'manager_build_id': BUILD_ID}
        if command == 'update':
            index, state, checked = manager.index(force=True)
            candidate = index.get('manager')
            result = {'current_version': VERSION, 'current_build_id': BUILD_ID, 'candidate_build_id': candidate.get('build_id') if candidate else None, 'candidate_version': candidate.get('version') if candidate else None,
                      'check_status': state, 'checked_at': checked}
            if args.check or not candidate or not (newer(candidate['version'], VERSION) or (candidate['version'] == VERSION and candidate.get('build_id') != BUILD_ID)):
                return 0, result
            required_yes(yes)
            if manager.offline:
                raise Failure('dependency_missing', 'Manager update needs an online check', 4)
            # Manager packages include Python and pyserial. Launcher reads current.json
            # at every invocation; the running process keeps its original directory.
            with install_lock(manager.root):
                installed = manager.asset(candidate, install=True)
                if not (installed / 'python/python.exe').is_file() or not (installed / 'micropixel_manager.py').is_file():
                    raise Failure('invalid_archive', 'Manager runtime is incomplete', 4)
                probe = subprocess.run([str(installed / 'python/python.exe'), '-I', '-X', 'utf8', str(installed / 'micropixel_manager.py'), 'manager-version', '--json'], capture_output=True, text=True, timeout=20)
                info = json.loads(probe.stdout)
                if probe.returncode or not info.get('ok') or info['result'].get('manager_version') != candidate['version'] or info['result'].get('build_id') != candidate.get('build_id'):
                    raise Failure('invalid_archive', 'Updated manager runtime failed its self-check', 4)
                atomic_json(manager.root / 'current.json', {'directory': str(installed)})
            return 0, {**result, 'updated': True}
    parser_path = Path(__file__).with_name('bootstrap-cli.py')
    if not parser_path.exists():
        parser_path = Path(__file__).parents[1] / 'micropixel'  # Source checkout.
    cli = load_cli(parser_path)
    parsed = cli.parser().parse_args(arguments)
    command = parsed.command
    value = getattr(parsed, 'project', getattr(parsed, 'directory', getattr(parsed, 'source', '.')))
    project = Path(value).resolve()
    if project.is_file():
        project = project.parent
    if command in ('build', 'package', 'publish', 'run') or (project / LOCK_NAME).exists():
        lock = manager.lock(project)
    else:
        if not (manager.root / 'default.json').exists():
            raise Failure('environment_not_ready', 'Run micropixel setup --yes first', 4)
        lock = read_json(manager.root / 'default.json')
    manifest, checksum = manager.manifest(lock['sdk_version'], expected=lock['manifest_sha256'])
    if lock.get('toolchain_id', manifest['toolchain_id']) != manifest['toolchain_id']:
        raise Failure('incompatible_lock', 'Lock toolchain ID differs from manifest', 4)
    status = manager.status(lock['sdk_version'], ttl=300 if command in ('package', 'publish') else 86400) if command in ('build', 'run', 'package', 'publish') else None
    env = os.environ.copy()
    if lock.get('external_toolchain'):
        if not all(env.get(k) for k in ('WASI_SDK_PATH', 'WAMRC', 'XTENSA_WAMRC')):
            raise Failure('dependency_missing', 'External mode requires explicit WASI_SDK_PATH, WAMRC and XTENSA_WAMRC', 4)
        sdk = manager.asset(manifest['sdk'])
        manager.warnings.append({'code': 'external_toolchain', 'message': 'External toolchain is not verified by the managed manifest'})
    else:
        paths = manager.prepare(manifest)
        sdk = Path(paths.pop('sdk'))
        for key in ('WASI_CLANGXX', 'WASI_CLANG', 'WAMRC', 'XTENSA_WAMRC', 'WASI_SDK_PATH'):
            env.pop(key, None)
        env.update(paths)
        # Empty overrides also mask project .env values consumed by the SDK CLI.
        env['WASI_CLANG'] = ''
        env['WASI_CLANGXX'] = str(Path(paths['WASI_SDK_PATH']) / 'bin/clang++.exe')
    env['MICROPIXEL_TOOLCHAIN_ID'] = manifest['toolchain_id'] if not lock.get('external_toolchain') else 'external'
    if status and not json_mode:
        seen_path = manager.root / 'notices' / (digest(str(project).encode()) + '.json')
        seen = read_json(seen_path) if seen_path.exists() else {}
        if command in ('package', 'publish') or seen.get('candidate') != status['candidate_version']:
            print(json.dumps(status, ensure_ascii=False), file=sys.stderr)
            atomic_json(seen_path, {'candidate': status['candidate_version']})
    child_arguments = list(arguments)
    if json_mode:
        child_arguments.insert(child_arguments.index('--') if '--' in child_arguments else len(child_arguments), '--json')
    result = subprocess.run([sys.executable, '-X', 'utf8', str(sdk / 'micropixel'), *child_arguments],
                            env=env, stdout=subprocess.PIPE if json_mode else None, text=True, encoding='utf-8')
    if json_mode:
        try:
            envelope = json.loads(result.stdout)
        except ValueError as error:
            raise Failure('invalid_cli_output', 'Pinned SDK does not support this JSON command', 4) from error
        if envelope.get('schema_version') != 1:
            raise Failure('incompatible_cli', 'Pinned SDK JSON format is unsupported', 4)
    else:
        envelope = {'result': {}, 'warnings': [], 'error': None, 'code': 'ok' if result.returncode == 0 else 'execution_failed'}
    if command == 'init' and result.returncode == 0:
        atomic_json(project / LOCK_NAME, {'schema_version': 1, 'sdk_version': manifest['sdk_version'],
                    'toolchain_id': manifest['toolchain_id'], 'manifest_sha256': checksum, 'external_toolchain': False})
    envelope['result']['version_status'] = status
    return result.returncode, envelope


def main() -> int:
    arguments = sys.argv[1:]
    boundary = arguments.index('--') if '--' in arguments else len(arguments)
    flags = arguments[:boundary]
    json_mode, yes, offline = '--json' in flags, '--yes' in flags, '--offline' in flags
    if json_mode:
        for stream in (sys.stdout, sys.stderr):
            if hasattr(stream, 'reconfigure'):
                stream.reconfigure(encoding='utf-8')
    arguments = [a for a in flags if a not in ('--json', '--yes', '--offline')] + arguments[boundary:]
    root = Path(os.environ.get('MICROPIXEL_HOME', str(Path(os.environ.get('LOCALAPPDATA', Path.home() / '.local/share')) / 'MicroPixel')))
    manager = Manager(root, offline=offline)
    code, error, result = 1, None, {}
    try:
        with contextlib.redirect_stdout(sys.stderr) if json_mode else contextlib.nullcontext():
            code, result = execute(manager, arguments, yes, json_mode)
    except Failure as failure:
        code, error = failure.exit_code, {'code': failure.code, 'message': str(failure)}
    except SystemExit as failure:
        code = int(failure.code or 0)
        error = {'code': 'invalid_arguments', 'message': 'See stderr for usage'} if code else None
    except (TypeError, AttributeError) as failure:
        code, error = 4, {'code': 'invalid_metadata', 'message': str(failure)}
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as failure:
        code, error = 1, {'code': 'execution_failed', 'message': str(failure)}
    except KeyboardInterrupt:
        code, error = 1, {'code': 'interrupted', 'message': 'Operation interrupted; retry is safe'}
    if 'result' in result and 'code' in result:
        envelope = result
        envelope.update(schema_version=1, ok=code == 0)
        envelope['warnings'] = envelope.get('warnings', []) + manager.warnings
    else:
        envelope = {'schema_version': 1, 'ok': code == 0, 'code': error['code'] if error else 'ok',
                    'result': result, 'error': error, 'warnings': manager.warnings}
    if json_mode:
        print(json.dumps(envelope, ensure_ascii=False))
    elif error:
        print('micropixel: ' + error['message'], file=sys.stderr)
    elif result:
        print(json.dumps(result, ensure_ascii=False, indent=2))
    return code


if __name__ == '__main__':
    raise SystemExit(main())
