#!/usr/bin/env python3
"""Reject stale, mismatched, or unreviewed release tags without network access."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('vco_release', Path(__file__).resolve().parents[1] / 'scripts/release.py')
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseTagTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        (self.root / 'src').mkdir()
        (self.root / 'docs/releases').mkdir(parents=True)
        (self.root / 'src/version.h').write_text('#define VCO_VERSION "0.8.50"\n')
        (self.root / 'docs/releases/0.8.50.md').write_text('Reviewed notes\n中文说明\n', encoding='utf-8')
        self.git('init', '-q')
        self.git('config', 'user.name', 'VCO Test')
        self.git('config', 'user.email', 'test@example.invalid')
        self.git('add', '.')
        self.git('commit', '-qm', 'Fixture version')

    def tearDown(self):
        self.directory.cleanup()

    def git(self, *args):
        return subprocess.check_output(['git', *args], cwd=self.root,
                                       stderr=subprocess.DEVNULL, text=True).strip()

    def test_version_tag_resolves_to_exact_checkout(self):
        self.git('tag', 'v0.8.50')
        record = release.check_tag('v0.8.50', self.root)
        self.assertEqual(record, {'tag': 'v0.8.50', 'version': '0.8.50',
                                  'commit': self.git('rev-parse', 'HEAD'), 'prerelease': False})

    def test_annotated_tag_is_peeled_to_commit(self):
        self.git('tag', '-a', 'v0.8.50', '-m', 'Release fixture')
        self.assertEqual(release.check_tag('v0.8.50', self.root)['commit'], self.git('rev-parse', 'HEAD'))

    def test_prerelease_variants_keep_the_base_source_version(self):
        for suffix in ('alpha.1', 'beta.2', 'rc.1'):
            tag = 'v0.8.50-' + suffix
            self.git('tag', tag)
            record = release.check_tag(tag, self.root)
            self.assertTrue(record['prerelease'])
            self.assertEqual(record['version'], '0.8.50')

    def test_missing_tag_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'tag is missing'):
            release.check_tag('v0.8.50', self.root)

    def test_version_mismatch_is_rejected(self):
        self.git('tag', 'v0.8.51')
        with self.assertRaisesRegex(ValueError, 'does not match'):
            release.check_tag('v0.8.51', self.root)

    def test_tag_on_another_commit_is_rejected(self):
        self.git('tag', 'v0.8.50')
        (self.root / 'new.txt').write_text('new revision')
        self.git('add', '.')
        self.git('commit', '-qm', 'Later revision')
        with self.assertRaisesRegex(ValueError, 'Checkout differs'):
            release.check_tag('v0.8.50', self.root)

    def test_missing_or_empty_reviewed_notes_are_rejected(self):
        self.git('tag', 'v0.8.50')
        path = self.root / 'docs/releases/0.8.50.md'
        for contents in ('', ' \n'):
            path.write_text(contents)
            with self.assertRaisesRegex(ValueError, 'reviewed bilingual notes'):
                release.check_tag('v0.8.50', self.root)
        path.unlink()
        with self.assertRaisesRegex(ValueError, 'reviewed bilingual notes'):
            release.check_tag('v0.8.50', self.root)

    def test_invalid_tags_are_rejected_before_git_resolution(self):
        for tag in ('main', '0.8.50', 'v00.8.50', 'v0.8.50-rc.01', 'v0.8.50;echo',
                    '../v0.8.50', 'v0.8.50\n', 'v0.8.50+metadata', 'v0.8.50-preview'):
            with self.subTest(tag=tag):
                with self.assertRaisesRegex(ValueError, 'Expected vMAJOR'):
                    release.check_tag(tag, self.root)


if __name__ == '__main__':
    unittest.main()
