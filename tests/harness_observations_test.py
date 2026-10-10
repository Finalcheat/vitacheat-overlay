#!/usr/bin/env python3
"""Independent JSON consumer checks against the actual C observation producer."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('harness_observations', ROOT / 'scripts/harness_observations.py')
reader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reader)
EXECUTABLE = sys.argv.pop(1)
BASE = subprocess.check_output([EXECUTABLE, 'emit'])
EXTREMES = subprocess.check_output([EXECUTABLE, 'extremes'])


def line(record):
    return (json.dumps(record, separators=(',', ':')) + '\n').encode()


class Observations(unittest.TestCase):
    def record(self):
        return json.loads(BASE)

    def test_c_output(self):
        report = reader.analyze_bytes(BASE)
        self.assertEqual(report['complete_observations'], 1)
        self.assertEqual(report['truncated_tail_bytes'], 0)
        record = report['runs'][0]['observations'][0]
        self.assertEqual(record['rx'], [0x2007, 0x4770])
        self.assertEqual(record['thumb32'], [0xbf00, 0xf04f, 7, 0x4770])
        self.assertEqual(record['cpu'], [7, 7])
        self.assertEqual(record['bulk'], [117, 0, 0])
        self.assertEqual(record['named_data']['patch'], 0)
        self.assertEqual(record['elapsed_us'], 200)
        self.assertNotIn('passed', report)
        self.assertIsNone(report['runs'][0]['first_fault'])

    def test_integer_boundaries_and_fault(self):
        record = reader.analyze_bytes(EXTREMES)['runs'][0]['observations'][0]
        self.assertEqual(record['pid'], (1 << 31) - 1)
        self.assertEqual(record['data'], [(1 << 32) - 1] * 19)
        self.assertEqual(record['module']['native'], -(1 << 31))
        self.assertEqual(record['witness']['samples'], (1 << 64) - 1)
        self.assertEqual(record['witness']['first'][1], -(1 << 31))
        self.assertEqual(record['elapsed_us'], 1)
        fault = reader.analyze_bytes(EXTREMES)['runs'][0]['first_fault']
        self.assertEqual(fault['sequence'], 1)
        self.assertEqual(fault['witness']['error'], 4)

    def test_truncated_tail(self):
        for suffix in (b'{', b'{"data":', b'\xe4\xb8', BASE[:-1]):
            with self.subTest(suffix=suffix[:12]):
                report = reader.analyze_bytes(BASE + suffix)
                self.assertEqual(report['complete_observations'], 1)
                self.assertEqual(report['truncated_tail_bytes'], len(suffix))

    def test_empty(self):
        self.assertEqual(reader.analyze_bytes(b'')['complete_observations'], 0)
        self.assertEqual(reader.analyze_bytes(b'{')['complete_observations'], 0)

    def test_complete_corruption(self):
        for malformed in (b'{\n', b'\xff\n', b'\n', b'{}\n', b'[]\n',
                          b' ' * 2048 + b'\n', b'[' * 1000 + b']' * 1000 + b'\n',
                          BASE.replace(b'"schema":1', b'"schema":1,"schema":1')):
            with self.subTest(malformed=malformed[:20]):
                with self.assertRaises(ValueError):
                    reader.analyze_bytes(malformed)

    def test_invalid_fields(self):
        changes = [('schema', True), ('version', '00.94'), ('run', '0000000000000000'),
                   ('run', 'not a clock'), ('pid', -1), ('sequence', 61), ('page', 2),
                   ('source', 3), ('tick_us', 999), ('tick_us', 1 << 64),
                   ('data', [0] * 18), ('cpu', [False, 7]), ('rx', [65536, 0])]
        for name, value in changes:
            with self.subTest(name=name, value=value):
                record = self.record()
                record[name] = value
                with self.assertRaises(ValueError):
                    reader.analyze_bytes(line(record))
        record = self.record()
        record['unrecognized'] = 0
        with self.assertRaises(ValueError):
            reader.analyze_bytes(line(record))
        del record['unrecognized'], record['data']
        with self.assertRaises(ValueError):
            reader.analyze_bytes(line(record))

    def test_nested_ranges(self):
        for group, key, value in (
            ('module', 'native', -(1 << 31) - 1), ('module', 'state', 5),
            ('module', 'error', 6), ('module', 'reading', [0, 0, 0, 0, 2]),
            ('witness', 'error', 6), ('witness', 'active', True),
            ('witness', 'first', [0] * 6), ('witness', 'origin', [0] * 4),
            ('witness', 'first_address', [-1, 0]),
        ):
            with self.subTest(group=group, key=key):
                record = self.record()
                record[group][key] = value
                with self.assertRaises(ValueError):
                    reader.analyze_bytes(line(record))

    def test_sequence_clock_and_runs(self):
        second = self.record()
        second['sequence'] = 2
        second['tick_us'] += 1
        report = reader.analyze_bytes(BASE + line(second) + EXTREMES)
        self.assertEqual([len(run['observations']) for run in report['runs']], [2, 1])
        for malformed in (BASE + BASE, line(second), BASE + line({**second, 'sequence': 3}),
                          BASE + line({**second, 'tick_us': 1100}), BASE + EXTREMES + line(second)):
            with self.subTest(malformed=malformed[-30:]):
                with self.assertRaises(ValueError):
                    reader.analyze_bytes(malformed)

    def test_first_fault_preserved(self):
        first = self.record()
        first['witness']['error'] = 2
        first['witness']['first'][2] = 88
        later = copy.deepcopy(first)
        later['sequence'] = 2
        later['witness']['first'][2] = 123
        report = reader.analyze_bytes(line(first) + line(later))
        self.assertEqual(report['runs'][0]['first_fault']['witness']['first'][2], 88)
        self.assertEqual(report['runs'][0]['observations'][1]['witness']['first'][2], 123)

    def test_unexpected_cpu_is_observed(self):
        record = self.record()
        record['cpu'][0] = -1
        record['rx'][0] = 0xffff
        result = reader.analyze_bytes(line(record))['runs'][0]['observations'][0]
        self.assertEqual(result['cpu'][0], -1)
        self.assertEqual(result['rx'][0], 0xffff)

    def test_file_limit(self):
        with self.assertRaises(ValueError):
            reader.analyze_bytes(b' ' * (reader.LOG_LIMIT + 1))

    def test_cli_provenance_and_errors(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'log.jsonl'
            output = Path(directory) / 'report.json'
            source.write_bytes(BASE)
            command = [sys.executable, str(ROOT / 'scripts/harness_observations.py'),
                       str(source), '--output', str(output)]
            subprocess.run(command, check=True, capture_output=True)
            report = json.loads(output.read_text())
            self.assertEqual(report['source_sha256'], hashlib.sha256(BASE).hexdigest())
            self.assertEqual(report['complete_observations'], 1)
            output.unlink()
            source.write_bytes(b'{\n')
            failed = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(failed.returncode, 1)
            self.assertIn('line 1', failed.stderr)
            self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
