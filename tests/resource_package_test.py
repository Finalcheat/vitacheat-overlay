#!/usr/bin/env python3
"""Verify fresh package provenance and the installer's offline contract."""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch
from zipfile import ZipFile

SOURCE = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


package = load('vco_package', SOURCE / 'scripts/package.py')
deploy = load('vco_deploy', SOURCE / 'scripts/deploy.py')


class SourcePackageTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.context = patch.object(package, 'ROOT', self.root)
        self.context.start()
        inputs = (
            'CMakeLists.txt', 'kernel-exports.yml', 'game-exports.yml', 'ui-exports.yml', 'shell-exports.yml',
            'scripts/paf_stubs.py', 'vendor/taihen/imports.yml', 'vendor/taihen/taihen.h',
            'scripts/build.sh', 'scripts/build_stamp.py', 'src/catalog/encoding.c',
            'src/catalog/cp936_table.inc', 'scripts/generate_cp936.py', 'vendor/unicode/CP936.TXT',
            'scripts/generate_ui_resources.py', 'scripts/rco.py', 'scripts/ui_assets.py',
            'scripts/check_ui_palette.py', 'scripts/check_game_self.py', 'scripts/check_syscall_abi.py',
            'resources/base.rco', 'resources/overlay.json', 'build/vco-base.rco')
        for name in (*inputs, *package.DOCUMENTS, *package.NOTICES):
            self.write(name, b'fixture input')
        self.write('build/CMakeCache.txt', b'VCO_RESOURCE_PROFILE:BOOL=OFF\n')
        self.write('src/version.h', b'#define VCO_VERSION "0.8.50"\n')
        self.write('src/config/config.h', b'#define VCO_CONFIG_VERSION 6\n')
        self.write('src/agent/protocol.h', b'#define VCO_AGENT_ABI 0x00010004u\n')
        for name in package.PLUGINS:
            self.write('build/' + name, b'SCE\0fixture SELF')
        shutil.copyfile(SOURCE / 'scripts/build_stamp.py', self.root / 'scripts/build_stamp.py')
        self.record()

    def tearDown(self):
        self.context.stop()
        self.directory.cleanup()

    def write(self, name, data):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def record(self):
        stamp = load('fixture_stamp', self.root / 'scripts/build_stamp.py')
        record = stamp.current()
        self.write('build/plugin-build.json', json.dumps(record).encode())

    def test_clean_tree_packages_without_any_historical_archive_or_device_record(self):
        files = package.payloads()
        manifest = json.loads(files['manifest.json'])
        self.assertFalse(manifest['hardware_tested'])
        self.assertEqual(manifest['variant'], 'source-build')
        self.assertEqual(manifest['settings_schema'], 6)
        self.assertTrue(manifest['game_plugin_required'])
        self.assertTrue(manifest['native_execution_connected'])
        for name, record in manifest['files'].items():
            self.assertEqual(record, {'bytes': len(files[name]),
                                     'sha256': hashlib.sha256(files[name]).hexdigest()})
        archive = self.root / 'candidate.zip'
        package.write_archive(archive, files)
        installed = deploy.package_files(archive)
        self.assertEqual(set(installed), {'ux0:vitacheat/' + name
                                         for name in (*package.PLUGINS, 'vco-base.rco')})
        self.assertFalse((self.root / 'dist').exists())
        self.assertFalse((self.root / 'build/validation-0.8.43').exists())

    def test_changed_artifact_blocks_packaging(self):
        self.write('build/VitaCheatOverlayGame.suprx', b'SCE\0changed')
        with self.assertRaisesRegex(ValueError, 'Sources or artifacts changed'):
            package.payloads()

    def test_artifact_changed_between_validation_and_read_is_rejected(self):
        record = package.verified_build()
        def validated_then_changed():
            self.write('build/VitaCheatOverlayGame.suprx', b'SCE\0changed after validation')
            return record
        with patch.object(package, 'verified_build', side_effect=validated_then_changed):
            with self.assertRaisesRegex(ValueError, 'Artifact changed while packaging'):
                package.payloads()

    def test_modified_or_added_source_blocks_packaging(self):
        for name in ('src/catalog/cp936_table.inc', 'src/new.c'):
            with self.subTest(name=name):
                self.write(name, b'changed after build')
                with self.assertRaisesRegex(ValueError, 'Sources or artifacts changed'):
                    package.payloads()
                self.record()

    def test_missing_build_record_cannot_bypass_provenance(self):
        (self.root / 'build/plugin-build.json').unlink()
        with self.assertRaisesRegex(ValueError, 'Sources or artifacts changed'):
            package.payloads()

    def test_missing_input_hash_cannot_bypass_validation(self):
        path = self.root / 'build/plugin-build.json'
        record = json.loads(path.read_text())
        del record['sources']['src/catalog/encoding.c']
        path.write_text(json.dumps(record))
        with self.assertRaisesRegex(ValueError, 'Sources or artifacts changed'):
            package.payloads()

    def test_profile_build_cannot_be_packaged_as_ordinary(self):
        self.write('build/CMakeCache.txt', b'VCO_RESOURCE_PROFILE:BOOL=ON\n')
        with self.assertRaisesRegex(SystemExit, 'Developer resource profile'):
            package.payloads()

    def test_not_self_is_rejected_even_if_stamp_matches(self):
        self.write('build/VitaCheatOverlayGame.suprx', b'ELF, not SELF')
        self.record()
        with self.assertRaisesRegex(ValueError, 'Not a Vita SELF'):
            package.payloads()

    def test_harness_option_requires_its_separate_current_stamp(self):
        with self.assertRaises((OSError, SystemExit, ValueError)):
            package.payloads(include_harness=True)

    def test_archives_are_byte_stable_and_have_no_absolute_paths(self):
        files = package.payloads()
        first, second = self.root / 'first.zip', self.root / 'second.zip'
        package.write_archive(first, files)
        package.write_archive(second, files)
        self.assertEqual(first.read_bytes(), second.read_bytes())
        with ZipFile(first) as archive:
            for name in archive.namelist():
                self.assertFalse(Path(name).is_absolute())
                self.assertNotIn('..', Path(name).parts)
            self.assertNotIn(str(self.root).encode(), archive.read('build-stamp.json'))

    def candidate(self, files=None):
        path = self.root / 'candidate.zip'
        package.write_archive(path, files if files is not None else package.payloads())
        path.with_suffix('.zip.sha256').write_text(
            f'{package.digest(path.read_bytes())}  {path.name}\n')
        return path

    def test_verified_archive_retains_license_and_installation_contract(self):
        manifest = package.verify_archive(self.candidate(), '0.8.50')
        self.assertIn('LICENSE', manifest['files'])
        self.assertEqual(manifest['source'], {'commit': None, 'dirty': None})

    def test_archive_checksum_corruption_is_rejected(self):
        path = self.candidate()
        path.write_bytes(path.read_bytes() + b'changed')
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            package.verify_archive(path)

    def test_manifest_payload_corruption_is_rejected_even_with_valid_archive_checksum(self):
        files = package.payloads()
        files['ux0/vitacheat/VitaCheatOverlayGame.suprx'] = b'SCE\0tampered'
        with self.assertRaisesRegex(ValueError, 'payload mismatch'):
            package.verify_archive(self.candidate(files))

    def test_missing_license_cannot_be_hidden_by_updating_manifest(self):
        files = package.payloads()
        del files['LICENSE']
        manifest = json.loads(files['manifest.json'])
        del manifest['files']['LICENSE']
        files['manifest.json'] = json.dumps(manifest).encode()
        with self.assertRaisesRegex(ValueError, 'Missing installation files or notices'):
            package.verify_archive(self.candidate(files))

    def test_undeclared_extra_file_is_rejected(self):
        files = package.payloads()
        files['extra.txt'] = b'undeclared'
        with self.assertRaisesRegex(ValueError, 'Manifest coverage mismatch'):
            package.verify_archive(self.candidate(files))

    def test_unsafe_archive_path_is_rejected(self):
        files = package.payloads()
        files['../extra.txt'] = b'outside'
        with self.assertRaisesRegex(ValueError, 'Unsafe package path'):
            package.verify_archive(self.candidate(files))

    def test_wrong_version_or_source_commit_is_rejected(self):
        path = self.candidate()
        with self.assertRaisesRegex(ValueError, 'version mismatch'):
            package.verify_archive(path, expected_version='0.8.51')
        with self.assertRaisesRegex(ValueError, 'source commit mismatch'):
            package.verify_archive(path, expected_commit='a' * 40)

    def test_dirty_source_cannot_be_used_for_tagged_release(self):
        files = package.payloads()
        manifest = json.loads(files['manifest.json'])
        manifest['source'] = {'commit': 'a' * 40, 'dirty': True}
        files['manifest.json'] = json.dumps(manifest).encode()
        with self.assertRaisesRegex(ValueError, 'uncommitted changes'):
            package.verify_archive(self.candidate(files), expected_commit='a' * 40)

    def test_git_identity_distinguishes_clean_and_modified_sources(self):
        def git(*args):
            subprocess.run(['git', *args], cwd=self.root, check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        git('init', '-q')
        git('config', 'user.name', 'VCO Test')
        git('config', 'user.email', 'test@example.invalid')
        git('add', '.')
        git('commit', '-qm', 'Test fixture')
        clean = package.source_identity()
        self.assertFalse(clean['dirty'])
        self.assertEqual(len(clean['commit']), 40)
        self.write('LICENSE', b'changed after commit')
        self.assertEqual(package.source_identity(), {'commit': clean['commit'], 'dirty': True})


if __name__ == '__main__':
    unittest.main()
