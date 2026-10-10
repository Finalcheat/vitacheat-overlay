#!/usr/bin/env python3
"""Regression: a stale harness must block packaging even when plugins are unchanged."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('build_stamp', Path(__file__).resolve().parents[1] / 'scripts/build_stamp.py')
stamp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stamp)


class ProvenanceTests(unittest.TestCase):
    def test_generated_encoding_table_invalidates_plugins(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'src/catalog').mkdir(parents=True)
            (root / 'scripts').mkdir()
            (root / 'vendor/taihen').mkdir(parents=True)
            (root / 'vendor/unicode').mkdir()
            (root / 'build').mkdir()
            (root / 'resources').mkdir()
            cache = root / 'build/CMakeCache.txt'
            cache.write_text('VCO_RESOURCE_PROFILE:BOOL=OFF\n')
            inputs = [root / name for name in (
                'CMakeLists.txt', 'kernel-exports.yml', 'game-exports.yml', 'ui-exports.yml', 'shell-exports.yml',
                'scripts/paf_stubs.py', 'vendor/taihen/imports.yml', 'vendor/taihen/taihen.h',
                'scripts/build.sh', 'scripts/build_stamp.py', 'src/catalog/encoding.c',
                'src/catalog/cp936_table.inc', 'scripts/generate_cp936.py', 'vendor/unicode/CP936.TXT',
                'scripts/generate_ui_resources.py', 'scripts/rco.py', 'scripts/ui_assets.py',
                'scripts/check_ui_palette.py', 'scripts/check_game_self.py', 'scripts/check_syscall_abi.py',
                'resources/base.rco', 'resources/overlay.json')]
            inputs += [root / 'build' / name for name in (*stamp.PLUGINS, 'vco-base.rco')]
            for path in inputs:
                path.write_bytes(b'original')
            with patch.object(stamp, 'ROOT', root):
                with patch('sys.argv', ['build_stamp.py', 'record']):
                    stamp.main()
                with patch('sys.argv', ['build_stamp.py', 'check']):
                    stamp.main()
                    cache.write_text('VCO_RESOURCE_PROFILE:BOOL=ON\n')
                    with self.assertRaises(SystemExit):
                        stamp.main()
                    cache.write_text('VCO_RESOURCE_PROFILE:BOOL=OFF\n')
                    stamp.main()
                    table = root / 'src/catalog/cp936_table.inc'
                    table.write_bytes(b'changed decoder mapping')
                    with self.assertRaises(SystemExit):
                        stamp.main()
                    table.write_bytes(b'original')
                    stamp.main()
                    # Resource changes must invalidate the build even if the
                    # executable plugin files are still unchanged. The Game
                    # export contract and payload also invalidate a stale build.
                    for relative in ('resources/base.rco', 'resources/overlay.json',
                                     'game-exports.yml', 'build/VitaCheatOverlayGame.suprx',
                                     'scripts/ui_assets.py', 'scripts/rco.py', 'scripts/check_ui_palette.py',
                                     'scripts/check_game_self.py', 'scripts/check_syscall_abi.py', 'build/vco-base.rco'):
                        path = root / relative
                        path.write_bytes(b'changed resource after build')
                        with self.assertRaises(SystemExit):
                            stamp.main()
                        path.write_bytes(b'original')
                        stamp.main()

    def test_harness_sources_sdk_and_vpk_each_invalidate_stamp(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sources = root / 'tools/device-harness'
            sources.mkdir(parents=True)
            (root / 'scripts').mkdir()
            (root / 'build/harness').mkdir(parents=True)
            sdk = root / 'sdk/common'
            sdk.mkdir(parents=True)
            inputs = [sources / 'main.c', sources / 'codes.c', sources / 'codes.h',
                      sources / 'CMakeLists.txt', sources / 'fixture.c', sources / 'fixture.h',
                      sources / 'fixture_module.c', sources / 'fixture_target.S',
                      sources / 'fixture-a-exports.yml', sources / 'fixture-b-exports.yml',
                      root / 'scripts/build-harness.sh',
                      root / 'scripts/build_stamp.py', root / 'build/harness/VcoDeviceHarness.vpk']
            inputs += [sdk / name for name in ('debugScreen.c', 'debugScreen.h', 'debugScreenFont.c', 'debugScreen_custom.h')]
            for path in inputs:
                path.write_bytes(b'original')
            (root / 'build/harness/CMakeCache.txt').write_text(f'VCO_SDK_COMMON:PATH={sdk}\n')
            with patch.object(stamp, 'ROOT', root):
                with patch('sys.argv', ['build_stamp.py', 'record-harness']):
                    stamp.main()
                for path in inputs:
                    with self.subTest(path=path.name):
                        with patch('sys.argv', ['build_stamp.py', 'check-harness']):
                            stamp.main()
                            path.write_bytes(b'changed after build')
                            with self.assertRaises(SystemExit):
                                stamp.main()
                            path.write_bytes(b'original')
                            stamp.main()


if __name__ == '__main__':
    unittest.main()
