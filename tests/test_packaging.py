import importlib.util
from pathlib import Path
import json
import platform
from unittest.mock import patch
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('packaging_preview', ROOT / 'scripts/package_preview.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class PackagingTests(unittest.TestCase):
    def test_reproducible_archive_and_file_modes(self):
        with tempfile.TemporaryDirectory() as temporary:
            first, second = [Path(temporary) / name for name in ('one.tar.gz', 'two.tar.gz')]
            files = {'README.md': b'Preview only\n', 'bin/rex-launcher': b'fixture\n',
                     'bin/rex-player': b'native fixture\n'}
            module.write_archive(first, files)
            module.write_archive(second, dict(reversed(list(files.items()))))
            self.assertEqual(first.read_bytes(), second.read_bytes())
            with tarfile.open(first) as archive:
                self.assertEqual(archive.getnames(), sorted(files))
                self.assertEqual(archive.getmember('bin/rex-launcher').mode, 0o755)
                self.assertEqual(archive.getmember('bin/rex-player').mode, 0o755)
                self.assertEqual(archive.getmember('README.md').mode, 0o644)
                for name, expected in files.items():
                    self.assertEqual(archive.extractfile(name).read(), expected)
                    self.assertEqual(archive.getmember(name).mtime, 0)

    def test_distribution_includes_offline_tools_and_docs(self):
        files = module.support_files()
        self.assertEqual(set(files), {'release_manager.py', 'provision.py',
                                     'docs/RELEASE_MANAGER.md', 'docs/PROVISIONING.md',
                                     'docs/INTEGRATED_PLAYER.md', 'docs/keymap.md', 'docs/media.md', 'keymaps/default.json'})
        self.assertIn(b'unsigned', files['release_manager.py'])
        self.assertIn(b'--allow-execution', files['docs/RELEASE_MANAGER.md'])
        self.assertEqual(json.loads(files['keymaps/default.json'])['version'], 1)
        self.assertIn(b'separate RexPlayer source checkout', files['docs/INTEGRATED_PLAYER.md'])

    def test_release_sidecar_covers_final_archive_without_circular_hash(self):
        with tempfile.TemporaryDirectory() as temporary:
            archive, sidecar = [Path(temporary) / name for name in ('preview.tar.gz', 'release.json')]
            files = {'bin/rex-launcher': b'#!/bin/sh\nexit 0\n',
                     'bin/rex-player': b'#!/bin/sh\nexit 0\n', **module.support_files()}
            module.write_archive(archive, files)
            with patch.object(module.release_manager, 'host_platform',
                              return_value={'os': 'linux', 'architecture': 'x86_64'}):
                result = module.write_release_manifest(archive, sidecar, '1.2.3-local', True)
            self.assertEqual(result, json.loads(sidecar.read_text()))
            self.assertEqual(result['channel'], 'unsigned-local-preview')
            self.assertEqual(result['archive']['size'], archive.stat().st_size)
            self.assertEqual(result['healthcheck']['argv'], ['bin/rex-launcher', '--help'])
            self.assertEqual(result['entrypoints']['player'], 'bin/rex-player')
            self.assertEqual({item['path'] for item in result['files']}, set(files))
            with patch.object(module.release_manager, 'host_platform',
                              return_value={'os': 'linux', 'architecture': 'x86_64'}):
                with self.assertRaises(FileExistsError):
                    module.write_release_manifest(archive, sidecar, '1.2.3-local', True)

    def test_failed_sidecar_write_removes_only_its_partial_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            archive, sidecar = [Path(temporary) / name for name in ('preview.tar.gz', 'release.json')]
            module.write_archive(archive, {'bin/rex-launcher': b'#!/bin/sh\nexit 0\n'})
            with patch.object(module.release_manager, 'host_platform',
                              return_value={'os': 'linux', 'architecture': 'x86_64'}), \
                 patch.object(module.release_manager, 'canonical', side_effect=OSError('fixture disk failure')):
                with self.assertRaisesRegex(OSError, 'disk failure'):
                    module.write_release_manifest(archive, sidecar, '1.0.0', False)
            self.assertFalse(sidecar.exists())
            self.assertTrue(archive.exists())

    @unittest.skipUnless(platform.system() == 'Linux', 'host installation is Linux-only')
    def test_built_archive_installs_and_rolls_back_with_preserved_data(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            manager = module.release_manager.ReleaseManager(base / 'install')
            installed = []
            for version in ('1.0.0-local', '2.0.0-local'):
                archive = base / f'{version}.tar.gz'
                sidecar = base / f'{version}.json'
                files = {'bin/rex-launcher': b'#!/bin/sh\nexit 0\n', **module.support_files()}
                module.write_archive(archive, files)
                module.write_release_manifest(archive, sidecar, version, False)
                state = manager.install(sidecar, archive, allow_unsigned=True, allow_execution=True)
                installed.append(state['current'])
                (base / 'install/user-data/keep').write_text('preserve')
            state = manager.rollback(allow_execution=True)
            self.assertEqual(state['current'], installed[0])
            self.assertEqual(state['previous'], installed[1])
            self.assertEqual((base / 'install/user-data/keep').read_text(), 'preserve')

    def test_invalid_entry_name_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            for name in ('', '/absolute', '../outside', './dot', 'one//two'):
                with self.subTest(name=name), self.assertRaises(ValueError):
                    module.write_archive(Path(temporary) / 'preview.tar.gz', {name: b'fixture'})


if __name__ == '__main__':
    unittest.main()
