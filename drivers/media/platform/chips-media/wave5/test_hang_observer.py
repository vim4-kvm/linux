#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Test observer deadlines with owned child processes; no video device access."""
import importlib.util
from pathlib import Path
import signal
import subprocess
import sys
import unittest
from unittest.mock import Mock, patch

spec = importlib.util.spec_from_file_location('observer', Path(__file__).with_name('reproduce_hang.py'))
observer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(observer)


class ObserverTest(unittest.TestCase):
    def test_unkillable_child_does_not_extend_deadline(self):
        process = Mock()
        process.poll.return_value = None
        with patch.object(observer.time, 'monotonic', side_effect=[0, 0.1, 1.1]), \
                patch.object(observer.time, 'sleep') as sleep:
            self.assertIsNone(observer.wait_until_exit(process, 1))
            sleep.assert_called_once()

    def test_term_then_kill_owned_process_group(self):
        # The pipe handshake ensures TERM is ignored before testing the timeout.
        process = subprocess.Popen(
            [sys.executable, '-c',
             'import signal; signal.signal(signal.SIGTERM, signal.SIG_IGN); '
             'print("ready", flush=True); signal.pause()'],
            stdout=subprocess.PIPE, start_new_session=True, text=True)
        try:
            self.assertEqual(process.stdout.readline().strip(), 'ready')
            observer.signal_group(process, signal.SIGTERM)
            self.assertIsNone(observer.wait_until_exit(process, 0))
            observer.signal_group(process, signal.SIGKILL)
            self.assertEqual(observer.wait_until_exit(process, 3), -signal.SIGKILL)
            observer.signal_group(process, signal.SIGKILL)  # already gone is harmless
        finally:
            process.kill()
            process.wait(timeout=3)
            process.stdout.close()


if __name__ == '__main__':
    unittest.main()
