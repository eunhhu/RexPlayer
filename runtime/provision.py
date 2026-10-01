#!/usr/bin/env python3
"""Prepare verified local Waydroid images and an explicit first-install plan.

This tool never elevates privileges, downloads images, starts containers or changes
system/network settings. Execute the exported commands only after reviewing them
on the intended Linux host. Existing Waydroid installations are never reset.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import stat
import tempfile

MAX_IMAGE_SIZE = 16 * 1024**3
SOURCE = 'https://docs.waydro.id/usage/waydroid-command-line-options'


def inspect_host() -> dict:
    return {
        'os': platform.system(), 'architecture': platform.machine(),
        'waydroid_executable': shutil.which('waydroid'),
        'existing_waydroid': os.path.lexists('/var/lib/waydroid'),
        'wayland_session': bool(os.environ.get('WAYLAND_DISPLAY')),
        'wsl': 'microsoft' in platform.release().lower(),
        'privileged_changes_performed': False,
    }


def validate_manifest(value: object) -> dict:
    if not isinstance(value, dict) or set(value) != {'schema_version', 'architecture', 'images'}:
        raise ValueError('manifest requires only schema_version, architecture, images')
    if type(value['schema_version']) is not int or value['schema_version'] != 1:
        raise ValueError('unsupported image manifest schema')
    if value['architecture'] not in ('x86_64', 'aarch64'):
        raise ValueError('unsupported image architecture')
    images = value['images']
    if not isinstance(images, dict) or set(images) != {'system.img', 'vendor.img'}:
        raise ValueError('exactly system.img and vendor.img are required')
    for name, metadata in images.items():
        if not isinstance(metadata, dict) or set(metadata) != {'sha256', 'size'}:
            raise ValueError(f'invalid image metadata: {name}')
        if not isinstance(metadata['sha256'], str) or not re.fullmatch('[0-9a-f]{64}', metadata['sha256']):
            raise ValueError('invalid sha256')
        if type(metadata['size']) is not int or not 0 < metadata['size'] <= MAX_IMAGE_SIZE:
            raise ValueError('image size outside supported limits')
    return value


def unique_json(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f'duplicate manifest field: {key}')
        result[key] = value
    return result


def load_manifest(path: Path) -> dict:
    with path.open('rb') as stream:
        payload = stream.read(16385)
    if len(payload) > 16384:
        raise ValueError('manifest exceeds 16 KiB')
    return validate_manifest(json.loads(payload.decode('utf-8'), object_pairs_hook=unique_json))


def prepare(source: Path, manifest: dict, destination: Path, host: dict | None = None) -> dict:
    """Copy and hash exact files into a new directory; publish only on success."""
    manifest = validate_manifest(manifest)
    host = inspect_host() if host is None else host
    if host['os'] != 'Linux' or host['wsl']:
        raise ValueError('native Linux only; WSL kernel isolation requires a separate validated installer')
    if host['architecture'] != manifest['architecture']:
        raise ValueError('image and host architecture mismatch')
    if host['existing_waydroid']:
        raise ValueError('existing Waydroid configuration detected; first-install preparation refuses reset')
    destination = destination.absolute()
    if destination.exists() or destination.is_symlink():
        raise ValueError('destination already exists; no overwrite')
    if not destination.parent.is_dir():
        raise ValueError('destination parent must already exist')
    temporary = Path(tempfile.mkdtemp(prefix='.rex-images-', dir=destination.parent))
    try:
        for name, metadata in manifest['images'].items():
            path = source / name
            if path.is_symlink():
                raise ValueError(f'symlink image refused: {name}')
            flags = os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0) | getattr(os, 'O_NONBLOCK', 0)
            fd = os.open(path, flags)
            with os.fdopen(fd, 'rb') as incoming:
                info = os.fstat(incoming.fileno())
                if not stat.S_ISREG(info.st_mode) or info.st_size != metadata['size']:
                    raise ValueError(f'image is not a regular file of expected size: {name}')
                digest, count = hashlib.sha256(), 0
                with (temporary / name).open('xb') as outgoing:
                    while chunk := incoming.read(1024 * 1024):
                        count += len(chunk)
                        if count > metadata['size']:
                            raise ValueError(f'image grew during copy: {name}')
                        digest.update(chunk)
                        outgoing.write(chunk)
                    outgoing.flush()
                    os.fsync(outgoing.fileno())
                if count != metadata['size'] or digest.hexdigest() != metadata['sha256']:
                    raise ValueError(f'image digest/size mismatch: {name}')
        plan = {
            'schema_version': 1, 'status': 'PREPARED_NOT_INSTALLED',
            'trust': 'caller-supplied hashes; signature and provenance not established',
            'images': manifest, 'documentation': SOURCE,
            'requirements': ['Install trusted Waydroid distribution package separately',
                             'Review kernel Binder, graphics and audio support',
                             'Confirm no existing /var/lib/waydroid state before initialization',
                             'Administrative initialization affects system Waydroid state',
                             'Container start creates network/mount state and requires separate approval'],
            'administrative_commands_for_review': [
                ['waydroid', 'init', '-i', str(destination), '-s', 'VANILLA'],
                ['waydroid', 'container', 'start']],
            'unprivileged_command_after_admin_setup': ['waydroid', 'session', 'start'],
            'runtime_verified': False,
        }
        (temporary / 'provision-plan.json').write_text(json.dumps(plan, indent=2) + '\n', encoding='utf-8')
        # Reserve the final directory exclusively. Never replace another writer.
        destination.mkdir(mode=0o700)
        try:
            for name in ('system.img', 'vendor.img', 'provision-plan.json'):
                os.rename(temporary / name, destination / name)
            (destination / 'READY').write_text('Prepared only; not installed or boot-verified\n', encoding='utf-8')
        except BaseException:
            # Only this transaction owns destination; incomplete directories have no READY.
            raise
        return plan
    finally:
        shutil.rmtree(temporary)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('inspect')
    p = sub.add_parser('prepare-images')
    p.add_argument('--source', type=Path, required=True)
    p.add_argument('--manifest', type=Path, required=True)
    p.add_argument('--destination', type=Path, required=True)
    args = parser.parse_args()
    try:
        result = inspect_host() if args.command == 'inspect' else prepare(args.source, load_manifest(args.manifest), args.destination)
        print(json.dumps(result, indent=2))
        return 0
    except (OSError, ValueError) as error:
        print(json.dumps({'status': 'FAILED', 'error': str(error), 'runtime_verified': False}))
        return 1

if __name__ == '__main__':
    raise SystemExit(main())
