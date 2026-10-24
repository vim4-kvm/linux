#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run VIM4 host C regression fixtures, optionally followed by KUnit.

No modules are loaded and no device nodes are opened. A nonzero exit from any
fixture is retained even if later fixtures pass. Run from any working directory.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time


TEST_DIRS = (
    'drivers/gpu/drm/meson',
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
    'test_t7_layers.py', 'test_t7_link_clock.py', 'test_t7_modeset.py',
    'test_t7_osd.py', 'test_t7_sram.py', 'test_t7_video.py',
    'test_t7_video_phase.py', 'test_t7_video_pps.py',
}


def run_tests(source, output, timeout, vendor):
    tracked = subprocess.check_output(
        ['git', '-C', str(source), 'ls-files', '--',
         *(directory + '/test_*.py' for directory in TEST_DIRS)], text=True)
    tests = sorted(path for path in tracked.splitlines() if (source / path).is_file())
    if not tests:
        raise RuntimeError('No VIM4 regression fixtures found')
    results = []
    for name in tests:
        started = time.monotonic()
        log = output / (name.replace('/', '__') + '.log')
        command = [sys.executable, str(source / name)]
        if Path(name).name in VENDOR_TESTS:
            command.extend(['--vendor', str(vendor)])
        with log.open('w') as stream:
            try:
                result = subprocess.run(command,
                                        cwd=source, stdout=stream,
                                        stderr=subprocess.STDOUT, timeout=timeout)
                status = result.returncode
            except subprocess.TimeoutExpired:
                status = 124
                stream.write('\nFixture exceeded timeout\n')
        results.append({'test': name, 'exit': status,
                        'seconds': round(time.monotonic() - started, 3),
                        'log': log.name})
        print(f'{"PASS" if status == 0 else "FAIL"} {name}', flush=True)
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='New result directory outside source')
    parser.add_argument('--timeout', type=float, default=120, help='Seconds per host fixture')
    parser.add_argument('--vendor', type=Path, default=os.environ.get('VIM4_VENDOR'),
                        help='Local common_drivers checkout (or VIM4_VENDOR)')
    parser.add_argument('--kunit-config', type=Path, help='Optional KUnit config; requires QEMU for arm64')
    parser.add_argument('--kunit-arch', default='arm64')
    args = parser.parse_args()
    if args.vendor is None or not args.vendor.is_dir():
        parser.error('Supply an existing common_drivers checkout with --vendor or VIM4_VENDOR')
    vendor = args.vendor.resolve()
    source = Path(__file__).resolve().parents[2]
    output = args.output.resolve()
    if output.is_relative_to(source):
        parser.error('Output must be outside the kernel source tree')
    output.mkdir(parents=True, exist_ok=False)
    results = run_tests(source, output, args.timeout, vendor)
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
    }
    (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    return int(any(result['exit'] for result in results) or kunit not in (None, 0))


if __name__ == '__main__':
    raise SystemExit(main())
