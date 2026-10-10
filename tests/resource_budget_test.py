#!/usr/bin/env python3
"""Verify metadata/accounting inputs without pretending to measure device RAM."""
import importlib.util
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('resource_budget', ROOT / 'scripts/resource_budget.py')
budget = importlib.util.module_from_spec(spec)
spec.loader.exec_module(budget)


def metadata():
    data = bytearray(0x160)
    data[:4] = b'SCE\0'
    struct.pack_into('<QQ', data, 0x40, 0x80, 0x100)
    data[0x80:0x87] = b'\x7fELF\x01\x01\x01'
    struct.pack_into('<HH', data, 0x80 + 16, 0xfe04, 40)
    struct.pack_into('<HH', data, 0x80 + 42, 32, 3)
    struct.pack_into('<8I', data, 0x100, 1, 0, 0x81000000, 0, 100, 100, 5, 4096)
    struct.pack_into('<8I', data, 0x120, 1, 0, 0x81020000, 0, 4, 9000, 6, 4096)
    struct.pack_into('<8I', data, 0x140, 0x60000000, 0, 0, 0, 1000, 1000, 0, 4)
    return data


def abi_values():
    values = dict.fromkeys(budget.ABI_FIELDS, 1)
    values.update(pointer_bytes=4, size_t_bytes=4, wchar_t_bytes=2,
                  entry_bytes=24, code_bytes=16, op_bytes=44, patch_bytes=16,
                  entries_limit=256, codes_limit=8192)
    return values


def abi_data(values):
    return b''.join(struct.pack('<40sI', name.encode('ascii'), value) for name, value in values.items())


class BudgetInputs(unittest.TestCase):
    def test_declared_bss_and_pages(self):
        rows = budget.self_segments(metadata())
        self.assertEqual(rows[0]['zero_fill_bytes'], 0)
        self.assertEqual(rows[1]['file_bytes'], 4)
        self.assertEqual(rows[1]['memory_bytes'], 9000)
        self.assertEqual(rows[1]['zero_fill_bytes'], 8996)
        self.assertEqual(rows[1]['page_rounded_model_bytes'], 12288)
        self.assertEqual(len(rows), 2)  # Relocations are not resident PT_LOAD data.

    def test_truncated_tables(self):
        data = metadata()
        for end in range(len(data)):
            with self.subTest(end=end):
                with self.assertRaises(ValueError):
                    budget.self_segments(data[:end])

    def test_invalid_program_tables(self):
        changes = [(0x40, '<Q', 0xffffffffffffffff), (0x80 + 16, '<H', 2),
                   (0x80 + 18, '<H', 3), (0x80 + 42, '<H', 16),
                   (0x80 + 44, '<H', 0), (0x80 + 44, '<H', 17),
                   (0x120 + 20, '<I', 3), (0x120 + 20, '<I', 0x1000001),
                   (0x120 + 8, '<I', 0xfffff000), (0x120 + 24, '<I', 7),
                   (0x120 + 28, '<I', 0), (0x120, '<I', 0)]
        for offset, fmt, value in changes:
            with self.subTest(offset=offset, value=value):
                data = metadata()
                struct.pack_into(fmt, data, offset, value)
                with self.assertRaises(ValueError):
                    budget.self_segments(data)

    def test_wrong_metadata_format(self):
        for offset, value in ((0, ord('X')), (0x80 + 4, 2), (0x80 + 5, 2)):
            data = metadata()
            data[offset] = value
            with self.assertRaises(ValueError):
                budget.self_segments(data)

    def test_arm_probe_records(self):
        values = abi_values()
        self.assertEqual(budget.decode_abi(abi_data(values)), values)
        for name, value in (('pointer_bytes', 8), ('size_t_bytes', 8), ('wchar_t_bytes', 4),
                            ('schema', 2), ('op_bytes', 0), ('upload_max_bytes', 0x1000001)):
            invalid = dict(values, **{name: value})
            with self.subTest(name=name):
                with self.assertRaises(ValueError):
                    budget.decode_abi(abi_data(invalid))

    def test_probe_unknown_duplicate_and_missing(self):
        data = abi_data(abi_values())
        for invalid in (data[:-1], b'', data + data[:44],
                        abi_data({**abi_values(), 'unknown': 1}), data[44:]):
            with self.assertRaises(ValueError):
                budget.decode_abi(invalid)
        invalid = bytearray(data)
        invalid[0] = ord(' ')
        with self.assertRaises(ValueError):
            budget.decode_abi(invalid)

    def test_admitted_arena_counts_and_alignment(self):
        abi = abi_values()
        self.assertEqual(budget.arena_bytes(abi, 0, 0), 104)
        self.assertEqual(budget.arena_bytes(abi, 1, 1), 104)
        self.assertEqual(budget.arena_bytes(abi, 256, 8192), 628736)
        self.assertEqual(budget.page(628736), 630784)
        for entries, codes in ((-1, 1), (257, 1), (1, 8193), (1, -1), (True, 1), (1, 1.0)):
            with self.subTest(entries=entries, codes=codes):
                with self.assertRaises(ValueError):
                    budget.arena_bytes(abi, entries, codes)

    def test_startup_and_resident_stack_separation(self):
        rows = budget.thread_requests()
        shell = [r for r in rows if r['process'] == 'shell' and r['lifetime'] == 'resident']
        game = [r for r in rows if r['process'] == 'game']
        startup = [r for r in rows if r['lifetime'] == 'startup']
        self.assertEqual(sum(r['requested_stack_bytes'] for r in shell), 81920)
        self.assertEqual(sum(r['requested_stack_bytes'] for r in game), 24576)
        self.assertEqual(sum(r['requested_stack_bytes'] for r in startup), 32768)
        self.assertNotIn('vco_boot', [r['name'] for r in shell])


if __name__ == '__main__':
    unittest.main()
