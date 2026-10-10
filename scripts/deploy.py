#!/usr/bin/env python3
"""Verified VitaShell FTP installation/rollback. No device access on import or plan."""
from __future__ import annotations
import argparse
import ftplib
import hashlib
import io
import json
import re
import secrets
from pathlib import Path, PurePosixPath
from zipfile import ZipFile

PLUGINS = ('VitaCheatOverlayKernel.skprx', 'VitaCheatOverlayShell.suprx',
           'VitaCheatOverlayUI.suprx')
GAME_PLUGIN = 'VitaCheatOverlayGame.suprx'
ALL_PLUGINS = PLUGINS + (GAME_PLUGIN,)
LEGACY = {'vitacheat.skprx', 'vitacheat.suprx'}
STORAGE_ROOT = 'ux0:vitacheat'
RESOURCE_PATH = STORAGE_ROOT + '/vco-base.rco'
CONFIG_PATHS = ('ur0:tai/config.txt', 'ux0:tai/config.txt')


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def update_config(raw: bytes) -> bytes:
    """Keep VitaCheat loaded and replace only recognized VCO load paths."""
    text = raw.decode('utf-8-sig')
    newline = '\r\n' if '\r\n' in text else '\n'
    lines = text.splitlines()
    result: list[str] = []
    wanted = {'*KERNEL': STORAGE_ROOT + '/VitaCheatOverlayKernel.skprx',
              '*main': STORAGE_ROOT + '/VitaCheatOverlayShell.suprx'}
    seen = set()
    for line in lines:
        clean = line.strip()
        # Undo only our earlier automatic disable marker. User comments and
        # unrelated disabled plugins remain byte-for-byte as supplied.
        marker = '# VCO disabled: '
        if clean.startswith(marker):
            original = clean[len(marker):]
            filename = original.split('#', 1)[0].strip().rsplit('/', 1)[-1].lower()
            if filename in LEGACY:
                line = original
                clean = line.strip()
        if clean in wanted and clean not in seen:
            result.extend((line, wanted[clean]))
            seen.add(clean)
            continue
        if clean and not clean.startswith(('#', '*')):
            path = clean.split('#', 1)[0].strip()
            filename = path.rsplit('/', 1)[-1].lower()
            if filename in {name.lower() for name in ALL_PLUGINS}:
                continue
        result.append(line)
    for section, path in wanted.items():
        if section not in seen:
            result.extend(('', section, path))
    output = newline.join(result) + newline
    return (b'\xef\xbb\xbf' if raw.startswith(b'\xef\xbb\xbf') else b'') + output.encode('utf-8')


def package_files(path: Path) -> dict[str, bytes]:
    with ZipFile(path) as archive:
        if len(archive.namelist()) != len(set(archive.namelist())):
            raise ValueError('Duplicate package paths')
        manifest = json.loads(archive.read('manifest.json'))
        if not manifest.get('native_execution_connected'):
            raise ValueError('Package is not an execution build')
        game_required = manifest.get('game_plugin_required', False)
        if not isinstance(game_required, bool):
            raise ValueError('Invalid game plugin requirement')
        plugins = ALL_PLUGINS if game_required else PLUGINS
        # Old releases may be read for migration preflight or rolled back with
        # their journal; installation below accepts only the shared layout.
        if manifest.get('storage_root') == STORAGE_ROOT:
            expected = {f'ux0/vitacheat/{name}' for name in plugins}
            expected.add('ux0/vitacheat/vco-base.rco')
        elif 'storage_root' not in manifest:
            if game_required:
                raise ValueError('Game backend requires shared VitaCheat directory')
            expected = {f'ur0/tai/{name}' for name in plugins}
            expected.add('ur0/data/VitaCheatOverlay/base.rco')
        else:
            raise ValueError('Unsupported package storage root')
        if not game_required and 'ux0/vitacheat/' + GAME_PLUGIN in archive.namelist():
            raise ValueError('Undeclared game plugin')
        files = {}
        for name in sorted(expected):
            data = archive.read(name)
            record = manifest['files'][name]
            if len(data) != record['bytes'] or digest(data) != record['sha256']:
                raise ValueError(f'Package integrity mismatch: {name}')
            if name.endswith(('.skprx', '.suprx')) and not data.startswith(b'SCE\0'):
                raise ValueError(f'Not a Vita SELF: {name}')
            volume, relative = name.split('/', 1)
            files[volume + ':' + relative] = data
        return files


class Remote:
    def __init__(self, ftp: ftplib.FTP):
        self.ftp = ftp

    @staticmethod
    def path(path: str) -> str:
        # VitaShell exports volumes as directories: /ur0:/tai/...
        if '\r' in path or '\n' in path or not re.fullmatch(r'(ur0|ux0):[^:]+', path):
            raise ValueError('Invalid Vita path')
        volume, rest = path.split(':', 1)
        if any(part in ('..', '.') for part in rest.split('/')):
            raise ValueError('Invalid Vita path components')
        return f'/{volume}:/{rest.lstrip("/")}'

    def read(self, path: str, missing_ok: bool = False) -> bytes | None:
        data = io.BytesIO()
        try:
            self.ftp.retrbinary('RETR ' + self.path(path), data.write)
        except ftplib.error_perm as exc:
            # Prove absence with directory listing. An arbitrary 550 can mean
            # denied access and must never be mistaken for a missing backup.
            if not missing_ok or not str(exc).startswith('550'):
                raise
            if self._exists(self.path(path)):
                raise
            return None
        return data.getvalue()

    def _exists(self, path: str) -> bool:
        parent, name = path.rstrip('/').rsplit('/', 1)
        parent = parent or '/'
        try:
            entries = self._names(parent)
        except ftplib.error_perm as exc:
            if parent == '/' or not str(exc).startswith('550') or self._exists(parent):
                raise
            return False
        return any(item.rstrip('/').rsplit('/', 1)[-1] == name for item in entries)

    def _names(self, parent: str) -> list[str]:
        try:
            return self.ftp.nlst(parent)
        except ftplib.error_perm as exc:
            if str(exc)[:3] not in {'500', '502', '504'}:
                raise
        # VitaShell implements LIST but not NLST. Change directory explicitly:
        # its LIST implementation need not accept a path argument. A listing
        # failure or unrecognized line is not evidence that a file is absent.
        previous = self.ftp.pwd()
        lines: list[str] = []
        try:
            self.ftp.cwd(parent)
            self.ftp.retrlines('LIST', lines.append)
        finally:
            self.ftp.cwd(previous)
        names = []
        for line in lines:
            if re.fullmatch(r'total [0-9]+', line):
                continue
            fields = line.split(None, 8)
            if (len(fields) != 9 or not re.fullmatch(r'[-d][rwxstST-]{9}', fields[0])
                    or not fields[4].isdigit()):
                raise IOError('Unrecognized FTP directory listing; cannot prove absence')
            names.append(fields[8])
        return names

    def directories(self, path: str) -> None:
        parent = PurePosixPath(self.path(path)).parent
        current = ''
        for part in parent.parts:
            if part == '/':
                continue
            current += '/' + part
            try:
                self.ftp.cwd(current)
            except ftplib.error_perm:
                self.ftp.mkd(current)

    def write(self, path: str, data: bytes) -> None:
        self.directories(path)
        self.ftp.storbinary('STOR ' + self.path(path), io.BytesIO(data))
        if self.read(path) != data:
            raise IOError(f'Remote verification failed: {path}')

    def rename(self, source: str, target: str) -> None:
        self.ftp.rename(self.path(source), self.path(target))

    def delete(self, path: str) -> None:
        try:
            self.ftp.delete(self.path(path))
        except ftplib.error_reply as exc:
            # VitaShell reports successful DELE with 226 rather than the 250
            # expected by ftplib. Accept that reply only after proving absence.
            if not str(exc).startswith('226'):
                raise
        if self.read(path, True) is not None:
            raise IOError(f'Remote file remains after deletion: {path}')


def save_journal(directory: Path, journal: dict) -> None:
    temporary = directory / 'journal.json.tmp'
    temporary.write_text(json.dumps(journal, ensure_ascii=False, indent=2) + '\n')
    temporary.replace(directory / 'journal.json')


def install(remote: Remote, package: Path, config_path: str, backup: Path) -> dict:
    if config_path not in CONFIG_PATHS:
        raise ValueError('Select the active tai config path explicitly')
    files = package_files(package)
    layouts = [{STORAGE_ROOT + '/' + name for name in names} | {RESOURCE_PATH}
               for names in (PLUGINS, ALL_PLUGINS)]
    if set(files) not in layouts:
        raise ValueError('This installer requires a shared VitaCheat-directory package')
    original_config = remote.read(config_path)
    assert original_config is not None
    files[config_path] = update_config(original_config)
    backup.mkdir(parents=True, exist_ok=False)
    run = secrets.token_hex(8)
    journal = {'format': 1, 'run': run, 'phase': 'backing_up', 'config_path': config_path, 'files': {}}
    # Read and save every original before changing any installed path.
    for index, (path, data) in enumerate(files.items()):
        original = original_config if path == config_path else remote.read(path, True)
        local = f'{index:02d}.original'
        if original is not None:
            (backup / local).write_bytes(original)
        journal['files'][path] = {'original': local if original is not None else None,
                                  'original_sha256': digest(original) if original is not None else None,
                                  'installed_sha256': digest(data), 'state': 'saved',
                                  'stage': path + '.vco-stage-' + run,
                                  'remote_backup': path + '.vco-backup-' + run}
    save_journal(backup, journal)
    # Stage the complete set first. Config is deliberately committed last.
    for path, data in files.items():
        record = journal['files'][path]
        remote.write(record['stage'], data)
        record['state'] = 'staged'
        save_journal(backup, journal)
    journal['phase'] = 'committing'
    save_journal(backup, journal)
    for path in files:
        record = journal['files'][path]
        # Refuse to overwrite changes made after the backup.
        current = remote.read(path, True)
        if (digest(current) if current is not None else None) != record['original_sha256']:
            raise IOError(f'Remote changed during install: {path}')
        if current is not None:
            remote.rename(path, record['remote_backup'])
            record['state'] = 'original_moved'
            save_journal(backup, journal)
        remote.rename(record['stage'], path)
        record['state'] = 'installed'
        save_journal(backup, journal)
        if digest(remote.read(path)) != record['installed_sha256']:
            raise IOError(f'Installed verification failed: {path}')
    journal['phase'] = 'installed_reboot_required'
    save_journal(backup, journal)
    return journal


def rollback(remote: Remote, backup: Path) -> dict:
    journal = json.loads((backup / 'journal.json').read_text())
    if journal['format'] != 1 or journal['config_path'] not in CONFIG_PATHS or not re.fullmatch(r'[0-9a-f]{16}', journal['run']):
        raise ValueError('Unsupported rollback journal')
    originals = {}
    # All hashes and current paths are checked before the first rollback write.
    for path, record in journal['files'].items():
        if path not in {root + '/' + name for root in (STORAGE_ROOT, 'ur0:tai') for name in ALL_PLUGINS} | {
            RESOURCE_PATH, 'ur0:data/VitaCheatOverlay/base.rco', journal['config_path']}:
            raise ValueError('Unexpected rollback path')
        local = record['original']
        if local and not re.fullmatch(r'\d{2}\.original', local):
            raise ValueError('Invalid backup filename')
        original = (backup / local).read_bytes() if local else None
        if (digest(original) if original is not None else None) != record['original_sha256']:
            raise ValueError(f'Backup integrity mismatch: {path}')
        current = remote.read(path, True)
        current_hash = digest(current) if current is not None else None
        if current_hash not in (record['original_sha256'], record['installed_sha256'], None):
            raise IOError(f'Remote has newer changes: {path}')
        originals[path] = (original, current)
    # Config first during rollback; data files then return to their original set.
    ordered = sorted(originals, key=lambda path: path != journal['config_path'])
    for path in ordered:
        original, current = originals[path]
        if current == original:
            continue
        if original is None:
            if current is not None:
                if remote.read(path, True) != current:
                    raise IOError(f'Remote changed during rollback: {path}')
                remote.delete(path)
        else:
            staged = path + '.vco-rollback-' + journal['run']
            remote.write(staged, original)
            # Staging/read-back may take time. Recheck each installed path at
            # commit too, not just once before staging the entire rollback.
            if remote.read(path, True) != current:
                raise IOError(f'Remote changed during rollback: {path}')
            if current is not None:
                remote.rename(path, path + '.vco-replaced-' + secrets.token_hex(8))
            remote.rename(staged, path)
            if remote.read(path) != original:
                raise IOError(f'Rollback verification failed: {path}')
    journal['phase'] = 'rolled_back_reboot_required'
    save_journal(backup, journal)
    return journal


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    plan = sub.add_parser('plan', help='Offline config diff; no FTP connection')
    plan.add_argument('--config', required=True, type=Path)
    plan.add_argument('--output', required=True, type=Path)
    for action in ('install', 'rollback'):
        command = sub.add_parser(action)
        command.add_argument('--host', required=True)
        command.add_argument('--port', type=int, default=1337)
        command.add_argument('--backup', required=True, type=Path)
        if action == 'install':
            command.add_argument('--package', required=True, type=Path)
            command.add_argument('--config-path', required=True, choices=CONFIG_PATHS)
    args = parser.parse_args()
    if args.action == 'plan':
        args.output.write_bytes(update_config(args.config.read_bytes()))
        print(args.output)
        return
    ftp = ftplib.FTP()
    ftp.connect(args.host, args.port, timeout=15)
    ftp.login()
    try:
        remote = Remote(ftp)
        result = install(remote, args.package, args.config_path, args.backup) if args.action == 'install' else rollback(remote, args.backup)
        print(result['phase'])
        print('Backup:', args.backup)
    finally:
        ftp.close()


if __name__ == '__main__':
    main()
