#!/usr/bin/env python3
"""Build an unsigned, Linux-only launcher preview; never a stable player release."""
from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime"))
import release_manager


def support_files() -> dict[str, bytes]:
    """Self-contained offline installation and explicit provisioning preparation tools."""
    names = {
        "release_manager.py": ROOT / "runtime/release_manager.py",
        "provision.py": ROOT / "runtime/provision.py",
        "docs/RELEASE_MANAGER.md": ROOT / "runtime/RELEASE_MANAGER.md",
        "docs/PROVISIONING.md": ROOT / "runtime/PROVISIONING.md",
        "docs/INTEGRATED_PLAYER.md": ROOT / "docs/INTEGRATED_PLAYER.md",
        "docs/keymap.md": ROOT / "core/keymap/README.md",
        "docs/media.md": ROOT / "core/media/README.md",
        "keymaps/default.json": ROOT / "core/keymap/examples/default.json",
    }
    files = {name: path.read_bytes() for name, path in names.items()}
    files["docs/INTEGRATED_PLAYER.md"] = (
        b"> Packaged source runbook: source commands and repository-relative links below\n"
        b"> refer to a separate RexPlayer source checkout. In this archive, see\n"
        b"> RELEASE_MANAGER.md, PROVISIONING.md, keymap.md, and ../keymaps/default.json.\n\n"
        + files["docs/INTEGRATED_PLAYER.md"])
    files["docs/keymap.md"] = (
        b"> Packaged profile: ../keymaps/default.json corresponds to examples/default.json\n"
        b"> in the source checkout. Copy it before editing and pass --keymap /path/to/copy.json.\n\n"
        + files["docs/keymap.md"])
    return files


def write_release_manifest(archive: Path, destination: Path, version: str, native_ui: bool) -> dict:
    """Sidecar covers the final archive, avoiding self-referential checksum metadata."""
    entries = {"launcher": "bin/rex-launcher"}
    if native_ui:
        entries["player"] = "bin/rex-player"
    manifest = release_manager.create_manifest(archive, version, entrypoints=entries)
    # Exclusive creation never removes a pre-existing sidecar on failure.
    stream = destination.open("xb")
    try:
        with stream:
            stream.write(release_manager.canonical(manifest))
            stream.flush()
            os.fsync(stream.fileno())
    except BaseException:
        destination.unlink(missing_ok=True)
        raise
    return manifest


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
    parser.add_argument('--version', default='0.1.0-local', help='SemVer identity for this unsigned local build')
    parser.add_argument('--manifest-output', type=Path, help='new sidecar path; defaults to <output>.manifest.json')
    parser.add_argument('--native-ui', action='store_true', help='also build/include the native GPUI release binary (requires native libraries)')
    args = parser.parse_args()
    if platform.system() != 'Linux':
        parser.error('preview packaging currently supports native Linux only')
    manifest_output = args.manifest_output or Path(str(args.output) + '.manifest.json')
    if args.output.absolute() == manifest_output.absolute():
        parser.error('archive and manifest need different output paths')
    if args.output.exists() or manifest_output.exists():
        parser.error('output or manifest already exists; choose new filenames')
    try:
        release_manager.validate_version(args.version)
    except release_manager.ReleaseError:
        parser.error('version must be a supported SemVer, at most 64 characters')
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
                'build_libc': list(platform.libc_ver()),
                'portable_distro_compatibility_verified': False,
                'includes_android_runtime': False, 'includes_native_gui': args.native_ui,
                'native_ui_profile': 'release' if args.native_ui else None,
                'native_ui_rendering_verified': False,
                'production_ready': False,
            }, sort_keys=True, indent=2) + '\n').encode(),
        }
        files.update(support_files())
        if args.native_ui:
            native_target = ROOT / 'core/ui/target'
            subprocess.run(['cargo', 'build', '--locked', '--release', '--features', 'native-gpui',
                            '--manifest-path', str(ROOT / 'core/ui/Cargo.toml'),
                            '--target-dir', str(native_target)], check=True, cwd=ROOT)
            native_binary = native_target / 'release/rex-player'
            subprocess.run([str(native_binary), '--help'], check=True, timeout=10)
            files['bin/rex-player'] = native_binary.read_bytes()
            files['docs/native-ui.md'] = (ROOT / 'core/ui/README.md').read_bytes()
            files['docs/VALIDATION.md'] = (ROOT / 'core/ui/VALIDATION.md').read_bytes()
        usage = '# Unsigned Linux preview\n\nRun `./bin/rex-launcher --help` for the command-line entry point.\n'
        if args.native_ui:
            usage += 'Run `./bin/rex-player` inside a supported graphical Linux desktop for the native shell.\n'
        usage += ('This archive includes an offline user-space host release manager, but no Android runtime.\n'
                  'The sidecar manifest and hashes are unsigned and do not authenticate the publisher.\n'
                  'See docs/RELEASE_MANAGER.md for explicit-trust install, launch, upgrade, and rollback.\n'
                  'See docs/PROVISIONING.md for preparation of separately supplied Android images.\n'
                  'It is not production-ready. Build commands below require a separate source checkout.\n\n---\n\n')
        files['README.md'] = usage.encode() + files['README.md']
        files['capability-report.schema.json'] = (ROOT / 'runtime/capability-report.schema.json').read_bytes()
        files['SHA256SUMS'] = ''.join(f'{hashlib.sha256(data).hexdigest()}  {name}\n'
                                    for name, data in sorted(files.items())).encode()
        # Exclusive creation prevents accidental replacement of an existing artifact.
        with args.output.open('xb'):
            pass
        try:
            write_archive(args.output, files)
            manifest = write_release_manifest(args.output, manifest_output, args.version, args.native_ui)
        except BaseException:
            args.output.unlink(missing_ok=True)
            raise
    print(f'UNSIGNED_PREVIEW={args.output}')
    print(f'UNSIGNED_MANIFEST={manifest_output}')
    print(f'RELEASE_ID={release_manager.release_id(manifest)}')
    print(f'SHA256={hashlib.sha256(args.output.read_bytes()).hexdigest()}')
    print('ANDROID_RUNTIME_VALIDATION=NOT_RUN; PRODUCTION_READY=NO')


if __name__ == '__main__':
    main()
