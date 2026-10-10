#!/usr/bin/env python3
"""Check the actual C wire output plus malformed/reset/partial capture handling."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('resource_profile', ROOT / 'scripts/resource_profile.py')
profile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)
EXECUTABLE = sys.argv.pop(1) if len(sys.argv) > 1 else None


def sample(sequence, tick=None, phase=0, enabled=0, state=1, session=1,
           control_uid=12, control_clock=None, observer_clock=None, memory_error=0):
    tick = (sequence - 1) * 5000000 if tick is None else tick
    control_clock = 0x1234567800000000 + sequence * 200 if control_clock is None else control_clock
    observer_clock = sequence * 10 if observer_clock is None else observer_clock
    memory = (100000 - sequence * 100, 200000, 300000) if not memory_error else (0, 0, 0)
    return (f'rp_sample {sequence:04x} {tick:016x} {tick + 123:016x} {phase:08x} '
            f'{enabled:08x} {state:08x} {session:016x} 0x00000000\n'
            f'rp_observer {sequence:04x} 0000000b {observer_clock:016x} 00002000 0x00000000\n'
            f'rp_control {sequence:04x} {control_uid:08x} {control_clock:016x} 00008000 0x00000000\n'
            f'rp_memory {sequence:04x} {memory[0]:08x} {memory[1]:08x} {memory[2]:08x} '
            f'0x{memory_error:08x}\n'
            f'rp_commit {sequence:04x} 0x00000000\n')


def capture(*samples):
    return 'rp_start 0.8.34 0x0000003c\n' + ''.join(samples)


class AnalysisTests(unittest.TestCase):
    def test_actual_c_output(self):
        self.assertIsNotNone(EXECUTABLE)
        text = subprocess.check_output([EXECUTABLE, 'emit'], text=True)
        result = profile.analyze(text)
        self.assertEqual(result['committed_samples'], 3)
        self.assertEqual(result['capture_end']['reason'], 1)
        self.assertEqual(result['rejected_intervals'][0]['reason'], 'context_transition')
        self.assertEqual(result['intervals'][0]['wall_us'], 5000000)
        self.assertEqual(result['intervals'][0]['control']['raw_clock_delta'], 2)
        self.assertEqual(result['samples'][0]['control']['stack_bytes'], 32768)
        self.assertEqual(result['samples'][0]['free_pool_bytes']['user'], 12345678)
        self.assertFalse(result['truncated_tail'])

    def test_actual_busy_c_output_continues_and_commits_end(self):
        for position in range(2, 7):
            with self.subTest(position=position):
                text = subprocess.check_output([EXECUTABLE, 'emit_busy', str(position)], text=True)
                result = profile.analyze(text)
                self.assertEqual(result['committed_samples'], 2)
                self.assertEqual(result['incomplete_samples'], [] if position == 2 else [1])
                self.assertEqual(result['capture_end']['reason'], 1)
                self.assertEqual(result['capture_end']['sequence'], 3)
                self.assertEqual(result['capture_end']['tick_us'], 300000000)
                self.assertEqual([sample['sequence'] for sample in result['samples']], [2, 3])

    def test_interrupted_write_never_commits_partial_sample(self):
        complete = capture(sample(1))
        parts = sample(2).splitlines(keepends=True)
        for count in range(1, 5):
            with self.subTest(count=count):
                result = profile.analyze(complete + ''.join(parts[:count]))
                self.assertEqual(result['committed_samples'], 1)
                self.assertEqual(result['incomplete_samples'], [2])
                self.assertEqual(result['intervals'], [])
        result = profile.analyze(complete + sample(2).removesuffix('\n'))
        self.assertEqual(result['committed_samples'], 1)
        self.assertEqual(result['incomplete_samples'], [2])
        self.assertTrue(result['truncated_tail'])

    def test_counter_units_are_kept_raw(self):
        result = profile.analyze(capture(sample(1), sample(2)))
        interval = result['intervals'][0]
        self.assertEqual(interval['control']['raw_clock_delta'], 200)
        self.assertEqual(interval['observer']['raw_clock_delta'], 10)
        self.assertEqual(result['phases'][0]['free_pool_bytes']['user']['net_free_change'], -100)
        self.assertGreater(result['samples'][0]['control']['raw_run_clocks'], 2**32)
        self.assertTrue(any('No CPU percentage' in item for item in result['limitations']))

    def test_uid_replacement_or_counter_reset_does_not_become_utilization(self):
        for following in (sample(2, control_uid=13), sample(2, control_clock=1)):
            result = profile.analyze(capture(sample(1), following))
            self.assertIsNone(result['intervals'][0]['control']['raw_clock_delta'])
            self.assertEqual(result['intervals'][0]['control']['reason'], 'thread_or_counter_changed')

    def test_transitions_gaps_and_saturated_visibility(self):
        for following in (sample(2, phase=4), sample(2, enabled=1), sample(2, state=2),
                          sample(2, session=2)):
            result = profile.analyze(capture(sample(1), following))
            self.assertEqual(result['intervals'], [])
            self.assertEqual(result['rejected_intervals'][0]['reason'], 'context_transition')
        result = profile.analyze(capture(sample(1), sample(3)))
        self.assertEqual(result['rejected_intervals'][0]['reason'], 'sample_gap')
        result = profile.analyze(capture(sample(1, phase=0xffffffff), sample(2, phase=0xffffffff)))
        self.assertEqual(result['intervals'], [])
        # Closing/reopening is a new memory phase despite equal visibility.
        result = profile.analyze(capture(sample(1, phase=3), sample(2, phase=7)))
        self.assertEqual(len(result['phases']), 2)

    def test_query_failures_are_not_zero_memory_measurements(self):
        text = capture(sample(1, memory_error=0x80020001), sample(2))
        text = text.replace('rp_control 0001 0000000c 12345678000000c8 00008000 0x00000000',
                            'rp_control 0001 0000000c 0000000000000000 00000000 0x80020001')
        result = profile.analyze(text)
        self.assertEqual(result['phases'][0]['valid_memory_samples'], 1)
        self.assertEqual(result['phases'][0]['free_pool_bytes']['user']['min'], 99800)
        self.assertEqual(result['intervals'][0]['control']['reason'], 'query_failed')

    def test_malformed_and_out_of_order_records(self):
        valid = capture(sample(1), sample(2))
        corruptions = [
            valid.replace('0x0000003c', '0x0000003d', 1),
            valid.replace('00008000', '0000800', 1),
            valid.replace('rp_commit 0001', 'rp_commit 0002', 1),
            valid.replace('rp_commit 0001 0x00000000', 'rp_commit 0001 0x00000001', 1),
            valid.replace('rp_memory 0001', 'rp_memory 0000', 1),
            valid.replace(' 0001863c ', ' 8001863c ', 1),
            valid + 'rp_commit 0002 0x00000000\n',
            capture(sample(1), sample(61)),
            capture(sample(1), sample(2, tick=4999999)),
            capture(sample(1), sample(2, tick=300000000)),
            sample(1),
            valid + 'rp_start 0.8.34 0x0000003c\n',
            valid + 'rp_end 0002 0000000001312d00 0x00000001\n' + sample(3),
        ]
        for text in corruptions:
            with self.subTest(text=text[:120]):
                self.assertNotEqual(valid, text)
                with self.assertRaises(ValueError):
                    profile.analyze(text)

    def test_clock_failure_before_first_sample(self):
        result = profile.analyze('rp_start 0.8.34 0x0000003c\n'
                                 'rp_end 0000 0000000000000000 0x00000002\n')
        self.assertEqual(result['committed_samples'], 0)
        self.assertIsNone(result['acquisition_us'])
        self.assertEqual(result['capture_end']['reason'], 2)

    def test_cli_retains_source_provenance(self):
        data = capture(sample(1), sample(2)).encode('ascii')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'capture.log'
            output = Path(directory) / 'result.json'
            path.write_bytes(data)
            subprocess.run([sys.executable, str(ROOT / 'scripts/resource_profile.py'), str(path),
                            '--output', str(output)], check=True)
            result = json.loads(output.read_text())
            self.assertEqual(result['source']['sha256'], hashlib.sha256(data).hexdigest())
            self.assertEqual(result['source']['bytes'], len(data))


if __name__ == '__main__':
    unittest.main()
