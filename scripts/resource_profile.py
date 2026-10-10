#!/usr/bin/env python3
"""Analyze a bounded VCO developer capture without inventing CPU/RAM attribution."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import statistics

WIDTHS = {
    'rp_sample': (4, 16, 16, 8, 8, 8, 16),
    'rp_observer': (4, 8, 16, 8),
    'rp_control': (4, 8, 16, 8),
    'rp_memory': (4, 8, 8, 8),
    'rp_commit': (4,),
    'rp_end': (4, 16),
}
PARTS = {'rp_sample', 'rp_observer', 'rp_control', 'rp_memory', 'rp_commit'}


def parse(text):
    """Incomplete tail samples are evidence of interrupted writes, not zeros."""
    version = None
    limit = None
    groups = {}
    ended = None
    latest = 0
    previous_tick = None
    previous_begin = None
    first_begin = None
    lines = text.splitlines()
    truncated_tail = bool(text) and not text.endswith('\n')
    if truncated_tail:
        lines = lines[:-1]
    for number, line in enumerate(lines, 1):
        if not line:
            continue
        fields = line.split(' ')
        if not re.fullmatch(r'0x[0-9a-f]{8}', fields[-1]):
            raise ValueError(f'Line {number}: invalid record value')
        result = int(fields.pop(), 16)
        kind = fields.pop(0)
        if kind == 'rp_start':
            if version is not None or groups or len(fields) != 1 or result != 60:
                raise ValueError(f'Line {number}: invalid or duplicate capture header')
            if not re.fullmatch(r'\d+\.\d+\.\d+', fields[0]):
                raise ValueError(f'Line {number}: invalid version')
            version, limit = fields[0], result
            continue
        if version is None or ended is not None or kind not in WIDTHS:
            raise ValueError(f'Line {number}: missing header, unknown event, or record after end')
        widths = WIDTHS[kind]
        if len(fields) != len(widths) or any(
                not re.fullmatch(rf'[0-9a-f]{{{width}}}', field)
                for field, width in zip(fields, widths)):
            raise ValueError(f'Line {number}: invalid {kind} fields')
        values = tuple(int(field, 16) for field in fields)
        seq = values[0]
        if kind == 'rp_end':
            if seq < latest or seq > limit or result not in (1, 2, 3):
                raise ValueError(f'Line {number}: invalid capture end')
            ended = {'sequence': seq, 'tick_us': values[1], 'reason': result}
            continue
        if not 1 <= seq <= limit:
            raise ValueError(f'Line {number}: sample limit exceeded')
        if kind == 'rp_sample':
            if seq <= latest or values[2] < values[1] or values[5] > 6 or values[4] > 256:
                raise ValueError(f'Line {number}: invalid sample identity/timestamps/state')
            if previous_tick is not None and values[1] < previous_tick:
                raise ValueError(f'Line {number}: overlapping or reversed acquisition')
            if previous_begin is not None and values[1] - previous_begin < 5000000:
                raise ValueError(f'Line {number}: sampling cadence violated')
            if first_begin is None:
                first_begin = values[1]
            if values[1] - first_begin >= 300000000:
                raise ValueError(f'Line {number}: capture window exceeded')
            previous_begin = values[1]
            latest, previous_tick = seq, values[2]
            groups[seq] = {}
        if seq != latest or kind in groups.get(seq, {}) or seq not in groups:
            raise ValueError(f'Line {number}: duplicate or misplaced sample part')
        group = groups[seq]
        if 'rp_commit' in group or (kind == 'rp_commit' and set(group) != PARTS - {'rp_commit'}):
            raise ValueError(f'Line {number}: invalid sample commit')
        if kind in ('rp_sample', 'rp_commit') and result:
            raise ValueError(f'Line {number}: unexpected sample/commit error')
        if kind in ('rp_observer', 'rp_control'):
            if result and any(values[2:]):
                raise ValueError(f'Line {number}: failed query contains usable counters')
            if not result and (not values[1] or values[1] > 0x7fffffff or
                               values[3] > 0x7fffffff):
                raise ValueError(f'Line {number}: invalid thread identity/stack size')
        if kind == 'rp_memory':
            if result and any(values[1:]):
                raise ValueError(f'Line {number}: failed query contains usable pool sizes')
            if not result and any(value > 0x7fffffff for value in values[1:]):
                raise ValueError(f'Line {number}: invalid pool size')
        group[kind] = (values, result)
    if version is None:
        raise ValueError('No valid resource capture header; do not use an old or truncated log')
    samples = []
    incomplete = []
    for seq, group in groups.items():
        if set(group) != PARTS:
            incomplete.append(seq)
            continue
        base, _ = group['rp_sample']
        sample = dict(sequence=seq, tick_us=base[1], acquisition_us=base[2] - base[1],
                      visibility_epoch=base[3] >> 1, visible=bool(base[3] & 1),
                      desired_enabled_count=base[4], execution_state=base[5],
                      session=f'{base[6]:016x}')
        for label in ('observer', 'control'):
            fields, error = group['rp_' + label]
            sample[label] = {'uid': fields[1], 'raw_run_clocks': fields[2],
                             'stack_bytes': fields[3], 'error': f'{error:08x}'}
        fields, error = group['rp_memory']
        sample['free_pool_bytes'] = dict(zip(('user', 'cdram', 'phycont'), fields[1:]))
        sample['memory_error'] = f'{error:08x}'
        samples.append(sample)
    return version, samples, incomplete, ended, truncated_tail


def analyze(text):
    version, samples, incomplete, ended, truncated_tail = parse(text)
    intervals = []
    rejected = []
    phases = {}
    key_fields = ('visible', 'visibility_epoch', 'desired_enabled_count', 'execution_state', 'session')
    for current in samples:
        key = tuple(current[field] for field in key_fields)
        phase = phases.setdefault(key, {'context': dict(zip(key_fields, key)), 'samples': []})
        phase['samples'].append(current)
    for previous, current in zip(samples, samples[1:]):
        reason = None
        if current['sequence'] != previous['sequence'] + 1:
            reason = 'sample_gap'
        elif (any(current[field] != previous[field] for field in key_fields) or
              current['visibility_epoch'] != previous['visibility_epoch'] or
              current['visibility_epoch'] == 0x7fffffff):
            reason = 'context_transition'
        elif current['tick_us'] <= previous['tick_us']:
            reason = 'nonpositive_interval'
        if reason:
            rejected.append({'from': previous['sequence'], 'to': current['sequence'],
                             'reason': reason})
            continue
        interval = {'from': previous['sequence'], 'to': current['sequence'],
                    'wall_us': current['tick_us'] - previous['tick_us']}
        for label in ('observer', 'control'):
            old, new = previous[label], current[label]
            if old['error'] != '00000000' or new['error'] != '00000000':
                interval[label] = {'raw_clock_delta': None, 'reason': 'query_failed'}
            elif new['uid'] != old['uid'] or new['raw_run_clocks'] < old['raw_run_clocks']:
                interval[label] = {'raw_clock_delta': None, 'reason': 'thread_or_counter_changed'}
            else:
                interval[label] = {'raw_clock_delta': new['raw_run_clocks'] - old['raw_run_clocks']}
        intervals.append(interval)
    rollups = []
    for phase in phases.values():
        valid = [sample for sample in phase['samples'] if sample['memory_error'] == '00000000']
        pools = {}
        for label in ('user', 'cdram', 'phycont'):
            values = [sample['free_pool_bytes'][label] for sample in valid]
            pools[label] = (dict(min=min(values), max=max(values), first=values[0], last=values[-1],
                                 net_free_change=values[-1] - values[0]) if values else None)
        rollups.append(dict(context=phase['context'], samples=len(phase['samples']),
                            valid_memory_samples=len(valid), free_pool_bytes=pools))
    spans = [sample['acquisition_us'] for sample in samples]
    return {
        'schema': 1, 'source_version': version, 'capture_end': ended,
        'committed_samples': len(samples), 'incomplete_samples': incomplete,
        'truncated_tail': truncated_tail,
        'acquisition_us': dict(max=max(spans), median=statistics.median(spans)) if spans else None,
        'phases': rollups, 'intervals': intervals, 'rejected_intervals': rejected,
        'samples': samples,
        'limitations': [
            'runClocks is a raw SDK counter; units are not calibrated. No CPU percentage is computed.',
            'Only the observer/control threads are sampled, not kernel, PAF, catalog, or game threads.',
            'Free pools are reported by the current Shell process; changes are not VCO-owned memory.',
            'stack_bytes is the reported stack allocation, not stack usage or total plugin RAM.',
            'The sampler and log writer perturb the measured control thread and memory pools.',
            'A visible flag describes the page state; this capture contains no game frame-time data.',
            'Enabled/state/session endpoints cannot detect every unsampled execution transition.',
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    data = args.capture.read_bytes()
    try:
        report = analyze(data.decode('ascii'))
    except (UnicodeDecodeError, ValueError) as error:
        parser.error(str(error))
    report['source'] = {'path': str(args.capture), 'sha256': hashlib.sha256(data).hexdigest(),
                        'bytes': len(data)}
    output = json.dumps(report, indent=2, ensure_ascii=False) + '\n'
    if args.output:
        args.output.write_text(output, encoding='utf-8')
    else:
        print(output, end='')


if __name__ == '__main__':
    main()
