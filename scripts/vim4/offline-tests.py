#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run VIM4 host C regression fixtures, optionally followed by KUnit.

No modules are loaded and no device nodes are opened. A nonzero exit from any
fixture is retained even if later fixtures pass. Run from any working directory.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time


TEST_DIRS = (
    'drivers/gpu/drm/meson',
    'drivers/media/cec/platform/meson',
    'drivers/media/platform/amlogic/t7-hdmirx',
    'drivers/media/platform/amlogic/t7-jpeg',
    'drivers/media/platform/amlogic/vdec',
    'drivers/media/platform/chips-media/wave5',
    'drivers/net/wireless/broadcom/brcm80211/brcmfmac',
    'drivers/pmdomain/amlogic',
    'drivers/thermal',
    'drivers/usb/gadget/function',
    'drivers/usb/gadget/udc/crg',
    'sound/soc/meson',
    'tools/testing/panfrost',
)

VENDOR_TESTS = {
    'test_ao_cec_t7.py',
    'test_t7_layers.py', 'test_t7_link_clock.py', 'test_t7_modeset.py',
    'test_t7_osd.py', 'test_t7_sram.py', 'test_t7_video.py',
    'test_t7_video_phase.py', 'test_t7_video_pps.py',
}


def run_tests(source, output, timeout, vendor, audio_baseline):
    tracked = subprocess.check_output(
        ['git', '-C', str(source), 'ls-files', '--',
         *(directory + '/test_*.py' for directory in TEST_DIRS)], text=True)
    tests = sorted(path for path in tracked.splitlines() if (source / path).is_file())
    if not tests:
        raise RuntimeError('No VIM4 regression fixtures found')
    results = []
    route_test = 'sound/soc/meson/test_t7_audio_routes.py'
    variants = [(name, False) for name in tests]
    if route_test in tests:
        variants.append((route_test, True))
    for name, processing in variants:
        started = time.monotonic()
        label = name + (' --processing' if processing else '')
        log = output / (label.replace('/', '__') + '.log')
        command = [sys.executable, str(source / name)]
        if Path(name).name in VENDOR_TESTS:
            command.extend(['--vendor', str(vendor)])
        if name == route_test:
            command.extend(['--baseline', str(audio_baseline)])
            if processing:
                command.append('--processing')
        with log.open('w') as stream:
            try:
                result = subprocess.run(command,
                                        cwd=source, stdout=stream,
                                        stderr=subprocess.STDOUT, timeout=timeout)
                status = result.returncode
            except subprocess.TimeoutExpired:
                status = 124
                stream.write('\nFixture exceeded timeout\n')
        results.append({'test': label, 'exit': status,
                        'seconds': round(time.monotonic() - started, 3),
                        'log': log.name})
        print(f'{"PASS" if status == 0 else "FAIL"} {label}', flush=True)
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='New result directory outside source')
    parser.add_argument('--timeout', type=float, default=120, help='Seconds per host fixture')
    parser.add_argument('--vendor', type=Path, default=os.environ.get('VIM4_VENDOR'),
                        help='Local common_drivers checkout (or VIM4_VENDOR)')
    parser.add_argument('--audio-baseline', type=Path, default=os.environ.get('VIM4_AUDIO_BASELINE'),
                        help='Pre-audio-series audio-test DTB (or VIM4_AUDIO_BASELINE)')
    parser.add_argument('--kunit-config', type=Path, help='Optional KUnit config; requires QEMU for arm64')
    parser.add_argument('--kunit-arch', default='arm64')
    args = parser.parse_args()
    if args.vendor is None or not args.vendor.is_dir():
        parser.error('Supply an existing common_drivers checkout with --vendor or VIM4_VENDOR')
    vendor = args.vendor.resolve()
    source = Path(__file__).resolve().parents[2]
    baseline = args.audio_baseline
    if (source / 'sound/soc/meson/test_t7_audio_routes.py').is_file():
        if baseline is None or not baseline.is_file():
            parser.error('Supply a pre-audio-series DTB with --audio-baseline or VIM4_AUDIO_BASELINE')
        baseline = baseline.resolve()
    output = args.output.resolve()
    if output.is_relative_to(source):
        parser.error('Output must be outside the kernel source tree')
    output.mkdir(parents=True, exist_ok=False)
    results = run_tests(source, output, args.timeout, vendor, baseline)
    kunit = None
    if args.kunit_config:
        with (output / 'kunit.log').open('w') as stream:
            kunit = subprocess.run(
                [sys.executable, str(source / 'tools/testing/kunit/kunit.py'), 'run',
                 '--arch=' + args.kunit_arch,
                 '--kunitconfig=' + str(args.kunit_config.resolve()),
                 '--build_dir=' + str(output / 'kunit')],
                cwd=source, stdout=stream, stderr=subprocess.STDOUT).returncode
    report = {
        'revision': subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip(),
        'dirty': bool(subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain'], text=True).strip()),
        'tests': results, 'kunit_exit': kunit,
        'hardware_tested': False,
        'vendor': str(vendor),
        'vendor_revision': subprocess.check_output(
            ['git', '-C', str(vendor), 'rev-parse', 'HEAD'], text=True).strip(),
        'audio_baseline': str(baseline) if baseline else None,
        'audio_baseline_sha256': hashlib.sha256(baseline.read_bytes()).hexdigest()
        if baseline else None,
    }
    (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    return int(any(result['exit'] for result in results) or kunit not in (None, 0))


if __name__ == '__main__':
    raise SystemExit(main())
