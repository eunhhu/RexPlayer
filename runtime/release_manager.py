#!/usr/bin/env python3
"""Offline Linux user-space release transactions. Unsigned local preview only.

No downloads, elevation, service/kernel/network changes, or user-data migrations.
Health checks and launch run local executable code only after explicit consent.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import signal
import stat
import subprocess
import sys
import tarfile
import time
import uuid

MAX_ARCHIVE = 4 * 1024**3
MAX_UNPACKED = 8 * 1024**3
MAX_FILES = 20000
MAX_JSON = 8 * 1024**2
CHANNEL = 'unsigned-local-preview'
VERSION_RE = re.compile(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?\Z')
RELEASE_RE = re.compile(r'[0-9A-Za-z.+-]{1,64}-[a-f0-9]{64}\Z')
TX_RE = re.compile(r'[a-f0-9]{32}\Z')
MARKER = {'schema_version': 1, 'kind': 'rexplayer-offline-user-install'}
EMPTY_STATE = {'schema_version': 1, 'generation': 0, 'current': None, 'previous': None}


class ReleaseError(Exception):
    """An operation failed without authorizing changes outside the install root."""


def host_platform() -> dict:
    machine = platform.machine().lower()
    return {'os': platform.system().lower(),
            'architecture': {'amd64': 'x86_64', 'arm64': 'aarch64'}.get(machine, machine)}


def canonical(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=True) + '\n').encode()


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def require_keys(value: object, keys: set[str], label: str) -> None:
    if not isinstance(value, dict) or set(value) != keys:
        raise ReleaseError(f'{label}: invalid or unknown fields')


def positive_integer(value: object, maximum: int, label: str, *, zero: bool = False) -> None:
    if type(value) is not int or not (0 if zero else 1) <= value <= maximum:
        raise ReleaseError(f'{label}: invalid size or limit')


def safe_name(value: object) -> str:
    """Portable archive names: no links, dot paths, control chars, or drive names."""
    if not isinstance(value, str) or not value or len(value) > 1024:
        raise ReleaseError('invalid relative archive path')
    if any(ord(c) < 32 or ord(c) == 127 for c in value) or '\\' in value or ':' in value:
        raise ReleaseError('invalid characters in archive path')
    parts = value.split('/')
    if any(p in ('', '.', '..') or p.endswith((' ', '.')) for p in parts):
        raise ReleaseError('invalid relative archive path')
    if PurePosixPath(value).is_absolute():
        raise ReleaseError('absolute archive path')
    return value


def validate_version(value: object) -> str:
    if not isinstance(value, str) or len(value) > 64:
        raise ReleaseError('invalid release version')
    match = VERSION_RE.fullmatch(value)
    if match is None or any(part.isdigit() and len(part) > 1 and part.startswith('0')
                            for part in (match.group(4) or '').split('.')):
        raise ReleaseError('invalid semantic release version')
    return value


def validate_manifest(value: object, *, check_host: bool = True) -> dict:
    require_keys(value, {'schema_version', 'channel', 'version', 'platform', 'archive',
                         'files', 'healthcheck', 'entrypoints', 'user_data_schema'}, 'manifest')
    if type(value['schema_version']) is not int or value['schema_version'] != 1:
        raise ReleaseError('unsupported release manifest schema')
    if value['channel'] != CHANNEL:
        raise ReleaseError('only unsigned-local-preview manifests are supported; no signature is verified')
    validate_version(value['version'])
    if type(value['user_data_schema']) is not int or value['user_data_schema'] != 1:
        raise ReleaseError('incompatible user-data schema; migrations are not implemented')
    require_keys(value['platform'], {'os', 'architecture'}, 'platform')
    if value['platform']['os'] != 'linux' or not isinstance(value['platform']['architecture'], str) or value['platform']['architecture'] not in {'x86_64', 'aarch64'}:
        raise ReleaseError('only Linux x86_64/aarch64 local previews are supported')
    if check_host and value['platform'] != host_platform():
        raise ReleaseError('manifest platform does not match this host')
    require_keys(value['archive'], {'sha256', 'size', 'unpacked_size'}, 'archive')
    positive_integer(value['archive']['size'], MAX_ARCHIVE, 'archive size')
    positive_integer(value['archive']['unpacked_size'], MAX_UNPACKED, 'unpacked size')
    if not isinstance(value['archive']['sha256'], str) or not re.fullmatch('[0-9a-f]{64}', value['archive']['sha256']):
        raise ReleaseError('invalid archive sha256')
    if not isinstance(value['files'], list) or not 1 <= len(value['files']) <= MAX_FILES:
        raise ReleaseError('invalid file count')
    names, folded, total = {}, set(), 0
    for item in value['files']:
        require_keys(item, {'path', 'sha256', 'size', 'executable'}, 'file')
        name = safe_name(item['path'])
        if name in names or name.casefold() in folded:
            raise ReleaseError('duplicate or case-colliding manifest path')
        positive_integer(item['size'], MAX_UNPACKED, 'file size', zero=True)
        if not isinstance(item['sha256'], str) or not re.fullmatch('[0-9a-f]{64}', item['sha256']):
            raise ReleaseError('invalid file sha256')
        if type(item['executable']) is not bool:
            raise ReleaseError('executable must be boolean')
        names[name] = item
        folded.add(name.casefold())
        total += item['size']
    for name in names:
        if any(str(parent).casefold() in folded for parent in PurePosixPath(name).parents if str(parent) != '.'):
            raise ReleaseError('file path conflicts with a parent directory')
    if total != value['archive']['unpacked_size']:
        raise ReleaseError('unpacked size does not equal manifest file sizes')
    require_keys(value['healthcheck'], {'argv', 'timeout_seconds'}, 'healthcheck')
    argv = value['healthcheck']['argv']
    if not isinstance(argv, list) or not 1 <= len(argv) <= 64 or any(
            not isinstance(arg, str) or '\0' in arg or len(arg) > 4096 for arg in argv):
        raise ReleaseError('invalid healthcheck argv')
    safe_name(argv[0])
    if argv[0] not in names or not names[argv[0]]['executable']:
        raise ReleaseError('healthcheck must name an executable manifest file')
    positive_integer(value['healthcheck']['timeout_seconds'], 120, 'healthcheck timeout')
    entries = value['entrypoints']
    if not isinstance(entries, dict) or not entries or len(entries) > 16:
        raise ReleaseError('invalid entrypoints')
    for name, path in entries.items():
        if not isinstance(name, str) or not re.fullmatch('[a-z][a-z0-9-]{0,31}', name):
            raise ReleaseError('invalid entrypoint name')
        safe_name(path)
        if path not in names or not names[path]['executable']:
            raise ReleaseError('entrypoint must name an executable manifest file')
    return value


def _regular_fd(path: Path):
    flags = os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0) | getattr(os, 'O_NONBLOCK', 0)
    try:
        before = path.lstat()
        if not stat.S_ISREG(before.st_mode):
            raise ReleaseError(f'not a regular file: {path.name}')
        fd = os.open(path, flags)
        opened = os.fstat(fd)
        if not stat.S_ISREG(opened.st_mode) or (opened.st_dev, opened.st_ino) != (before.st_dev, before.st_ino):
            os.close(fd)
            raise ReleaseError(f'file changed while opening: {path.name}')
        return os.fdopen(fd, 'rb')
    except OSError as error:
        raise ReleaseError(f'cannot read regular file {path.name}: {error.strerror}') from error


def read_json(path: Path) -> dict:
    with _regular_fd(path) as source:
        raw = source.read(MAX_JSON + 1)
    if len(raw) > MAX_JSON:
        raise ReleaseError('JSON file is too large')
    def no_duplicates(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ReleaseError('duplicate JSON key')
            result[key] = value
        return result
    try:
        return json.loads(raw, object_pairs_hook=no_duplicates)
    except (UnicodeError, ValueError) as error:
        raise ReleaseError('invalid JSON') from error


def _sync_dir(path: Path) -> None:
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def _write_json(path: Path, value: dict) -> None:
    temporary = path.parent / ('.tmp-' + uuid.uuid4().hex)
    try:
        with temporary.open('xb') as stream:
            os.chmod(temporary, 0o600)
            stream.write(canonical(value))
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        _sync_dir(path.parent)
    finally:
        temporary.unlink(missing_ok=True)


def _check_dirs(path: Path) -> None:
    for part in [*reversed(path.parents), path]:
        try:
            info = part.lstat()
        except FileNotFoundError:
            continue
        if not stat.S_ISDIR(info.st_mode):
            raise ReleaseError(f'install path contains a symlink or non-directory: {part}')


def _sha_stream(stream, maximum: int) -> tuple[str, int]:
    hasher, count = hashlib.sha256(), 0
    while chunk := stream.read(1024 * 1024):
        count += len(chunk)
        if count > maximum:
            raise ReleaseError('file exceeds declared size')
        hasher.update(chunk)
    return hasher.hexdigest(), count


def release_id(manifest: dict) -> str:
    return manifest['version'] + '-' + digest(canonical(manifest))


class _PlainTarInfo(tarfile.TarInfo):
    def _proc_member(self, archive):
        # Reject metadata pseudo-members BEFORE tarfile can allocate their declared
        # body or interpret sparse/PAX/GNU extensions. Our package emits plain
        # regular-file headers even though it uses the PAX writer format.
        if self.type not in (tarfile.REGTYPE, tarfile.AREGTYPE):
            raise ReleaseError('only plain regular-file tar headers are supported')
        positive_integer(self.size, MAX_UNPACKED, 'tar header size', zero=True)
        return super()._proc_member(archive)


def archive_inventory(path: Path) -> list[dict]:
    """Create a manifest inventory; never extracts or runs archive contents."""
    files, seen, count = [], set(), 0
    with _regular_fd(path) as source, tarfile.open(fileobj=source, mode='r|gz', tarinfo=_PlainTarInfo) as archive:
        for member in archive:
            name = safe_name(member.name)
            if not member.isfile() or member.issparse() or member.linkname or member.mode & 0o7000:
                raise ReleaseError('only ordinary files with safe mode bits are permitted')
            if name in seen or len(files) >= MAX_FILES:
                raise ReleaseError('duplicate archive member or too many files')
            positive_integer(member.size, MAX_UNPACKED, 'archive member size', zero=True)
            count += member.size
            if count > MAX_UNPACKED:
                raise ReleaseError('archive exceeds unpacked size limit')
            stream = archive.extractfile(member)
            checksum, size = _sha_stream(stream, member.size)
            if size != member.size:
                raise ReleaseError('truncated archive member')
            files.append({'path': name, 'sha256': checksum, 'size': size,
                          'executable': bool(member.mode & 0o111)})
            seen.add(name)
    return sorted(files, key=lambda item: item['path'])


def create_manifest(archive: Path, version: str, *, entrypoints: dict | None = None,
                    health_argv: list[str] | None = None, timeout: int = 10) -> dict:
    with _regular_fd(archive) as source:
        checksum, size = _sha_stream(source, MAX_ARCHIVE)
    files = archive_inventory(archive)
    return validate_manifest({'schema_version': 1, 'channel': CHANNEL, 'version': version,
        'platform': host_platform(), 'archive': {'sha256': checksum, 'size': size,
            'unpacked_size': sum(item['size'] for item in files)}, 'files': files,
        'healthcheck': {'argv': health_argv or ['bin/rex-launcher', '--help'], 'timeout_seconds': timeout},
        'entrypoints': entrypoints or {'launcher': 'bin/rex-launcher'}, 'user_data_schema': 1})


class ReleaseManager:
    def __init__(self, root: Path):
        # abspath is deliberate: resolve() would hide a supplied symlink.
        self.root = Path(os.path.abspath(root))
        self.versions = self.root / 'versions'
        self.staging = self.root / 'staging'
        self.state_path = self.root / 'state.json'
        self.journal_path = self.root / 'journal.json'

    @contextmanager
    def _locked(self):
        if host_platform()['os'] != 'linux':
            raise ReleaseError('release transactions currently support Linux only')
        import fcntl
        _check_dirs(self.root)
        self.root.mkdir(parents=True, exist_ok=True, mode=0o700)
        if self.root.stat().st_uid != os.getuid():
            raise ReleaseError('install root must be owned by the current user')
        if self.root.stat().st_mode & 0o022:
            raise ReleaseError('install root must not be writable by group or others')
        lock_path = self.root / '.lock'
        fd = os.open(lock_path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600)
        try:
            if not stat.S_ISREG(os.fstat(fd).st_mode):
                raise ReleaseError('invalid install lock')
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as error:
                raise ReleaseError('another release operation holds the install lock') from error
            marker = self.root / 'installation.json'
            if marker.exists():
                if read_json(marker) != MARKER:
                    raise ReleaseError('unrecognized installation root')
            else:
                unexpected = [p.name for p in self.root.iterdir() if p.name != '.lock']
                if unexpected:
                    raise ReleaseError('refusing to adopt a nonempty unrecognized installation root')
                _write_json(marker, MARKER)
            for path in (self.versions, self.staging, self.root / 'user-data'):
                _check_dirs(path)
                path.mkdir(exist_ok=True, mode=0o700)
            if not os.path.lexists(self.state_path):
                # A missing state is only recoverable during empty initialization.
                # Never invent an empty pointer for an existing installed tree.
                if os.path.lexists(self.journal_path) or any(
                        any(path.iterdir()) for path in (self.versions, self.staging, self.root / 'user-data')):
                    raise ReleaseError('installation state is missing; refusing to reset existing release/data pointers')
                _write_json(self.state_path, EMPTY_STATE)
            yield
        finally:
            os.close(fd)

    @staticmethod
    def _validate_state(state: dict) -> dict:
        require_keys(state, {'schema_version', 'generation', 'current', 'previous'}, 'state')
        if type(state['schema_version']) is not int or state['schema_version'] != 1:
            raise ReleaseError('unsupported state schema')
        positive_integer(state['generation'], 2**63 - 1, 'state generation', zero=True)
        for field in ('current', 'previous'):
            if state[field] is not None and (not isinstance(state[field], str) or not RELEASE_RE.fullmatch(state[field])):
                raise ReleaseError('invalid release pointer')
        if state['current'] is None and state['previous'] is not None:
            raise ReleaseError('previous release without current release')
        if state['current'] is not None and state['current'] == state['previous']:
            raise ReleaseError('current and previous release must differ')
        return state

    def _state(self) -> dict:
        return self._validate_state(read_json(self.state_path))

    def _version(self, identifier: str) -> Path:
        if not isinstance(identifier, str) or not RELEASE_RE.fullmatch(identifier):
            raise ReleaseError('invalid release id')
        path = self.versions / identifier
        _check_dirs(path)
        if not path.is_dir():
            raise ReleaseError('release is not staged')
        return path

    def _verify(self, path: Path, identifier: str) -> dict:
        manifest = validate_manifest(read_json(path / 'manifest.json'))
        if release_id(manifest) != identifier:
            raise ReleaseError('stored manifest does not match release identity')
        payload = path / 'payload'
        _check_dirs(payload)
        expected = {item['path']: item for item in manifest['files']}
        found = set()
        for directory, subdirs, filenames in os.walk(payload, followlinks=False):
            for name in subdirs:
                if not stat.S_ISDIR((Path(directory) / name).lstat().st_mode):
                    raise ReleaseError('symlink or invalid directory in release')
            for name in filenames:
                file_path = Path(directory) / name
                relative = file_path.relative_to(payload).as_posix()
                if relative not in expected:
                    raise ReleaseError('unexpected file in release')
                item = expected[relative]
                with _regular_fd(file_path) as stream:
                    checksum, size = _sha_stream(stream, item['size'])
                    mode = stat.S_IMODE(os.fstat(stream.fileno()).st_mode)
                if checksum != item['sha256'] or size != item['size'] or mode != (0o755 if item['executable'] else 0o644):
                    raise ReleaseError(f'installed file failed integrity check: {relative}')
                found.add(relative)
        if found != set(expected):
            raise ReleaseError('release is missing manifest files')
        return manifest

    def _journal(self, value: dict) -> None:
        _write_json(self.journal_path, value)

    def _clear_journal(self) -> None:
        self.journal_path.unlink(missing_ok=True)
        _sync_dir(self.root)

    def _remove_stage(self, name: str) -> None:
        if not isinstance(name, str) or not TX_RE.fullmatch(name):
            raise ReleaseError('invalid staging transaction id')
        path = self.staging / name
        if os.path.lexists(path):
            _check_dirs(path)
            shutil.rmtree(path)
            _sync_dir(self.staging)

    def _recover(self) -> str | None:
        if not os.path.lexists(self.journal_path):
            return None
        journal = read_json(self.journal_path)
        if not isinstance(journal, dict) or type(journal.get('schema_version')) is not int or journal.get('schema_version') != 1 or not isinstance(journal.get('operation'), str):
            raise ReleaseError('invalid recovery journal; no automatic recovery is safe')
        if journal.get('operation') == 'stage':
            require_keys(journal, {'schema_version', 'operation', 'transaction', 'release_id'}, 'stage journal')
            identifier = journal['release_id']
            if not isinstance(identifier, str) or not RELEASE_RE.fullmatch(identifier):
                raise ReleaseError('invalid release id in journal')
            target = self.versions / identifier
            if os.path.lexists(target):
                self._verify(self._version(identifier), identifier)
            self._remove_stage(journal['transaction'])
            result = 'staged_release_retained' if target.exists() else 'incomplete_stage_removed'
        elif journal.get('operation') in {'activate', 'rollback'}:
            require_keys(journal, {'schema_version', 'operation', 'phase', 'before', 'after'}, 'activation journal')
            before = self._validate_state(journal['before'])
            after = self._validate_state(journal['after'])
            if journal['phase'] not in {'checking', 'health_passed'} or after['generation'] != before['generation'] + 1:
                raise ReleaseError('invalid activation journal transition')
            if after['current'] is None or after['previous'] != before['current']:
                raise ReleaseError('invalid activation journal pointers')
            state = self._state()
            if state == before:
                result = 'uncommitted_activation_aborted'
            elif state == after and journal['phase'] == 'health_passed':
                self._verify(self._version(after['current']), after['current'])
                result = 'committed_activation_retained'
            else:
                raise ReleaseError('state disagrees with recovery journal; refusing to guess')
        else:
            raise ReleaseError('unsupported recovery journal operation')
        self._clear_journal()
        return result

    def recover(self) -> dict:
        with self._locked():
            result = self._recover()
            return {'recovery': result or 'nothing_to_recover', 'state': self._state()}

    def stage(self, manifest_path: Path, archive_path: Path, *, allow_unsigned: bool = False) -> str:
        if not allow_unsigned:
            raise ReleaseError('unsigned local archive requires --allow-unsigned; hashes do not authenticate its publisher')
        manifest = validate_manifest(read_json(manifest_path))
        identifier = release_id(manifest)
        with self._locked():
            self._recover()
            destination = self.versions / identifier
            if os.path.lexists(destination):
                self._verify(self._version(identifier), identifier)
                return identifier
            transaction = uuid.uuid4().hex
            stage = self.staging / transaction
            self._journal({'schema_version': 1, 'operation': 'stage', 'transaction': transaction, 'release_id': identifier})
            try:
                stage.mkdir(mode=0o700)
                payload = stage / 'payload'
                payload.mkdir(mode=0o700)
                _write_json(stage / 'manifest.json', manifest)
                expected = {item['path']: item for item in manifest['files']}
                seen = set()
                # Verify and extract through the SAME descriptor: no path replacement window.
                with _regular_fd(archive_path) as source:
                    checksum, size = _sha_stream(source, manifest['archive']['size'])
                    if size != manifest['archive']['size'] or checksum != manifest['archive']['sha256']:
                        raise ReleaseError('archive digest or size mismatch')
                    source.seek(0)
                    with tarfile.open(fileobj=source, mode='r|gz', tarinfo=_PlainTarInfo) as archive:
                        for member in archive:
                            name = safe_name(member.name)
                            if not member.isfile() or member.issparse() or member.linkname or member.mode & 0o7000:
                                raise ReleaseError('unsafe archive member type or mode')
                            if name in seen or name not in expected:
                                raise ReleaseError('duplicate or unexpected archive file')
                            item = expected[name]
                            if member.size != item['size'] or bool(member.mode & 0o111) != item['executable']:
                                raise ReleaseError('archive member differs from manifest')
                            target = payload.joinpath(*name.split('/'))
                            target.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
                            stream = archive.extractfile(member)
                            hasher, copied = hashlib.sha256(), 0
                            with target.open('xb') as output:
                                while chunk := stream.read(1024 * 1024):
                                    copied += len(chunk)
                                    if copied > item['size']:
                                        raise ReleaseError('archive member exceeds size limit')
                                    hasher.update(chunk)
                                    output.write(chunk)
                                output.flush()
                                os.fchmod(output.fileno(), 0o755 if item['executable'] else 0o644)
                                os.fsync(output.fileno())
                            if copied != item['size'] or hasher.hexdigest() != item['sha256']:
                                raise ReleaseError('archive member digest or size mismatch')
                            seen.add(name)
                if seen != set(expected):
                    raise ReleaseError('archive is missing manifest files')
                self._verify(stage, identifier)
                for directory, _, _ in os.walk(stage, topdown=False):
                    _sync_dir(Path(directory))
                os.rename(stage, destination)
                _sync_dir(self.versions)
                _sync_dir(self.staging)
                self._clear_journal()
                return identifier
            except Exception:
                self._remove_stage(transaction)
                self._clear_journal()
                raise

    def _health(self, path: Path, manifest: dict) -> None:
        argv = manifest['healthcheck']['argv']
        command = [str(path / 'payload' / argv[0]), *argv[1:]]
        process = subprocess.Popen(command, cwd=path / 'payload', stdin=subprocess.DEVNULL,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        try:
            deadline = time.monotonic() + manifest['healthcheck']['timeout_seconds']
            while True:
                # Observe but do not reap the group leader. Keeping its PID owned
                # prevents group-ID reuse before descendant cleanup below.
                result = os.waitid(os.P_PID, process.pid, os.WEXITED | os.WNOHANG | os.WNOWAIT)
                if result is not None:
                    code = result.si_status if result.si_code == os.CLD_EXITED else -result.si_status
                    if code != 0:
                        raise ReleaseError(f'release healthcheck failed (exit {code}); current release is unchanged')
                    break
                if time.monotonic() >= deadline:
                    raise ReleaseError('release healthcheck timed out; current release is unchanged')
                time.sleep(0.02)
        finally:
            # Reap only AFTER killing the still-owned group, including descendants.
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired as error:
                raise ReleaseError('healthcheck process cleanup did not finish; activation refused') from error

    def _switch(self, identifier: str, *, operation: str, allow_execution: bool) -> dict:
        if not allow_execution:
            raise ReleaseError('healthcheck executes bundled local code; requires --allow-execution')
        state = self._state()
        path = self._version(identifier)
        manifest = self._verify(path, identifier)
        if state['current'] == identifier:
            return state
        after = {'schema_version': 1, 'generation': state['generation'] + 1,
                 'current': identifier, 'previous': state['current']}
        self._validate_state(after)
        journal = {'schema_version': 1, 'operation': operation, 'phase': 'checking', 'before': state, 'after': after}
        self._journal(journal)
        try:
            self._health(path, manifest)
            self._verify(path, identifier)
            journal['phase'] = 'health_passed'
            self._journal(journal)
            _write_json(self.state_path, after)
            self._clear_journal()
        except Exception:
            # Never clear a possibly committed switch on an I/O failure; recovery
            # reconciles the atomic state pointer against the recorded intent.
            if self._state() == state:
                self._clear_journal()
            raise
        return after

    def activate(self, identifier: str, *, allow_execution: bool = False) -> dict:
        with self._locked():
            self._recover()
            return self._switch(identifier, operation='activate', allow_execution=allow_execution)

    def install(self, manifest_path: Path, archive_path: Path, *, allow_unsigned: bool = False,
                allow_execution: bool = False) -> dict:
        if not allow_execution:
            raise ReleaseError('install runs a bundled healthcheck; requires --allow-execution')
        identifier = self.stage(manifest_path, archive_path, allow_unsigned=allow_unsigned)
        return self.activate(identifier, allow_execution=allow_execution)

    def rollback(self, *, allow_execution: bool = False) -> dict:
        with self._locked():
            self._recover()
            previous = self._state()['previous']
            if previous is None:
                raise ReleaseError('no retained previous release')
            return self._switch(previous, operation='rollback', allow_execution=allow_execution)

    def status(self) -> dict:
        with self._locked():
            state = self._state()
            releases = []
            for path in sorted(self.versions.iterdir()):
                if RELEASE_RE.fullmatch(path.name):
                    manifest = self._verify(self._version(path.name), path.name)
                    releases.append({'id': path.name, 'version': manifest['version']})
                else:
                    raise ReleaseError('unexpected content in versions directory')
            for field in ('current', 'previous'):
                if state[field] is not None and state[field] not in {item['id'] for item in releases}:
                    raise ReleaseError('state references a missing release')
            return {'schema_version': 1, 'channel': CHANNEL, 'signature_verified': False,
                    'production_ready': False, 'state': state, 'releases': releases,
                    'recovery_pending': os.path.lexists(self.journal_path),
                    'user_data_path': str(self.root / 'user-data')}

    def run(self, entrypoint: str, arguments: list[str], *, allow_execution: bool = False) -> int:
        if not allow_execution:
            raise ReleaseError('launch executes unsigned local code; requires --allow-execution')
        with self._locked():
            self._recover()
            identifier = self._state()['current']
            if identifier is None:
                raise ReleaseError('no current release')
            path = self._version(identifier)
            manifest = self._verify(path, identifier)
            if entrypoint not in manifest['entrypoints']:
                raise ReleaseError('entrypoint is not present in the active release')
            command = [str(path / 'payload' / manifest['entrypoints'][entrypoint]), *arguments]
            # Versions are never deleted, so a later activation cannot invalidate
            # this process. Do not hold the updater lock for an interactive UI.
            environment = dict(os.environ, REXPLAYER_USER_DATA=str(self.root / 'user-data'))
            process = subprocess.Popen(command, env=environment)
        return process.wait()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path.home() / '.local/share/rexplayer/releases')
    commands = parser.add_subparsers(dest='command', required=True)
    make = commands.add_parser('manifest', help='write a new unsigned sidecar; does not execute archive code')
    make.add_argument('--archive', type=Path, required=True)
    make.add_argument('--output', type=Path, required=True)
    make.add_argument('--version', required=True)
    make.add_argument('--native-ui', action='store_true')
    for name in ('stage', 'install'):
        command = commands.add_parser(name)
        command.add_argument('--manifest', type=Path, required=True)
        command.add_argument('--archive', type=Path, required=True)
        command.add_argument('--allow-unsigned', action='store_true', help='trust this local archive; its publisher is NOT authenticated')
        if name == 'install':
            command.add_argument('--allow-execution', action='store_true', help='allow bundled executable healthcheck')
    activate = commands.add_parser('activate')
    activate.add_argument('release_id')
    activate.add_argument('--allow-execution', action='store_true')
    rollback = commands.add_parser('rollback')
    rollback.add_argument('--allow-execution', action='store_true')
    commands.add_parser('status')
    commands.add_parser('recover')
    run = commands.add_parser('run', help='verify and launch a named entrypoint from the active release')
    run.add_argument('--allow-execution', action='store_true')
    run.add_argument('entrypoint')
    run.add_argument('arguments', nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    try:
        manager = ReleaseManager(args.root)
        if args.command == 'manifest':
            entries = {'launcher': 'bin/rex-launcher'}
            if args.native_ui:
                entries['player'] = 'bin/rex-player'
            value = create_manifest(args.archive, args.version, entrypoints=entries)
            with args.output.open('xb') as output:
                output.write(canonical(value))
            result = {'manifest': str(args.output), 'release_id': release_id(value), 'signature_verified': False}
        elif args.command == 'stage':
            result = {'release_id': manager.stage(args.manifest, args.archive, allow_unsigned=args.allow_unsigned)}
        elif args.command == 'install':
            result = manager.install(args.manifest, args.archive, allow_unsigned=args.allow_unsigned,
                                     allow_execution=args.allow_execution)
        elif args.command == 'activate':
            result = manager.activate(args.release_id, allow_execution=args.allow_execution)
        elif args.command == 'rollback':
            result = manager.rollback(allow_execution=args.allow_execution)
        elif args.command == 'run':
            arguments = args.arguments[1:] if args.arguments[:1] == ['--'] else args.arguments
            return manager.run(args.entrypoint, arguments, allow_execution=args.allow_execution)
        elif args.command == 'recover':
            result = manager.recover()
        else:
            result = manager.status()
        print(json.dumps(result, sort_keys=True, indent=2))
        return 0
    except (ReleaseError, OSError, tarfile.TarError, EOFError) as error:
        print(f'release manager: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
