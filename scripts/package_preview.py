#!/usr/bin/env python3
"""Build an unsigned, Linux-only launcher preview; never a stable player release."""
from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path
import platform
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def write_archive(destination: Path, files: dict[str, bytes]) -> None:
    """Emit reproducible metadata and gzip headers, independent of local paths."""
    for name in files:
        if not name or name.startswith('/') or any(p in ('', '.', '..') for p in name.split('/')):
            raise ValueError(f'invalid archive entry: {name}')
    with destination.open('wb') as raw:
        with gzip.GzipFile(filename='', mode='wb', fileobj=raw, mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode='w', format=tarfile.PAX_FORMAT) as archive:
                for name, content in sorted(files.items()):
                    info = tarfile.TarInfo(name)
                    info.size = len(content)
                    info.mode = 0o755 if name in {'bin/rex-launcher', 'bin/rex-player'} else 0o644
                    info.mtime = 0
                    info.uid = info.gid = 0
                    archive.addfile(info, io.BytesIO(content))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='new .tar.gz archive; will not overwrite')
    parser.add_argument('--native-ui', action='store_true', help='also build/include the native GPUI release binary (requires native libraries)')
    args = parser.parse_args()
    if platform.system() != 'Linux':
        parser.error('preview packaging currently supports native Linux only')
    if args.output.exists():
        parser.error('output already exists; choose a new filename')
    # Isolated target avoids accidentally packaging a stale or cross-compiled artifact.
    with tempfile.TemporaryDirectory(prefix='rex-preview-build-') as directory:
        target = Path(directory)
        subprocess.run(['cargo', 'build', '--locked', '--release', '--manifest-path',
                        str(ROOT / 'core/launcher/Cargo.toml'), '--target-dir', str(target)],
                       check=True, cwd=ROOT)
        binary = target / 'release/rex-launcher'
        subprocess.run([str(binary), '--help'], check=True, timeout=10)
        files = {
            'bin/rex-launcher': binary.read_bytes(),
            'LICENSE': (ROOT / 'LICENSE').read_bytes(),
            'README.md': (ROOT / 'core/launcher/README.md').read_bytes(),
            'capability_inspector.py': (ROOT / 'runtime/capability_inspector.py').read_bytes(),
            'preview.json': (json.dumps({
                'schema_version': 1, 'channel': 'unsigned-preview',
                'os': platform.system(), 'architecture': platform.machine(),
                'includes_android_runtime': False, 'includes_native_gui': args.native_ui,
                'native_ui_profile': 'release' if args.native_ui else None,
                'native_ui_rendering_verified': False,
                'production_ready': False,
            }, sort_keys=True, indent=2) + '\n').encode(),
        }
        if args.native_ui:
            native_target = ROOT / 'core/ui/target'
            subprocess.run(['cargo', 'build', '--locked', '--release', '--features', 'native-gpui',
                            '--manifest-path', str(ROOT / 'core/ui/Cargo.toml'),
                            '--target-dir', str(native_target)], check=True, cwd=ROOT)
            native_binary = native_target / 'release/rex-player'
            subprocess.run([str(native_binary), '--help'], check=True, timeout=10)
            files['bin/rex-player'] = native_binary.read_bytes()
            files['docs/native-ui.md'] = (ROOT / 'core/ui/README.md').read_bytes()
        usage = '# Unsigned Linux preview\n\nRun `./bin/rex-launcher --help` for the command-line entry point.\n'
        if args.native_ui:
            usage += 'Run `./bin/rex-player` inside a supported graphical Linux desktop for the native shell.\n'
        usage += 'This archive includes no Android runtime or installer. It is not production-ready.\nBuild commands below require a separate source checkout.\n\n---\n\n'
        files['README.md'] = usage.encode() + files['README.md']
        files['capability-report.schema.json'] = (ROOT / 'runtime/capability-report.schema.json').read_bytes()
        files['SHA256SUMS'] = ''.join(f'{hashlib.sha256(data).hexdigest()}  {name}\n'
                                    for name, data in sorted(files.items())).encode()
        # Exclusive creation prevents accidental replacement of an existing artifact.
        with args.output.open('xb'):
            pass
        try:
            write_archive(args.output, files)
        except BaseException:
            args.output.unlink(missing_ok=True)
            raise
    print(f'UNSIGNED_PREVIEW={args.output}')
    print(f'SHA256={hashlib.sha256(args.output.read_bytes()).hexdigest()}')
    print('ANDROID_RUNTIME_VALIDATION=NOT_RUN; PRODUCTION_READY=NO')


if __name__ == '__main__':
    main()
