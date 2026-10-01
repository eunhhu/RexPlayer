import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('provision', Path(__file__).resolve().parents[1] / 'provision.py')
p = importlib.util.module_from_spec(spec)
spec.loader.exec_module(p)


class ProvisionTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.source = self.root / 'source'
        self.source.mkdir()
        self.dest = self.root / 'prepared'
        self.host = {'os': 'Linux', 'architecture': 'x86_64', 'wsl': False, 'existing_waydroid': False}
        self.manifest = {'schema_version': 1, 'architecture': 'x86_64', 'images': {}}
        for name in ('system.img', 'vendor.img'):
            data = (name * 1000).encode()
            (self.source / name).write_bytes(data)
            self.manifest['images'][name] = {'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}

    def test_copies_verified_images_and_review_plan(self):
        result = p.prepare(self.source, self.manifest, self.dest, self.host)
        self.assertEqual(result['status'], 'PREPARED_NOT_INSTALLED')
        self.assertFalse(result['runtime_verified'])
        for name in self.manifest['images']:
            self.assertEqual((self.source / name).read_bytes(), (self.dest / name).read_bytes())
        self.assertTrue((self.dest / 'READY').exists())
        self.assertEqual(json.loads((self.dest / 'provision-plan.json').read_text()), result)
        self.assertNotIn('-f', result['administrative_commands_for_review'][0])

    def test_digest_failure_does_not_publish(self):
        self.manifest['images']['vendor.img']['sha256'] = '0' * 64
        with self.assertRaises(ValueError):
            p.prepare(self.source, self.manifest, self.dest, self.host)
        self.assertFalse(self.dest.exists())
        self.assertEqual(list(self.root.glob('.rex-images-*')), [])

    def test_existing_destination_untouched(self):
        self.dest.mkdir()
        marker = self.dest / 'user-data'
        marker.write_text('preserve')
        with self.assertRaises(ValueError):
            p.prepare(self.source, self.manifest, self.dest, self.host)
        self.assertEqual(marker.read_text(), 'preserve')

    def test_existing_install_wsl_and_wrong_host_refused(self):
        for patch in ({'existing_waydroid': True}, {'wsl': True}, {'os': 'Windows'}, {'architecture': 'aarch64'}):
            with self.subTest(patch=patch), self.assertRaises(ValueError):
                p.prepare(self.source, self.manifest, self.dest, dict(self.host, **patch))
        self.assertFalse(self.dest.exists())

    def test_symlink_image_rejected(self):
        path = self.source / 'system.img'
        original = self.root / 'original'
        path.rename(original)
        try:
            path.symlink_to(original)
        except OSError:
            self.skipTest('symlinks unavailable')
        with self.assertRaises((OSError, ValueError)):
            p.prepare(self.source, self.manifest, self.dest, self.host)

    def test_size_and_missing_file_refused(self):
        (self.source / 'system.img').write_bytes(b'short')
        with self.assertRaises(ValueError):
            p.prepare(self.source, self.manifest, self.dest, self.host)
        (self.source / 'system.img').unlink()
        with self.assertRaises(OSError):
            p.prepare(self.source, self.manifest, self.dest, self.host)

    def test_manifest_duplicates_unknown_and_bool_rejected(self):
        with self.assertRaises(ValueError):
            p.unique_json([('a', 1), ('a', 2)])
        for change in ({'extra': True}, {'schema_version': True}, {'architecture': 'arm'}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                p.validate_manifest(dict(self.manifest, **change))
        self.manifest['images']['system.img']['size'] = True
        with self.assertRaises(ValueError):
            p.validate_manifest(self.manifest)

    def test_repeat_refuses_without_mutation(self):
        p.prepare(self.source, self.manifest, self.dest, self.host)
        before = {x.name: x.read_bytes() for x in self.dest.iterdir()}
        with self.assertRaises(ValueError):
            p.prepare(self.source, self.manifest, self.dest, self.host)
        self.assertEqual(before, {x.name: x.read_bytes() for x in self.dest.iterdir()})

if __name__ == '__main__':
    unittest.main()
