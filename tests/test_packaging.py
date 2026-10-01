import importlib.util
from pathlib import Path
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

    def test_invalid_entry_name_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            for name in ('', '/absolute', '../outside', './dot', 'one//two'):
                with self.subTest(name=name), self.assertRaises(ValueError):
                    module.write_archive(Path(temporary) / 'preview.tar.gz', {name: b'fixture'})


if __name__ == '__main__':
    unittest.main()
