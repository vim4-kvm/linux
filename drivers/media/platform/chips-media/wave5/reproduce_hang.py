#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Plan, or explicitly execute, ONE exclusive Wave5 encoding/teardown trial.

Run on the board as root only inside a Mac board+heavy lock window, with video
applications already stopped and a prepared recovery procedure. This program
does not install, stop services, reboot, or restore a kernel. Default is plan
only. --execute can wedge an affected kernel; SIGKILL cannot release a D task.
Keep an independent UART log. A platform/AXI lockup can also stop this observer.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


def wait_until_exit(process, seconds):
    deadline = time.monotonic() + seconds
    while process.poll() is None and time.monotonic() < deadline:
        time.sleep(0.1)
    return process.poll()


def signal_group(process, sig):
    try:
        os.killpg(process.pid, sig)
    except ProcessLookupError:
        pass


def snapshot(pid, directory):
    directory.mkdir(parents=True, exist_ok=True)
    paths = [Path('/proc/interrupts'), Path('/proc/meminfo'), Path('/proc/uptime')]
    for task in Path(f'/proc/{pid}/task').glob('*'):
        paths.extend(task / name for name in ('stack', 'wchan', 'status', 'syscall'))
    for path in paths:
        output = directory / str(path).lstrip('/').replace('/', '-')
        try:
            output.write_bytes(path.read_bytes())
        except OSError as error:
            output.write_text(str(error))
    # The supervisor bounds this worker, including dmesg and proc reads.
    with (directory / 'dmesg.txt').open('wb') as log:
        subprocess.run(['dmesg', '--color=never'], stdout=log, stderr=log, check=False)


def collect(process, output, phase, report):
    with (output / 'observer.log').open('ab') as log:
        worker = subprocess.Popen([sys.executable, str(Path(__file__).resolve()),
                                   '--snapshot', str(process.pid), str(output / phase)],
                                  stdout=log, stderr=log, start_new_session=True,
                                  stdin=subprocess.DEVNULL)
    status = wait_until_exit(worker, 3)
    if status is None:
        signal_group(worker, signal.SIGKILL)
        status = wait_until_exit(worker, 1)
    report.setdefault('snapshots', []).append(dict(phase=phase, status=status,
                                                  worker_pid=worker.pid))
    return status is not None


def video_owners(device):
    owners = []
    for process in Path('/proc').glob('[0-9]*'):
        for fd in (process / 'fd').glob('*'):
            try:
                if fd.resolve() == device:
                    owners.append(str(fd))
            except OSError:
                pass
    return owners


def main():
    if len(sys.argv) == 4 and sys.argv[1] == '--snapshot':
        snapshot(int(sys.argv[2]), Path(sys.argv[3]))
        return 0
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--execute', action='store_true')
    parser.add_argument('--device', required=True)
    parser.add_argument('--plugin-path', required=True,
                        help='directory containing the private patched V4L2 plugin')
    parser.add_argument('--output', required=True, type=Path, help='new result directory')
    parser.add_argument('--codec', choices=['h264', 'h265'], default='h265')
    parser.add_argument('--width', type=int, default=1366)
    parser.add_argument('--height', type=int, default=768)
    parser.add_argument('--frames', type=int, default=180)
    parser.add_argument('--timeout', type=float, default=45)
    parser.add_argument('--terminate-after', type=float,
                        help='deliberately send TERM during encoding to exercise close')
    args = parser.parse_args()
    if (not 256 <= args.width <= 3840 or not 128 <= args.height <= 2160 or
            args.width % 2 or args.height % 2 or not 1 <= args.frames <= 100000 or
            not 1 <= args.timeout <= 300 or
            (args.terminate_after is not None and
             not 0 < args.terminate_after < args.timeout)):
        parser.error('invalid geometry, frame count, timeout or termination time')
    args.output = args.output.absolute()
    elementary = args.output / f'output.{args.codec}'
    command = ['gst-launch-1.0', '-e', 'videotestsrc', 'is-live=true',
               f'num-buffers={args.frames}', '!',
               f'video/x-raw,format=NV12,width={args.width},height={args.height},framerate=30/1',
               '!', f'v4l2{args.codec}enc', f'device={args.device}', '!']
    if args.codec == 'h265':
        command += ['video/x-h265,profile=main,level=(string)5.1', '!']
    command += [f'{args.codec}parse', '!', 'filesink', f'location={elementary}']
    print(json.dumps(dict(execute=args.execute, command=command,
                          plugin_path=args.plugin_path)), flush=True)
    if not args.execute:
        return 0
    if sys.platform != 'linux' or os.geteuid() != 0:
        parser.error('--execute requires Linux root inside the exclusive board window')
    device = Path(args.device).resolve(strict=True)
    name = Path('/sys/class/video4linux', device.name, 'name').read_text().strip()
    if name != 'C&M Wave5 VPU encoder':
        parser.error(f'{device} is not the Wave5 encoder: {name}')
    if video_owners(device):
        parser.error('encoder already open; stop video applications in the enclosing window')
    if not Path(args.plugin_path).is_dir():
        parser.error('private plugin directory is missing')
    args.output.mkdir(parents=True, exist_ok=False)
    report = dict(kernel=os.uname().release, command=command, started=time.time())
    (args.output / 'kernel.notes').write_bytes(Path('/sys/kernel/notes').read_bytes())
    environment = dict(os.environ, GST_PLUGIN_PATH=args.plugin_path,
                       GST_REGISTRY=str(args.output / 'registry.bin'),
                       GST_DEBUG='v4l2*:4')
    with (args.output / 'gst.log').open('wb') as log:
        process = subprocess.Popen(command, stdout=log, stderr=log, env=environment,
                                   stdin=subprocess.DEVNULL, start_new_session=True)
    report['pid'] = process.pid
    (args.output / 'run.json').write_text(json.dumps(report, indent=2) + '\n')
    timed_out = False
    observer_ok = True
    try:
        limit = args.terminate_after or args.timeout
        status = wait_until_exit(process, limit)
        timed_out = status is None and args.terminate_after is None
        if status is None:
            observer_ok = collect(process, args.output, 'before-term', report)
            signal_group(process, signal.SIGTERM)
            status = wait_until_exit(process, 5)
        if status is None:
            if observer_ok:
                observer_ok = collect(process, args.output, 'before-kill', report)
            signal_group(process, signal.SIGKILL)
            status = wait_until_exit(process, 3)
    finally:
        if process.poll() is None:
            signal_group(process, signal.SIGKILL)
            wait_until_exit(process, 1)
        if observer_ok:
            collect(process, args.output, 'final', report)
        tasks = sorted(p.name for p in Path(f'/proc/{process.pid}/task').glob('*'))
        report.update(exit_code=process.poll(), timed_out=timed_out, remaining_tasks=tasks,
                      ended=time.time())
        (args.output / 'run.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report), flush=True)
    if (report['exit_code'] is None or tasks or
            any(item['status'] is None for item in report.get('snapshots', []))):
        print('Tasks remain: STOP this window; do not start another encode or reboot loop.',
              file=sys.stderr)
        return 125
    if timed_out:
        return 124
    if any(item['status'] != 0 for item in report.get('snapshots', [])):
        return 1
    if args.terminate_after is not None:
        # Exit only proves cancellation liveness; inspect the bitstream separately.
        return 0 if report['exit_code'] in (0, -signal.SIGTERM) else 1
    return 0 if report['exit_code'] == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
