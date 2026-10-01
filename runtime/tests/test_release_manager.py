"""Real temporary-filesystem release transactions; native Linux execution only."""
from __future__ import annotations

import hashlib
import io
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import sys
import tarfile
import tempfile
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import release_manager as release

SCRIPT = '#!/bin/sh\n[ "$1" = "--health" ] && exit 0\nprintf "fixture:%s\\n" "$*"\n'


def archive_file(path, script=SCRIPT, extra=None):
    files = {'bin/rex-launcher': script.encode(), 'README.txt': b'offline fixture\n'}
    files.update(extra or {})
    with tarfile.open(path, 'w:gz') as archive:
        for name, content in sorted(files.items()):
            member = tarfile.TarInfo(name)
            member.mode = 0o755 if name == 'bin/rex-launcher' else 0o644
            member.size = len(content)
            archive.addfile(member, io.BytesIO(content))
    return files


def platform_manifest():
    return {'schema_version': 1, 'channel': release.CHANNEL, 'version': '1.0.0',
        'platform': {'os': 'linux', 'architecture': 'x86_64'},
        'archive': {'sha256': 'a' * 64, 'size': 1, 'unpacked_size': 1},
        'files': [{'path': 'bin/test', 'sha256': 'b' * 64, 'size': 1, 'executable': True}],
        'healthcheck': {'argv': ['bin/test'], 'timeout_seconds': 1},
        'entrypoints': {'launcher': 'bin/test'}, 'user_data_schema': 1}


class ManifestTests(unittest.TestCase):
    def test_valid_manifest_and_platform_refusal(self):
        value = platform_manifest()
        self.assertEqual(release.validate_manifest(value, check_host=False), value)
        with patch.object(release, 'host_platform', return_value={'os': 'windows', 'architecture': 'x86_64'}):
            with self.assertRaisesRegex(release.ReleaseError, 'platform'):
                release.validate_manifest(value)
            with tempfile.TemporaryDirectory() as temp:
                root = Path(temp) / 'not-created'
                with self.assertRaisesRegex(release.ReleaseError, 'Linux only'):
                    release.ReleaseManager(root).status()
                self.assertFalse(root.exists())

    def test_unsupported_manifest_contracts_fail_closed(self):
        for field, invalid in [('schema_version', 2), ('schema_version', True),
                               ('channel', 'stable'), ('version', '../escape'),
                               ('user_data_schema', 2), ('files', []), ('entrypoints', {})]:
            with self.subTest(field=field, invalid=invalid):
                value = platform_manifest()
                value[field] = invalid
                with self.assertRaises(release.ReleaseError):
                    release.validate_manifest(value, check_host=False)
        value = platform_manifest()
        value['signature'] = 'not-a-real-signature'
        with self.assertRaises(release.ReleaseError):
            release.validate_manifest(value, check_host=False)

    def test_strict_semantic_versions(self):
        for version in ('01.0.0', '1.02.0', '1.0.0-01', '1.0.0-alpha..1',
                        '1.0.0+metadata..1', '1.0.0-', '1.0.0+'):
            with self.subTest(version=version), self.assertRaises(release.ReleaseError):
                release.validate_version(version)
        for version in ('0.1.0', '1.0.0-alpha.1', '1.0.0-0', '1.0.0+build.01', '2.0.0-preview.1'):
            self.assertEqual(release.validate_version(version), version)

    def test_bad_paths_rejected(self):
        for name in ('', '/absolute', '../escape', 'a/../b', './bin', 'a//b',
                     'a\\b', 'C:drive', 'a\0b', 'a\nb', 'dir./file', 'dir /file'):
            with self.subTest(name=name), self.assertRaises(release.ReleaseError):
                release.safe_name(name)

    def test_duplicate_paths_case_collisions_and_parent_files(self):
        for name in ('bin/test', 'BIN/TEST', 'bin/test/child'):
            value = platform_manifest()
            value['files'].append(dict(value['files'][0], path=name))
            value['archive']['unpacked_size'] = 2
            with self.subTest(name=name), self.assertRaises(release.ReleaseError):
                release.validate_manifest(value, check_host=False)

    def test_size_and_execution_contracts(self):
        mutations = [lambda m: m['archive'].update(size=True),
                     lambda m: m['archive'].update(unpacked_size=2),
                     lambda m: m['files'][0].update(sha256='bad'),
                     lambda m: m['files'][0].update(executable=1),
                     lambda m: m['healthcheck'].update(argv=['/bin/sh']),
                     lambda m: m['healthcheck'].update(argv=['bin/test', '\0']),
                     lambda m: m['healthcheck'].update(timeout_seconds=121),
                     lambda m: m['entrypoints'].update(launcher='unknown')]
        for mutation in mutations:
            value = platform_manifest()
            mutation(value)
            with self.assertRaises(release.ReleaseError):
                release.validate_manifest(value, check_host=False)

    def test_json_duplicate_keys_are_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'manifest.json'
            path.write_text('{"schema_version":1,"schema_version":2}')
            with self.assertRaisesRegex(release.ReleaseError, 'duplicate'):
                release.read_json(path)


@unittest.skipUnless(platform.system() == 'Linux', 'real POSIX transactions require native Linux')
class ReleaseTransactionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.root = self.base / 'installed'
        self.manager = release.ReleaseManager(self.root)
        self.serial = 0

    def bundle(self, version='1.0.0', script=SCRIPT, extra=None, timeout=2):
        self.serial += 1
        archive = self.base / f'archive-{self.serial}.tar.gz'
        archive_file(archive, script, extra)
        manifest = release.create_manifest(archive, version,
            health_argv=['bin/rex-launcher', '--health'], timeout=timeout)
        path = self.base / f'manifest-{self.serial}.json'
        path.write_bytes(release.canonical(manifest))
        return path, archive, manifest

    def install(self, bundle):
        return self.manager.install(*bundle[:2], allow_unsigned=True, allow_execution=True)

    def test_install_launch_upgrade_rollback_preserves_user_data(self):
        first = self.bundle()
        state = self.install(first)
        self.assertEqual(state['current'], release.release_id(first[2]))
        data = self.root / 'user-data/savegame.txt'
        data.write_text('important saved game')
        second = self.bundle('2.0.0')
        state2 = self.install(second)
        self.assertEqual(state2['previous'], state['current'])
        self.assertEqual(state2['generation'], 2)
        rolled = self.manager.rollback(allow_execution=True)
        self.assertEqual(rolled['current'], state['current'])
        self.assertEqual(rolled['previous'], state2['current'])
        self.assertEqual(data.read_text(), 'important saved game')
        self.assertEqual(len(self.manager.status()['releases']), 2)
        self.assertFalse(self.manager.status()['signature_verified'])
        result = subprocess.run([sys.executable, str(Path(release.__file__)), '--root', str(self.root),
                                 'run', '--allow-execution', 'launcher', '--', 'hello', 'world'],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, 'fixture:hello world\n')

    def test_stage_does_not_execute_or_activate(self):
        marker = self.base / 'executed'
        bundle = self.bundle(script=f'#!/bin/sh\ntouch "{marker}"\n')
        identifier = self.manager.stage(*bundle[:2], allow_unsigned=True)
        self.assertFalse(marker.exists())
        self.assertIsNone(self.manager.status()['state']['current'])
        self.assertTrue((self.root / 'versions' / identifier / 'payload/bin/rex-launcher').exists())

    def test_explicit_unsigned_and_execution_opt_ins(self):
        bundle = self.bundle()
        with self.assertRaisesRegex(release.ReleaseError, 'allow-unsigned'):
            self.manager.stage(*bundle[:2])
        with self.assertRaisesRegex(release.ReleaseError, 'allow-execution'):
            self.manager.install(*bundle[:2], allow_unsigned=True)
        self.assertFalse(self.root.exists())
        identifier = self.manager.stage(*bundle[:2], allow_unsigned=True)
        with self.assertRaisesRegex(release.ReleaseError, 'allow-execution'):
            self.manager.activate(identifier)
        with self.assertRaisesRegex(release.ReleaseError, 'allow-execution'):
            self.manager.run('launcher', [])
        self.assertIsNone(self.manager.status()['state']['current'])

    def test_repeated_install_is_idempotent(self):
        counter = self.base / 'counter'
        bundle = self.bundle(script=f'#!/bin/sh\necho health >> "{counter}"\n')
        first = self.install(bundle)
        second = self.install(bundle)
        self.assertEqual(first, second)
        self.assertEqual(counter.read_text(), 'health\n')
        self.assertEqual(len(self.manager.status()['releases']), 1)
        self.assertEqual(list((self.root / 'staging').iterdir()), [])

    def test_failed_health_keeps_current_and_retains_candidate(self):
        before = self.install(self.bundle())
        bad = self.bundle('2.0.0', '#!/bin/sh\nexit 7\n')
        with self.assertRaisesRegex(release.ReleaseError, 'exit 7'):
            self.install(bad)
        self.assertEqual(self.manager.status()['state'], before)
        self.assertEqual(len(self.manager.status()['releases']), 2)
        self.assertFalse(self.manager.status()['recovery_pending'])

    def test_health_timeout_terminates_children_and_keeps_current(self):
        before = self.install(self.bundle())
        child_pid = self.base / 'child.pid'
        script = f'#!/bin/sh\nsleep 20 &\necho $! > "{child_pid}"\nwait\n'
        bad = self.bundle('2.0.0', script, timeout=1)
        started = time.monotonic()
        with self.assertRaisesRegex(release.ReleaseError, 'timed out'):
            self.install(bad)
        self.assertLess(time.monotonic() - started, 5)
        self.assertEqual(self.manager.status()['state'], before)
        pid = int(child_pid.read_text())
        proc = Path(f'/proc/{pid}/stat')
        # A killed child can briefly remain as a zombie pending init's reaping.
        if proc.exists():
            self.assertEqual(proc.read_text().split()[2], 'Z')

    def test_health_mutation_is_rejected_before_activation(self):
        self.install(self.bundle())
        before = self.manager.status()['state']
        bad = self.bundle('2.0.0', '#!/bin/sh\necho changed > README.txt\nexit 0\n')
        with self.assertRaisesRegex(release.ReleaseError, 'integrity|declared size'):
            self.install(bad)
        self.assertEqual(release.read_json(self.root / 'state.json'), before)

    def test_archive_size_hash_and_file_hash_fail_before_activation(self):
        for mutation in ('archive-size', 'archive-hash', 'file-hash'):
            with self.subTest(mutation=mutation):
                bundle = self.bundle()
                manifest = bundle[2]
                if mutation == 'archive-size':
                    manifest['archive']['size'] += 1
                elif mutation == 'archive-hash':
                    manifest['archive']['sha256'] = '0' * 64
                else:
                    manifest['files'][0]['sha256'] = '0' * 64
                bundle[0].write_bytes(release.canonical(manifest))
                with self.assertRaises(release.ReleaseError):
                    self.manager.stage(*bundle[:2], allow_unsigned=True)
                self.assertIsNone(self.manager.status()['state']['current'])
                self.assertEqual(list((self.root / 'staging').iterdir()), [])

    def test_truncated_archive_cleanup_and_retry(self):
        bundle = self.bundle()
        original = bundle[1].read_bytes()
        bundle[1].write_bytes(original[:30])
        with self.assertRaises(release.ReleaseError):
            self.manager.stage(*bundle[:2], allow_unsigned=True)
        bundle[1].write_bytes(original)
        self.install(bundle)
        self.assertIsNotNone(self.manager.status()['state']['current'])

    def test_symlink_and_unrecognized_roots_are_refused(self):
        actual = self.base / 'actual'
        actual.mkdir()
        linked = self.base / 'linked'
        linked.symlink_to(actual, target_is_directory=True)
        with self.assertRaisesRegex(release.ReleaseError, 'symlink'):
            release.ReleaseManager(linked / 'nested').status()
        (actual / 'unrelated').write_text('keep')
        with self.assertRaisesRegex(release.ReleaseError, 'unrecognized'):
            release.ReleaseManager(actual).status()
        self.assertEqual((actual / 'unrelated').read_text(), 'keep')

    def test_managed_symlinks_and_tampering_are_refused(self):
        identifier = self.install(self.bundle())['current']
        payload = self.root / 'versions' / identifier / 'payload'
        target = payload / 'README.txt'
        target.unlink()
        target.symlink_to(self.base / 'external')
        with self.assertRaisesRegex(release.ReleaseError, 'regular file'):
            self.manager.run('launcher', [], allow_execution=True)
        target.unlink()
        target.write_text('tamper')
        with self.assertRaises(release.ReleaseError):
            self.manager.status()

    def test_missing_state_never_resets_existing_installation(self):
        state = self.install(self.bundle())
        (self.root / 'state.json').unlink()
        with self.assertRaisesRegex(release.ReleaseError, 'state is missing'):
            self.manager.recover()
        self.assertFalse((self.root / 'state.json').exists())
        self.assertTrue((self.root / 'versions' / state['current']).exists())

    def test_health_cleanup_is_bounded_and_refuses_activation(self):
        bundle = self.bundle()
        identifier = self.manager.stage(*bundle[:2], allow_unsigned=True)
        from types import SimpleNamespace
        process = SimpleNamespace(pid=12345678)
        def stalled_reap(*, timeout):
            self.assertEqual(timeout, 2)
            raise subprocess.TimeoutExpired('fixture', timeout)
        process.wait = stalled_reap
        exited = SimpleNamespace(si_status=0, si_code=os.CLD_EXITED)
        with patch.object(release.subprocess, 'Popen', return_value=process), \
             patch.object(release.os, 'waitid', return_value=exited), \
             patch.object(release.os, 'killpg'):
            with self.assertRaisesRegex(release.ReleaseError, 'cleanup did not finish'):
                self.manager.activate(identifier, allow_execution=True)
        self.assertIsNone(self.manager.status()['state']['current'])

    def test_successful_health_cleans_background_children_before_reap(self):
        child_pid = self.base / 'child.pid'
        script = f'#!/bin/sh\nsleep 20 &\necho $! > "{child_pid}"\nexit 0\n'
        self.install(self.bundle(script=script))
        proc = Path(f'/proc/{int(child_pid.read_text())}/stat')
        if proc.exists():
            self.assertEqual(proc.read_text().split()[2], 'Z')

    def test_extended_tar_metadata_rejected_before_processing(self):
        bundle = self.bundle()
        with tarfile.open(bundle[1], 'w:gz', format=tarfile.PAX_FORMAT) as archive:
            member = tarfile.TarInfo('bin/rex-launcher')
            member.size, member.mode = len(SCRIPT.encode()), 0o755
            member.pax_headers = {'comment': 'unsupported metadata'}
            archive.addfile(member, io.BytesIO(SCRIPT.encode()))
        raw = bundle[1].read_bytes()
        bundle[2]['archive'].update(size=len(raw), sha256=hashlib.sha256(raw).hexdigest())
        bundle[0].write_bytes(release.canonical(bundle[2]))
        with self.assertRaisesRegex(release.ReleaseError, 'plain regular-file'):
            self.manager.stage(*bundle[:2], allow_unsigned=True)
        self.assertEqual(list((self.root / 'staging').iterdir()), [])

    def test_malformed_state_fails_closed(self):
        self.install(self.bundle())
        state = release.read_json(self.root / 'state.json')
        state['current'] = '../elsewhere'
        (self.root / 'state.json').write_bytes(release.canonical(state))
        with self.assertRaisesRegex(release.ReleaseError, 'pointer'):
            self.manager.status()

    def test_archive_member_types_paths_modes_and_duplicates_rejected(self):
        for variant in ('link', 'hardlink', 'fifo', 'directory', 'traversal', 'setuid', 'duplicate', 'extra', 'missing'):
            with self.subTest(variant=variant):
                bundle = self.bundle()
                content = SCRIPT.encode()
                with tarfile.open(bundle[1], 'w:gz') as archive:
                    first = tarfile.TarInfo('bin/rex-launcher')
                    first.size, first.mode = len(content), 0o755
                    archive.addfile(first, io.BytesIO(content))
                    member = tarfile.TarInfo('README.txt')
                    member.size, member.mode = 16, 0o644
                    if variant in ('link', 'hardlink', 'fifo', 'directory'):
                        member.type = {'link': tarfile.SYMTYPE, 'hardlink': tarfile.LNKTYPE,
                                       'fifo': tarfile.FIFOTYPE, 'directory': tarfile.DIRTYPE}[variant]
                        member.linkname = '../../outside' if variant in ('link', 'hardlink') else ''
                        member.size = 0
                    if variant == 'traversal':
                        member.name = '../escape'
                    if variant == 'setuid':
                        member.mode = 0o4644
                    if variant == 'duplicate':
                        member.name, member.size, member.mode = first.name, len(content), 0o755
                    if variant == 'extra':
                        member.name = 'extra.txt'
                    if variant != 'missing':
                        archive.addfile(member, io.BytesIO(content if variant == 'duplicate' else b'offline fixture\n'))
                raw = bundle[1].read_bytes()
                bundle[2]['archive'].update(size=len(raw), sha256=hashlib.sha256(raw).hexdigest())
                bundle[0].write_bytes(release.canonical(bundle[2]))
                with self.assertRaises(release.ReleaseError):
                    self.manager.stage(*bundle[:2], allow_unsigned=True)
                self.assertFalse((self.base / 'escape').exists())
                self.assertEqual(list((self.root / 'staging').iterdir()), [])

    def test_competing_process_cannot_take_live_lock(self):
        with self.manager._locked():
            result = subprocess.run([sys.executable, str(Path(release.__file__)), '--root', str(self.root), 'status'],
                                    capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 1)
        self.assertIn('holds the install lock', result.stderr)
        self.assertEqual(self.manager.status()['state'], release.EMPTY_STATE)

    def crash_process(self, code, *arguments):
        environment = dict(os.environ, PYTHONPATH=str(Path(release.__file__).parent))
        process = subprocess.run([sys.executable, '-c', code, str(self.root), *map(str, arguments)],
                                 env=environment, capture_output=True, text=True, timeout=10)
        self.assertEqual(process.returncode, 93, process.stderr)

    def test_process_death_during_extraction_recovers_and_retries(self):
        bundle = self.bundle()
        self.crash_process('''
import os, sys
from pathlib import Path
import release_manager as r
original = r.ReleaseManager._verify
def crash(self, path, identifier):
    os._exit(93)
r.ReleaseManager._verify = crash
r.ReleaseManager(Path(sys.argv[1])).stage(Path(sys.argv[2]), Path(sys.argv[3]), allow_unsigned=True)
''', *bundle[:2])
        self.assertTrue(self.manager.status()['recovery_pending'])
        result = self.manager.recover()
        self.assertEqual(result['recovery'], 'incomplete_stage_removed')
        self.assertEqual(list((self.root / 'staging').iterdir()), [])
        self.assertEqual(self.manager.recover()['recovery'], 'nothing_to_recover')
        self.install(bundle)

    def test_process_death_after_version_rename_retains_staged_release(self):
        bundle = self.bundle()
        self.crash_process('''
import os, sys
from pathlib import Path
import release_manager as r
def crash(self):
    os._exit(93)
r.ReleaseManager._clear_journal = crash
r.ReleaseManager(Path(sys.argv[1])).stage(Path(sys.argv[2]), Path(sys.argv[3]), allow_unsigned=True)
''', *bundle[:2])
        self.assertEqual(self.manager.recover()['recovery'], 'staged_release_retained')
        self.assertEqual(len(self.manager.status()['releases']), 1)
        self.install(bundle)

    def test_process_death_before_and_after_atomic_switch(self):
        first = self.install(self.bundle())
        second = self.bundle('2.0.0')
        identifier = self.manager.stage(*second[:2], allow_unsigned=True)
        for after in (False, True):
            with self.subTest(after_commit=after):
                self.crash_process(f'''
import os, sys
from pathlib import Path
import release_manager as r
original = r._write_json
def crash(path, value):
    if path.name == 'state.json':
        if {after!r}:
            original(path, value)
        os._exit(93)
    return original(path, value)
r._write_json = crash
r.ReleaseManager(Path(sys.argv[1])).activate(sys.argv[2], allow_execution=True)
''', identifier)
                result = self.manager.recover()
                self.assertEqual(result['recovery'], 'committed_activation_retained' if after else 'uncommitted_activation_aborted')
                self.assertEqual(result['state']['current'], identifier if after else first['current'])
                self.assertEqual(result['state']['generation'], 2 if after else 1)

    def test_process_death_during_health_never_activates(self):
        before = self.install(self.bundle())
        second = self.bundle('2.0.0')
        identifier = self.manager.stage(*second[:2], allow_unsigned=True)
        self.crash_process('''
import os, sys
from pathlib import Path
import release_manager as r
def crash(*args): os._exit(93)
r.ReleaseManager._health = crash
r.ReleaseManager(Path(sys.argv[1])).activate(sys.argv[2], allow_execution=True)
''', identifier)
        self.assertEqual(self.manager.recover()['state'], before)
        self.manager.activate(identifier, allow_execution=True)

    def test_rollback_rechecks_health_and_integrity(self):
        first = self.bundle()
        state = self.install(first)
        second = self.install(self.bundle('2.0.0'))
        executable = self.root / 'versions' / state['current'] / 'payload/bin/rex-launcher'
        executable.write_text('#!/bin/sh\nexit 0\n')
        with self.assertRaises(release.ReleaseError):
            self.manager.rollback(allow_execution=True)
        self.assertEqual(release.read_json(self.root / 'state.json'), second)

    def test_no_previous_rollback_and_unknown_entrypoint(self):
        self.install(self.bundle())
        with self.assertRaisesRegex(release.ReleaseError, 'previous'):
            self.manager.rollback(allow_execution=True)
        with self.assertRaisesRegex(release.ReleaseError, 'entrypoint'):
            self.manager.run('not-installed', [], allow_execution=True)

    def test_cli_manifest_and_install(self):
        archive = self.base / 'cli.tar.gz'
        archive_file(archive)
        manifest = self.base / 'sidecar.json'
        command = [sys.executable, str(Path(release.__file__)), '--root', str(self.root)]
        result = subprocess.run([*command, 'manifest', '--archive', str(archive), '--output', str(manifest), '--version', '3.1.2'],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        repeated = subprocess.run([*command, 'manifest', '--archive', str(archive), '--output', str(manifest), '--version', '3.1.2'],
                                  capture_output=True, text=True, timeout=10)
        self.assertEqual(repeated.returncode, 1)
        result = subprocess.run([*command, 'install', '--manifest', str(manifest), '--archive', str(archive),
                                 '--allow-unsigned', '--allow-execution'], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['generation'], 1)


if __name__ == '__main__':
    unittest.main()
