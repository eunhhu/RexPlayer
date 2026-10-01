#!/usr/bin/env python3
"""Exercise real H264 decoding and stream-process cleanup using synthetic pixels.

Requires installed trusted FFmpeg and Rust. No Android device, display server,
network connection or microphone is used. This is a codec/process check only.
"""
import os
import platform
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    if platform.system() != 'Linux':
        raise SystemExit('H264_CODEC_PROCESS_CHECK=UNSUPPORTED; Linux process backend required')
    with tempfile.TemporaryDirectory(prefix='rex-codec-') as directory:
        fixture = Path(directory) / 'testsrc.h264'
        subprocess.run(['ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'error',
                        '-f', 'lavfi', '-i', 'testsrc=size=64x48:rate=5', '-frames:v', '3',
                        '-pix_fmt', 'yuv420p', '-c:v', 'libx264', '-preset', 'ultrafast',
                        '-tune', 'zerolatency', '-f', 'h264', str(fixture)],
                       check=True, timeout=30)
        environment = dict(os.environ, REX_H264_FIXTURE=str(fixture))
        result = subprocess.run(['cargo', 'test', '--manifest-path', 'core/media/Cargo.toml',
                        '--locked', 'real_ffmpeg_', '--', '--ignored', '--nocapture'],
                       env=environment, cwd=ROOT, check=True, timeout=300,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        print(result.stdout, end='')
        for name in ('real_ffmpeg_decodes_three_h264_frames',
                     'real_ffmpeg_stalled_pipeline_cancels_and_times_out_promptly'):
            if f'test stream::tests::{name} ... ok' not in result.stdout:
                raise RuntimeError(f'required codec test did not pass: {name}')
    print('H264_CODEC_PROCESS_CHECK=PASS; ANDROID_STREAM_AND_AUDIO_SYNC=NOT_RUN')


if __name__ == '__main__':
    main()
