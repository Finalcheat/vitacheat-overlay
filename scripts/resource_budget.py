#!/usr/bin/env python3
"""Audit artifact segment sizes and ARM allocation policy; no device access."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
COMPONENTS = {'kernel': 'VitaCheatOverlayKernel.skprx', 'game': 'VitaCheatOverlayGame.suprx',
              'ui': 'VitaCheatOverlayUI.suprx', 'loader': 'VitaCheatOverlayShell.suprx'}
ABI_FIELDS = set(('schema pointer_bytes size_t_bytes wchar_t_bytes entry_bytes code_bytes '
                  'op_bytes patch_bytes catalog_workspace_bytes catalog_snapshot_bytes '
                  'game_executor_bytes game_native_memory_bytes game_pool_bytes game_worker_bytes '
                  'upload_max_bytes game_allocation_max_bytes game_blocks entries_limit '
                  'codes_limit active_limit claims_limit').split())
THREAD_SOURCES = (
    ('src/platform/catalog_service.c', 'shell', 'resident'),
    ('src/platform/execution_service.c', 'shell', 'resident'),
    ('src/platform/settings_service.c', 'shell', 'resident'),
    ('src/agent/vita_worker.c', 'game', 'resident'),
    ('src/shell.cpp', 'shell', 'startup'),
    ('src/loader.c', 'shell', 'startup'),
)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def page(n):
    return (n + 4095) & ~4095


def self_segments(data):
    def unpack(fmt, offset):
        if offset < 0 or offset + struct.calcsize(fmt) > len(data):
            raise ValueError('truncated SELF metadata')
        return struct.unpack_from(fmt, data, offset)

    if len(data) < 0x80 or data[:4] != b'SCE\0':
        raise ValueError('not a Vita SELF')
    elf, table = unpack('<QQ', 0x40)
    if data[elf:elf + 7] != b'\x7fELF\x01\x01\x01':
        raise ValueError('expected ELF32 little-endian metadata')
    elf_type, machine = unpack('<HH', elf + 16)
    stride, count = unpack('<HH', elf + 42)
    if elf_type not in (0xfe04, 0xfe07) or machine != 40 or stride != 32 or not 1 <= count <= 16:
        raise ValueError('unexpected Vita program table')
    if table + count * stride > len(data):
        raise ValueError('truncated program table')
    segments = []
    for i in range(count):
        kind, _, address, _, filesz, memsz, flags, alignment = unpack('<8I', table + i * stride)
        if kind != 1:
            continue
        if (not memsz or filesz > memsz or memsz > 16 * 1024 * 1024 or
                address + memsz > 1 << 32 or flags not in (5, 6) or alignment != 4096):
            raise ValueError('invalid load segment')
        segments.append({'index': i, 'flags': flags, 'virtual_address': address,
                         'file_bytes': filesz, 'memory_bytes': memsz,
                         'zero_fill_bytes': memsz - filesz,
                         'page_rounded_model_bytes': page(memsz)})
    if len(segments) != 2 or [segment['flags'] for segment in segments] != [5, 6]:
        raise ValueError('expected RX and RW load segments')
    return segments


def decode_abi(data):
    if not data or len(data) % 44:
        raise ValueError('invalid ABI probe record length')
    values = {}
    for offset in range(0, len(data), 44):
        raw_name, value = struct.unpack_from('<40sI', data, offset)
        name, separator, padding = raw_name.partition(b'\0')
        if not separator or any(padding) or not re.fullmatch(rb'[a-z_]+', name):
            raise ValueError('invalid ABI field name')
        name = name.decode('ascii')
        if name in values:
            raise ValueError('duplicate ABI field')
        values[name] = value
    if set(values) != ABI_FIELDS or any(not value or value > 16 * 1024 * 1024 for value in values.values()):
        raise ValueError('missing, unknown or invalid ABI fields')
    if any(values.get(key) != expected for key, expected in
           (('schema', 1), ('pointer_bytes', 4), ('size_t_bytes', 4), ('wchar_t_bytes', 2))):
        raise ValueError('probe is not the Vita ARM ABI')
    return values


def arena_bytes(abi, entries, codes):
    if (type(entries) is not int or type(codes) is not int or
            not 0 <= entries <= abi['entries_limit'] or not 0 <= codes <= abi['codes_limit']):
        raise ValueError('list exceeds admitted capacity')
    return sum((count * size + 7) & ~7 for count, size in
               ((max(1, entries), abi['entry_bytes']), (max(1, codes), abi['code_bytes']),
                (max(1, codes), abi['op_bytes']), (max(1, codes), abi['patch_bytes'])))


def thread_requests():
    threads = []
    pattern = re.compile(r'sceKernelCreateThread\(\s*"([^"]+)",\s*\w+,\s*'
                         r'(?:0x[0-9a-fA-F]+|[0-9]+),\s*(0x[0-9a-fA-F]+|[0-9]+),')
    for name, process, lifetime in THREAD_SOURCES:
        source = (ROOT / name).read_text()
        found = pattern.findall(source)
        if not found:
            raise ValueError('unrecognized stack request: ' + name)
        for thread, size in found:
            threads.append({'name': thread, 'process': process, 'lifetime': lifetime,
                            'requested_stack_bytes': int(size, 0), 'source': name})
    return threads


def collect(package, sdk, directory, build_record=None):
    with ZipFile(package) as archive:
        manifest = json.loads(archive.read('manifest.json'))
    if build_record is None:
        with ZipFile(package) as archive:
            record_data = archive.read('build-stamp.json')
        record_label = 'package:build-stamp.json'
    else:
        record_data = build_record.read_bytes()
        record_label = str(build_record)
    record = json.loads(record_data)
    if record.get('configuration', {}).get('resource_profile') is not False:
        raise ValueError('expected an ordinary plugin build')
    for name, expected in record['sources'].items():
        relative = Path(name)
        if relative.is_absolute() or '..' in relative.parts:
            raise ValueError('unsafe source path in build record')
        if sha((ROOT / relative).read_bytes()) != expected:
            raise ValueError('current source differs from packaged build: ' + name)
    directory.mkdir(parents=True, exist_ok=True)
    obj = directory / 'resource-abi.o'
    binary = directory / 'resource-abi.bin'
    subprocess.run([str(sdk / 'bin/arm-vita-eabi-gcc'), '-std=c99', '-Os', '-mthumb',
                    '-fshort-wchar', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'src'),
                    '-I', str(ROOT / 'src/cheats'),
                    '-c', str(ROOT / 'tools/resource-budget/abi.c'), '-o', str(obj)], check=True)
    subprocess.run([str(sdk / 'bin/arm-vita-eabi-objcopy'), '-O', 'binary',
                    '--only-section=.vco.resource.abi', str(obj), str(binary)], check=True)
    abi = decode_abi(binary.read_bytes())
    with ZipFile(package) as archive:
        manifest = json.loads(archive.read('manifest.json'))
        if manifest.get('execution_backend') != 'game_suprx' or not manifest.get('game_plugin_required'):
            raise ValueError('expected the current game-executor package')
        components = {}
        for component, filename in COMPONENTS.items():
            name = 'ux0/vitacheat/' + filename
            data = archive.read(name)
            if ({'bytes': len(data), 'sha256': sha(data)} != manifest['files'][name] or
                    record['artifacts']['build/' + filename] != sha(data)):
                raise ValueError('package payload integrity mismatch: ' + name)
            segments = self_segments(data)
            components[component] = {
                'version': manifest['component_versions'][component], 'file': name,
                'self_sha256': sha(data), 'segments': segments,
                'segment_memory_bytes': sum(segment['memory_bytes'] for segment in segments),
                'page_rounded_segment_model_bytes': sum(segment['page_rounded_model_bytes'] for segment in segments),
            }
    threads = thread_requests()
    shell_stack = sum(t['requested_stack_bytes'] for t in threads if t['process'] == 'shell' and t['lifetime'] == 'resident')
    game_stack = sum(t['requested_stack_bytes'] for t in threads if t['process'] == 'game')
    shell_static = sum(components[c]['page_rounded_segment_model_bytes'] for c in ('kernel', 'loader', 'ui'))
    game_static = components['game']['page_rounded_segment_model_bytes']
    payload = page(abi['upload_max_bytes'])
    arena = page(arena_bytes(abi, abi['entries_limit'], abi['codes_limit']))
    if arena > page(abi['game_allocation_max_bytes']) or abi['game_blocks'] < 3:
        raise ValueError('maximum admitted replacement cannot fit allocator policy')
    sources = [ROOT / name for name, _, _ in THREAD_SOURCES]
    sources += [ROOT / name for name in ('tools/resource-budget/abi.c', 'scripts/resource_budget.py',
        'src/agent/vita_allocator.c', 'src/agent/vita_allocator.h', 'src/agent/game.c',
        'src/agent/executor.c', 'src/agent/vita_memory.h', 'src/execution/limits.h',
        'src/execution/upload.h', 'src/catalog/catalog.h', 'src/cheats/cheats.h')]
    return {
        'schema': 1, 'device_measurement': False,
        'package': {'path': str(package), 'sha256': sha(package.read_bytes())},
        'build_record': {'path': record_label, 'sha256': sha(record_data),
                         'current_production_sources_match': True},
        'abi': abi, 'components': components, 'thread_stack_requests': threads,
        'allocation_policy': {
            'game_payload_block_bytes': payload, 'maximum_list_block_bytes': arena,
            'parse_workspace_temporary_bytes': page(abi['catalog_workspace_bytes']),
            'game_blocks': abi['game_blocks'],
        },
        'accounted_model_bytes': {
            'shell_and_kernel_segments_plus_resident_stacks': shell_static + shell_stack,
            'game_segments_stack_and_payload_before_list': game_static + game_stack + payload,
            'game_with_maximum_current_list': game_static + game_stack + payload + arena,
            'game_during_maximum_list_replacement': game_static + game_stack + payload + 2 * arena,
            'all_with_maximum_current_list': shell_static + shell_stack + game_static + game_stack + payload + arena,
        },
        'sources': {str(path.relative_to(ROOT)): sha(path.read_bytes()) for path in sorted(set(sources))},
        'limitations': [
            'PT_LOAD memory sizes are artifact metadata. Page rounding is a model, not a live allocation measurement.',
            'The model spans kernel, Shell and game processes; their distinct memory quotas cannot be added as one usable pool.',
            'PAF objects, fonts, decoded resources/textures, allocator overhead, system objects and taiHEN patch storage are excluded.',
            'Temporary parse workspace and startup stacks are separate; independent paths may overlap in time.',
            'Failed native frees can retain process-owned blocks; nominal steady state is not a leak or peak proof.',
            'No CPU percentage, game frame time, measured peak RAM or long-run leak conclusion is provided.',
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--sdk', type=Path, default=Path(os.environ['VITASDK']) if 'VITASDK' in os.environ else None)
    parser.add_argument('--directory', type=Path, default=ROOT / 'build/resource-budget')
    parser.add_argument('--build-record', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.sdk is None:
        parser.error('set VITASDK or pass --sdk')
    try:
        report = collect(args.package, args.sdk, args.directory, args.build_record)
    except (OSError, ValueError, KeyError, struct.error, subprocess.CalledProcessError) as error:
        parser.exit(1, f'{error}\n')
    output = args.output or args.directory / 'resource-budget.json'
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(f'Artifact allocation model saved: {output}; not a device measurement.')


if __name__ == '__main__':
    main()
