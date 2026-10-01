#!/usr/bin/env python3
"""Run non-privileged source gates. Never labels source checks runtime-ready."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def run(name: str, command: list[str], timeout: int = 600) -> dict:
    start = time.monotonic()
    if shutil.which(command[0]) is None:
        return {'name': name, 'status': 'BLOCKED', 'reason': f'missing tool: {command[0]}'}
    try:
        process = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                                 timeout=timeout, check=False)
        return {'name': name, 'status': 'PASS' if process.returncode == 0 else 'FAIL',
                'exit_code': process.returncode,
                'seconds': round(time.monotonic() - start, 3),
                'output': process.stdout + process.stderr}
    except subprocess.TimeoutExpired:
        return {'name': name, 'status': 'FAIL', 'reason': f'timeout after {timeout}s'}
    except OSError as error:
        return {'name': name, 'status': 'BLOCKED', 'reason': str(error)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--json', action='store_true', help='emit structured results on stdout')
    parser.add_argument('--native-ui', action='store_true', help='also compile/test the GPUI binary; needs native Linux build dependencies')
    args = parser.parse_args()
    checks: list[dict] = []
    def check(name, command, timeout=600):
        if args.json:
            print(f"CHECK_START: {name}", file=sys.stderr, flush=True)
        result = run(name, command, timeout)
        result['command'] = command
        checks.append(result)
        if args.json:
            print(f"CHECK_{result['status']}: {name}", file=sys.stderr, flush=True)
        if not args.json:
            print(f"{result['status']}: {name}", flush=True)
            if result['status'] != 'PASS':
                print(result.get('reason', result.get('output', '')).rstrip(), flush=True)

    check('historical evidence structure', [sys.executable, 'proof/validate_evidence.py'])
    check('evidence regression tests', [sys.executable, '-m', 'unittest', 'discover', '-s', 'tests', '-v'])
    check('runtime inspector, provisioning and release transaction tests', [sys.executable, '-m', 'unittest', 'discover', '-s', 'runtime/tests', '-v'])
    check('historical manifest hashes', ['sha256sum', '-c', 'proof/evidence/SHA256SUMS.txt'])
    for script in sorted((ROOT / 'proof').rglob('*.sh')):
        relative = str(script.relative_to(ROOT))
        shell = 'sh' if script.name == 'android_detection_matrix.sh' else 'bash'
        check(f'syntax {relative}', [shell, '-n', relative])
    with tempfile.TemporaryDirectory(prefix='rex-source-checks-') as temporary:
        for source in ('proof/input/rex_uinput_mt.c', 'proof/windows-wsl/create_binder_devices.c'):
            check(f'compile {source}', ['gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-std=c11',
                                       source, '-o', str(Path(temporary) / Path(source).stem)])
    for directory in ('proof/input-rust', 'core/input', 'core/launcher', 'core/input-linux', 'core/keymap', 'core/media', 'core/ui'):
        manifest = f'{directory}/Cargo.toml'
        features = ['--all-features'] if directory in ('core/keymap', 'core/media') else []
        check(f'format {directory}', ['cargo', 'fmt', '--manifest-path', manifest, '--check'])
        check(f'clippy {directory}', ['cargo', 'clippy', '--manifest-path', manifest, '--locked',
                                     '--all-targets', *features, '--', '-D', 'warnings'])
        check(f'test {directory}', ['cargo', 'test', '--manifest-path', manifest, '--locked', *features])
        check(f'release build {directory}', ['cargo', 'build', '--manifest-path', manifest, '--locked', '--release', *features])
    check('real H264 codec and process pipeline', [sys.executable, 'scripts/check_media_codec.py'], 360)
    if args.native_ui:
        manifest = 'core/ui/Cargo.toml'
        common = ['--manifest-path', manifest, '--locked', '--features', 'native-gpui']
        check('native GPUI clippy', ['cargo', 'clippy', *common, '--all-targets', '--', '-D', 'warnings'], 1200)
        check('native GPUI tests', ['cargo', 'test', *common], 1200)
        check('native GPUI optimized linked build', ['cargo', 'build', *common, '--release'], 1200)
    check('PowerShell manifest verification', ['pwsh', '-NoProfile', '-File', 'proof/windows-wsl/verify_evidence.ps1'])
    check('PowerShell verifier tests', ['pwsh', '-NoProfile', '-File', 'proof/windows-wsl/test_verify_evidence.ps1'])
    status = 'FAIL' if any(c['status'] == 'FAIL' for c in checks) else (
        'BLOCKED' if any(c['status'] == 'BLOCKED' for c in checks) else 'PASS')
    report = {'schema_version': 1, 'scope': 'non-privileged source checks',
              'status': status, 'runtime_validation': 'NOT_RUN', 'production_ready': False,
              'native_ui_build_requested': args.native_ui, 'native_ui_rendering': 'NOT_RUN',
              'checks': checks}
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print(f'AGGREGATE={status}; live Android/Windows/graphics/audio validation NOT_RUN')
    return {'PASS': 0, 'FAIL': 1, 'BLOCKED': 2}[status]


if __name__ == '__main__':
    sys.exit(main())
