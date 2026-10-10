#!/usr/bin/env python3
"""Read bounded device-harness observations without inferring cheat acceptance."""
import argparse
import hashlib
import json
import re
from pathlib import Path

LOG_LIMIT = 65536
LINE_LIMIT = 2047
U32 = (0, (1 << 32) - 1)
U64 = (0, (1 << 64) - 1)
I32 = (-(1 << 31), (1 << 31) - 1)
DATA_NAMES = (
    'direct32', 'direct16', 'direct8', 'copy', 'repeat0', 'repeat1', 'repeat2',
    'pointer', 'source', 'target_a0', 'target_a1', 'target_a2',
    'target_b0', 'target_b1', 'target_b2', 'condition', 'pad', 'patch', 'module_marker',
)


def _object(value, keys, where):
    if type(value) is not dict or set(value) != set(keys.split()):
        raise ValueError(f'{where}: missing or unknown fields')


def _integer(value, bounds, where):
    if type(value) is not int or not bounds[0] <= value <= bounds[1]:
        raise ValueError(f'{where}: integer outside {bounds}')


def _array(value, bounds, where):
    if type(value) is not list or len(value) != len(bounds):
        raise ValueError(f'{where}: wrong array length')
    for index, (item, limit) in enumerate(zip(value, bounds)):
        _integer(item, limit, f'{where}[{index}]')


def _unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f'duplicate JSON field: {key}')
        result[key] = value
    return result


def validate(record):
    _object(record, 'schema kind version title_id run sequence tick_us pid page buttons source '
            'codes_result data bulk rx thumb32 cpu module witness', 'observation')
    _integer(record['schema'], (1, 1), 'schema')
    for key, expected in (('kind', 'observation'), ('version', '00.95'), ('title_id', 'VCOCH0001')):
        if record[key] != expected:
            raise ValueError(f'{key}: expected {expected}')
    if type(record['run']) is not str or not re.fullmatch(r'[0-9a-f]{16}', record['run']):
        raise ValueError('run: expected 16 lowercase hexadecimal digits')
    run = int(record['run'], 16)
    if not run:
        raise ValueError('run: zero clock')
    for key, bounds in (('sequence', (1, 60)), ('tick_us', U64), ('pid', (1, I32[1])),
                        ('page', (0, 1)), ('buttons', U32), ('source', (0, 2))):
        _integer(record[key], bounds, key)
    if record['tick_us'] < run:
        raise ValueError('tick_us: precedes run clock')
    for key, bounds in (('codes_result', [I32] * 2), ('data', [U32] * 19),
                        ('bulk', [U32] * 3), ('rx', [(0, 65535)] * 2),
                        ('thumb32', [(0, 65535)] * 4), ('cpu', [I32] * 2)):
        _array(record[key], bounds, key)
    module = record['module']
    _object(module, 'state uid generation variant loads unloads error native status index '
            'data_address function_address reading', 'module')
    for key in ('uid', 'native', 'status'):
        _integer(module[key], I32, f'module.{key}')
    for key in ('generation', 'variant', 'loads', 'unloads', 'index',
                'data_address', 'function_address'):
        _integer(module[key], U32, f'module.{key}')
    _integer(module['state'], (0, 4), 'module.state')
    _integer(module['error'], (0, 5), 'module.error')
    _array(module['reading'], [U32, I32, (0, 65535), (0, 65535), (0, 1)], 'module.reading')
    witness = record['witness']
    _object(witness, 'active samples generations reused_data reused_function error first '
            'first_address origin expected', 'witness')
    _integer(witness['active'], (0, 1), 'witness.active')
    _integer(witness['samples'], U64, 'witness.samples')
    _integer(witness['error'], (0, 5), 'witness.error')
    for key in ('generations', 'reused_data', 'reused_function'):
        _integer(witness[key], U32, f'witness.{key}')
    _array(witness['first'], [U32, I32, U32, I32, (0, 65535), (0, 65535), (0, 1)], 'witness.first')
    for key, size in (('first_address', 2), ('origin', 3), ('expected', 2)):
        _array(witness[key], [U32] * size, f'witness.{key}')


def analyze_bytes(data):
    if len(data) > LOG_LIMIT:
        raise ValueError('log exceeds device limit of 65536 bytes')
    end = data.rfind(b'\n') + 1
    tail = data[end:]
    runs = []
    seen = set()
    current = None
    total = 0
    for number, line in enumerate(data[:end].splitlines(keepends=True), 1):
        if not line.endswith(b'\n') or len(line) > LINE_LIMIT:
            raise ValueError(f'line {number}: invalid record boundary or length')
        try:
            record = json.loads(line.decode('utf-8'), object_pairs_hook=_unique)
            validate(record)
        except (ValueError, UnicodeDecodeError, RecursionError) as error:
            raise ValueError(f'line {number}: {error}') from error
        key = (record['pid'], record['run'])
        if current is None or key != (current['pid'], current['run']):
            if key in seen:
                raise ValueError(f'line {number}: a closed run reappeared')
            seen.add(key)
            current = {'pid': key[0], 'run': key[1], 'observations': [], 'first_fault': None}
            runs.append(current)
        expected_sequence = len(current['observations']) + 1
        if record['sequence'] != expected_sequence:
            raise ValueError(f'line {number}: expected sequence {expected_sequence}')
        if current['observations'] and record['tick_us'] < current['observations'][-1]['tick_us']:
            raise ValueError(f'line {number}: clock moved backwards')
        record['named_data'] = dict(zip(DATA_NAMES, record['data']))
        record['elapsed_us'] = record['tick_us'] - int(record['run'], 16)
        current['observations'].append(record)
        if current['first_fault'] is None and (record['module']['state'] == 4 or
                                              record['module']['error'] or
                                              record['witness']['error']):
            current['first_fault'] = {
                'sequence': record['sequence'], 'module': record['module'],
                'witness': record['witness'],
            }
        total += 1
    return {
        'schema': 1, 'source_sha256': hashlib.sha256(data).hexdigest(),
        'source_bytes': len(data), 'complete_observations': total,
        'truncated_tail_bytes': len(tail), 'runs': runs,
        'scope': 'Observed values only. ON/OFF, effects and lifecycle acceptance require the manual test record.',
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    try:
        with args.log.open('rb') as source:
            report = analyze_bytes(source.read(LOG_LIMIT + 1))
    except (OSError, ValueError) as error:
        parser.exit(1, f'{error}\n')
    rendered = json.dumps(report, ensure_ascii=False, indent=2) + '\n'
    if args.output:
        args.output.write_text(rendered, encoding='utf-8')
        print(f"{report['complete_observations']} observations in {len(report['runs'])} runs; "
              f"{report['truncated_tail_bytes']} incomplete tail bytes. No automatic acceptance.")
    else:
        print(rendered, end='')


if __name__ == '__main__':
    main()
