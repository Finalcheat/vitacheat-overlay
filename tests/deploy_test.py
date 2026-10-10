#!/usr/bin/env python3
import importlib.util
import ftplib
import json
from pathlib import Path
import tempfile
import unittest
from zipfile import ZipFile

spec = importlib.util.spec_from_file_location('deploy', Path(__file__).resolve().parents[1] / 'scripts/deploy.py')
deploy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deploy)


class FakeRemote:
    def __init__(self, files):
        self.files = dict(files)
        self.operations = []
        self.fail_at = None
        self.renames = 0

    def read(self, path, missing_ok=False):
        if missing_ok:
            return self.files.get(path)
        return self.files[path]

    def write(self, path, data):
        self.operations.append(('write', path))
        self.files[path] = data

    def rename(self, source, target):
        self.renames += 1
        if self.renames == self.fail_at:
            raise IOError('injected interrupted rename')
        self.operations.append(('rename', source, target))
        if target in self.files:
            raise IOError('target exists')
        self.files[target] = self.files.pop(source)

    def delete(self, path):
        self.operations.append(('delete', path))
        del self.files[path]


class DeployTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.package = self.root / 'execution.zip'
        self.backup = self.root / 'backup'
        self.config = b'# untouched\r\n*KERNEL\r\nux0:vitacheat/vitacheat.skprx\r\nur0:tai/other.skprx\r\n*ALL\r\nux0:vitacheat/vitacheat.suprx\r\n*main\r\nur0:tai/other.suprx\r\n'
        files = {f'ux0/vitacheat/{name}': b'SCE\0' + name.encode() for name in deploy.PLUGINS}
        files['ux0/vitacheat/vco-base.rco'] = b'RCO payload'
        manifest = {'native_execution_connected': True, 'storage_root': 'ux0:vitacheat',
                    'files': {path: {'bytes': len(data), 'sha256': deploy.digest(data)} for path, data in files.items()}}
        with ZipFile(self.package, 'w') as archive:
            for path, data in files.items():
                archive.writestr(path, data)
            archive.writestr('manifest.json', json.dumps(manifest))
        self.original = {'ur0:tai/config.txt': self.config,
                         'ux0:vitacheat/VitaCheatOverlayKernel.skprx': b'old kernel',
                         'ux0:vitacheat/db/PCSE00001.psv': b'_V1 Original\n'}
        self.remote = FakeRemote(self.original)

    def tearDown(self):
        self.directory.cleanup()

    def game_package(self, declared=True, present=True):
        with ZipFile(self.package) as archive:
            payloads = {name: archive.read(name) for name in archive.namelist()}
        manifest = json.loads(payloads.pop('manifest.json'))
        manifest['game_plugin_required'] = declared
        path = 'ux0/vitacheat/' + deploy.GAME_PLUGIN
        if present:
            payloads[path] = b'SCE\0game executor'
            manifest['files'][path] = {'bytes': len(payloads[path]),
                                       'sha256': deploy.digest(payloads[path])}
        with ZipFile(self.package, 'w') as archive:
            for name, data in payloads.items():
                archive.writestr(name, data)
            archive.writestr('manifest.json', json.dumps(manifest))

    def test_game_backend_install_and_rollback(self):
        self.game_package()
        deploy.install(self.remote, self.package, 'ur0:tai/config.txt', self.backup)
        self.assertEqual(self.remote.files['ux0:vitacheat/' + deploy.GAME_PLUGIN], b'SCE\0game executor')
        self.assertNotIn(deploy.GAME_PLUGIN.encode(), self.remote.files['ur0:tai/config.txt'])
        self.assertEqual(self.remote.files['ux0:vitacheat/db/PCSE00001.psv'], b'_V1 Original\n')
        deploy.rollback(self.remote, self.backup)
        for path, data in self.original.items():
            self.assertEqual(self.remote.files[path], data)
        self.assertNotIn('ux0:vitacheat/' + deploy.GAME_PLUGIN, self.remote.files)

    def test_missing_game_executor_is_rejected_before_device_writes(self):
        self.game_package(present=False)
        with self.assertRaises(KeyError):
            deploy.install(self.remote, self.package, 'ur0:tai/config.txt', self.backup)
        self.assertFalse(self.remote.operations)

    def test_undeclared_game_executor_is_rejected(self):
        self.game_package(declared=False)
        with self.assertRaisesRegex(ValueError, 'Undeclared game plugin'):
            deploy.package_files(self.package)

    def test_game_executor_is_loaded_only_by_kernel(self):
        raw = self.config + b'*ALL\r\nux0:vitacheat/VitaCheatOverlayGame.suprx\r\n'
        updated = deploy.update_config(raw)
        self.assertNotIn(deploy.GAME_PLUGIN.encode(), updated)
        self.assertIn(b'ux0:vitacheat/vitacheat.suprx', updated)

    def test_config_preserves_other_plugins_and_is_idempotent(self):
        result = deploy.update_config(self.config)
        self.assertEqual(result, deploy.update_config(result))
        self.assertIn(b'ur0:tai/other.skprx\r\n', result)
        self.assertIn(b'ux0:vitacheat/vitacheat.skprx\r\n', result)
        self.assertIn(b'ux0:vitacheat/vitacheat.suprx\r\n', result)
        self.assertNotIn(b'# VCO disabled:', result)
        self.assertIn(b'*main\r\nux0:vitacheat/VitaCheatOverlayShell.suprx\r\n', result)
        self.assertNotIn(b'VitaCheatOverlayUI.suprx', result)
        duplicates = b'*KERNEL\nux0:vitacheat/VitaCheatOverlayKernel.skprx\n*KERNEL\nux0:tai/VitaCheatOverlayKernel.skprx\n*ALL\nux0:vitacheat/VitaCheatOverlayUI.suprx\n'
        result = deploy.update_config(duplicates)
        self.assertEqual(result.count(b'VitaCheatOverlayKernel.skprx'), 1)
        self.assertNotIn(b'VitaCheatOverlayUI.suprx', result)

    def test_only_vco_disabled_legacy_lines_are_restored(self):
        config = b'*KERNEL\n# VCO disabled: ux0:vitacheat/vitacheat.skprx\n*ALL\n# VCO disabled: ux0:vitacheat/vitacheat.suprx\n# ux0:vitacheat/vitacheat.suprx\n# VCO disabled: ur0:tai/other.skprx\n'
        result = deploy.update_config(config)
        self.assertIn(b'\nux0:vitacheat/vitacheat.skprx\n', result)
        self.assertIn(b'\nux0:vitacheat/vitacheat.suprx\n', result)
        self.assertIn(b'# ux0:vitacheat/vitacheat.suprx\n', result)
        self.assertIn(b'# VCO disabled: ur0:tai/other.skprx\n', result)
        self.assertEqual(result, deploy.update_config(result))

    def test_install_and_rollback(self):
        journal = deploy.install(self.remote, self.package, 'ur0:tai/config.txt', self.backup)
        self.assertEqual(journal['phase'], 'installed_reboot_required')
        commits = [op for op in self.remote.operations if op[0] == 'rename' and '.vco-stage-' in op[1]]
        self.assertEqual(commits[-1][2], 'ur0:tai/config.txt')
        self.assertEqual(self.remote.files['ux0:vitacheat/db/PCSE00001.psv'], b'_V1 Original\n')
        result = deploy.rollback(self.remote, self.backup)
        self.assertEqual(result['phase'], 'rolled_back_reboot_required')
        for path, value in self.original.items():
            self.assertEqual(self.remote.files[path], value)
        self.assertNotIn('ux0:vitacheat/VitaCheatOverlayUI.suprx', self.remote.files)
        deploy.rollback(self.remote, self.backup)  # Retry remains harmless.

    def test_every_commit_interruption_can_roll_back(self):
        for failure in range(1, 8):
            remote = FakeRemote(self.original)
            remote.fail_at = failure
            backup = self.root / f'interruption-{failure}'
            try:
                deploy.install(remote, self.package, 'ur0:tai/config.txt', backup)
            except IOError:
                pass
            remote.fail_at = None
            deploy.rollback(remote, backup)
            for path, value in self.original.items():
                self.assertEqual(remote.files[path], value)
            self.assertNotIn('ux0:vitacheat/VitaCheatOverlayUI.suprx', remote.files)

    def test_rollback_refuses_newer_changes_without_writes(self):
        deploy.install(self.remote, self.package, 'ur0:tai/config.txt', self.backup)
        self.remote.files['ur0:tai/config.txt'] += b'# user change\n'
        before = len(self.remote.operations)
        with self.assertRaises(IOError):
            deploy.rollback(self.remote, self.backup)
        self.assertEqual(len(self.remote.operations), before)

    def test_corrupt_backup_and_package_rejected(self):
        deploy.install(self.remote, self.package, 'ur0:tai/config.txt', self.backup)
        journal = json.loads((self.backup / 'journal.json').read_text())
        original = journal['files']['ur0:tai/config.txt']['original']
        (self.backup / original).write_bytes(b'corrupt')
        before = len(self.remote.operations)
        with self.assertRaises(ValueError):
            deploy.rollback(self.remote, self.backup)
        self.assertEqual(len(self.remote.operations), before)
        with ZipFile(self.package, 'a') as archive:
            archive.writestr('ux0/vitacheat/VitaCheatOverlayKernel.skprx', b'corrupt')
        with self.assertRaises(ValueError):
            deploy.package_files(self.package)

    def test_rollback_does_not_overwrite_edits_during_staging(self):
        deploy.install(self.remote, self.package, 'ur0:tai/config.txt', self.backup)
        write = self.remote.write
        updated = self.remote.files['ur0:tai/config.txt'] + b'# concurrent user edit\n'

        def concurrent_write(path, data):
            write(path, data)
            if path.startswith('ur0:tai/config.txt.vco-rollback-'):
                self.remote.files['ur0:tai/config.txt'] = updated

        self.remote.write = concurrent_write
        before = self.remote.renames
        with self.assertRaisesRegex(IOError, 'changed during rollback'):
            deploy.rollback(self.remote, self.backup)
        self.assertEqual(self.remote.files['ur0:tai/config.txt'], updated)
        self.assertEqual(self.remote.renames, before)

    def test_volume_paths(self):
        self.assertEqual(deploy.Remote.path('ur0:tai/config.txt'), '/ur0:/tai/config.txt')
        for path in ('ur0:../config.txt', 'ur0:tai/x\nDELE /ux0:', 'ur0:/tai/../config.txt', 'http://x'):
            with self.assertRaises(ValueError):
                deploy.Remote.path(path)


class ListingFTP:
    def __init__(self):
        self.directory = '/'
        self.directories = {
            '/': ['drwxr-xr-x 1 vita vita 32768 Oct 5 06:17 ur0:'],
            '/ur0:': ['drwxr-xr-x 1 vita vita 32768 Oct 5 06:17 tai'],
            '/ur0:/tai': ['-rw-r--r-- 1 vita vita 440 Oct 5 06:17 config.txt',
                         '-rw-r--r-- 1 vita vita 12 Oct 5 06:17 file with spaces.txt'],
        }
        self.nlst_error = '502 Sorry, command not implemented.'
        self.list_error = None
        self.calls = []

    def nlst(self, path):
        self.calls.append(('NLST', path))
        if self.nlst_error:
            raise ftplib.error_perm(self.nlst_error)
        return ['/ur0:/tai/config.txt']

    def pwd(self):
        return self.directory

    def cwd(self, path):
        self.calls.append(('CWD', path))
        if path not in self.directories:
            raise ftplib.error_perm('550 Directory not found or denied')
        self.directory = path

    def retrlines(self, command, callback):
        self.calls.append((command, self.directory))
        if self.list_error:
            raise ftplib.error_perm(self.list_error)
        for line in self.directories[self.directory]:
            callback(line)

    def retrbinary(self, command, callback):
        self.calls.append((command, self.directory))
        raise ftplib.error_perm('550 File not found or denied')


class ListingTests(unittest.TestCase):
    def setUp(self):
        self.ftp = ListingFTP()
        self.remote = deploy.Remote(self.ftp)

    def test_vitashell_list_fallback_proves_absence_and_preserves_directory(self):
        self.assertIsNone(self.remote.read('ur0:tai/missing.skprx', True))
        self.assertEqual(self.ftp.directory, '/')
        self.assertIn(('LIST', '/ur0:/tai'), self.ftp.calls)
        self.assertTrue(self.remote._exists('/ur0:/tai/file with spaces.txt'))
        self.assertIsNone(self.remote.read('ur0:missing/nested/file', True))
        self.assertEqual(self.ftp.directory, '/')

    def test_550_for_present_file_or_directory_remains_an_error(self):
        with self.assertRaises(ftplib.error_perm):
            self.remote.read('ur0:tai/config.txt', True)
        self.ftp.directories['/ur0:'] += ['drwxr-xr-x 1 vita vita 32768 Oct 5 06:17 denied']
        with self.assertRaises(ftplib.error_perm):
            self.remote.read('ur0:denied/file', True)
        self.assertEqual(self.ftp.directory, '/')

    def test_listing_failure_or_unknown_format_does_not_prove_absence(self):
        self.ftp.list_error = '550 Access denied'
        with self.assertRaises(ftplib.error_perm):
            self.remote.read('ur0:tai/missing', True)
        self.assertEqual(self.ftp.directory, '/')
        self.ftp.list_error = None
        self.ftp.directories['/ur0:/tai'] = ['unknown listing format']
        with self.assertRaisesRegex(IOError, 'cannot prove absence'):
            self.remote.read('ur0:tai/missing', True)
        self.assertEqual(self.ftp.directory, '/')

    def test_supported_nlst_and_permission_errors_keep_the_original_policy(self):
        self.ftp.nlst_error = None
        self.assertTrue(self.remote._exists('/ur0:/tai/config.txt'))
        self.assertFalse(any(command == 'LIST' for command, path in self.ftp.calls))
        self.ftp.nlst_error = '550 Access denied'
        with self.assertRaises(ftplib.error_perm):
            self.remote.read('ur0:tai/missing', True)
        self.assertFalse(any(command == 'LIST' for command, path in self.ftp.calls))

    def test_empty_directory_and_total_line_are_valid_listings(self):
        self.ftp.directories['/ur0:/tai'] = []
        self.assertIsNone(self.remote.read('ur0:tai/missing', True))
        self.ftp.directories['/ur0:/tai'] = ['total 0']
        self.assertIsNone(self.remote.read('ur0:tai/missing', True))

    def test_vitashell_delete_226_requires_verified_absence(self):
        def delete(path):
            self.ftp.directories['/ur0:/tai'] = []
            raise ftplib.error_reply('226 File deleted.')

        self.ftp.delete = delete
        self.remote.delete('ur0:tai/config.txt')
        self.assertEqual(self.ftp.directory, '/')

    def test_delete_success_reply_cannot_hide_a_remaining_file(self):
        self.ftp.delete = lambda path: '250 File deleted.'
        self.remote.read = lambda path, missing_ok=False: b'remains'
        with self.assertRaisesRegex(IOError, 'remains after deletion'):
            self.remote.delete('ur0:tai/config.txt')
        self.ftp.delete = lambda path: (_ for _ in ()).throw(ftplib.error_reply('226 File deleted.'))
        with self.assertRaisesRegex(IOError, 'remains after deletion'):
            self.remote.delete('ur0:tai/config.txt')

    def test_other_delete_replies_and_unverifiable_absence_remain_errors(self):
        self.ftp.delete = lambda path: (_ for _ in ()).throw(ftplib.error_reply('200 Command OK'))
        with self.assertRaises(ftplib.error_reply):
            self.remote.delete('ur0:tai/config.txt')
        self.ftp.delete = lambda path: (_ for _ in ()).throw(ftplib.error_reply('226 File deleted.'))
        self.ftp.list_error = '550 Access denied'
        with self.assertRaises(ftplib.error_perm):
            self.remote.delete('ur0:tai/config.txt')


if __name__ == '__main__':
    unittest.main()
