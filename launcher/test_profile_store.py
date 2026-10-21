"""Synthetic checkpoint-foundation tests. No game, Steam, or owner saves.

Run: python -I launcher/test_profile_store.py
Every fixture and cleanup stays inside qa/profile-test-work/case-*.
These tests verify file-store behavior, not native save/load integration.
"""
from dataclasses import replace
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import stat
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

_module_spec = importlib.util.spec_from_file_location(
    'profile_store', Path(__file__).absolute().with_name('profile_store.py'))
if _module_spec is None or _module_spec.loader is None:
    raise RuntimeError('Cannot load sibling checkpoint foundation')
p = importlib.util.module_from_spec(_module_spec)
sys.modules[_module_spec.name] = p  # dataclasses resolves the defining module here
_module_spec.loader.exec_module(p)


def synthetic_save(image_size=23, fill=7):
    header = bytearray(p.HEADER)
    header[:4] = b'RGMH'
    struct.pack_into('<II', header, 4, 1, p.HEADER)
    struct.pack_into('<I', header, 20, image_size)
    return bytes(header) + b'I' * image_size + b'\0' * 4 + bytes([fill]) * 508


class ProfileStoreTests(unittest.TestCase):
    def setUp(self):
        p.TEST_ROOT.mkdir(parents=True, exist_ok=True)
        temporary = tempfile.TemporaryDirectory(prefix='case-', dir=p.TEST_ROOT)
        self.root = Path(temporary.name).absolute()
        # Verify cleanup's exact recursive target before registering it.
        self.assertEqual(self.root.parent, p.TEST_ROOT)
        self.assertTrue(self.root.name.startswith('case-'))
        self.addCleanup(temporary.cleanup)
        self.store = p.ProfileStore(self.root)
        self.evidence = p.NormalExitEvidence(11, 22, 0, 0, True,
                                            native_save_success_attested=True)

    def seed(self, relative='run-source', image_size=23, fill=7):
        sandbox = self.store.new_sandbox(relative)
        saves = sandbox / p.SAVE_DIR
        saves.mkdir(parents=True)
        save = saves / 'SavedGame1.SFOFSaveGame'
        save.write_bytes(synthetic_save(image_size, fill))
        return save

    def checkpoint(self, image_size=23):
        save = self.seed(image_size=image_size)
        version = self.store.commit('campaign', 'run-source', self.evidence)
        return save, version

    def manifest_path(self, version):
        return self.root / 'profiles' / 'campaign' / version / 'manifest.json'

    def change_manifest(self, version, change):
        path = self.manifest_path(version)
        manifest = json.loads(path.read_bytes())
        change(manifest)
        raw = p._encoded(manifest)
        path.write_bytes(raw)
        latest = path.parent.parent / 'latest.json'
        marker = json.loads(latest.read_bytes())
        marker['manifest_sha256'] = hashlib.sha256(raw).hexdigest()
        latest.write_bytes(p._encoded(marker))

    def assert_latest(self, version):
        manifest, blobs = self.store.load('campaign')
        self.assertEqual(manifest['version'], version)
        self.assertEqual(len(blobs), 1)

    def test_complete_roundtrip_preserves_all_native_bytes(self):
        save, version = self.checkpoint()
        destination = self.store.restore('campaign', 'sessions/peer-one/sandbox')
        restored = destination / p.SAVE_DIR / save.name
        self.assertEqual(restored.read_bytes(), save.read_bytes())
        self.assertEqual(json.loads((destination / '.fof-restored-checkpoint.json').read_bytes())
                         ['version'], version)
        self.assert_latest(version)

    def test_zero_image_and_alternate_sizes_have_exact_lengths(self):
        for image_size in (0, 1, 15211, 81920):
            with self.subTest(image_size=image_size):
                metadata = p.validate_save(synthetic_save(image_size))
                self.assertEqual(metadata['size'], p.HEADER + image_size + 512)
                self.assertEqual(metadata['image_size'], image_size)

    def test_versions_are_new_and_old_version_still_loads(self):
        save, first = self.checkpoint()
        original = save.read_bytes()
        save.write_bytes(synthetic_save(fill=8))
        second = self.store.commit('campaign', 'run-source', self.evidence)
        self.assertNotEqual(first, second)
        _, old = self.store.load('campaign', first)
        self.assertEqual(old[save.name], original)
        self.assert_latest(second)

    def test_restore_and_new_sandbox_never_overwrite(self):
        self.checkpoint()
        self.store.restore('campaign', 'run-restored')
        with self.assertRaises(p.ProfileError):
            self.store.restore('campaign', 'run-restored')
        with self.assertRaises(p.ProfileError):
            self.store.new_sandbox('run-source')

    def test_foreign_store_root_is_rejected_before_writes(self):
        foreign = self.root / 'incoming'
        foreign.mkdir()
        sentinel = foreign / 'sentinel'
        sentinel.write_bytes(b'unchanged')
        with self.assertRaises(p.ProfileError):
            p.ProfileStore(foreign)
        self.assertEqual(sentinel.read_bytes(), b'unchanged')
        self.assertFalse((foreign / 'profiles').exists())

    def test_non_owned_sandbox_paths_are_rejected(self):
        paths = ('incoming/English', 'test/english', '../run-escape', '/run-escape',
                 'C:/run-escape', 'run-a/../run-b', 'run-a\\escape', 'run-a/',
                 'sessions/con/sandbox', 'sessions/id/../sandbox', 'sessions/id',
                 'sessions/id/sandbox/child', '', 'run-A', 'sessions//sandbox')
        for relative in paths:
            with self.subTest(relative=relative), self.assertRaises(p.ProfileError):
                self.store.new_sandbox(relative)

    def test_invalid_profile_identifiers_are_rejected(self):
        self.seed()
        for profile in ('../escape', 'CON', 'con', 'nul', 'com1', 'a/b', 'a:b',
                        '', None, 'A', 'a' * 49):
            with self.subTest(profile=profile), self.assertRaises(p.ProfileError):
                self.store.commit(profile, 'run-source', self.evidence)

    def test_normal_exit_is_not_native_save_completion(self):
        self.seed()
        evidence = replace(self.evidence, native_save_success_attested=False)
        with self.assertRaises(p.ProfileError):
            self.store.commit('campaign', 'run-source', evidence)

    def test_false_or_invalid_exit_evidence_fails_closed(self):
        self.seed()
        mutations = ({'game_pid': 0}, {'game_pid': True}, {'launcher_pid': -1},
                     {'game_pid': 0x100000000}, {'launcher_pid': 0x100000000},
                     {'launcher_pid': 11}, {'game_exitcode': 124},
                     {'launcher_exitcode': 126}, {'game_exitcode': False},
                     {'owned_processes_verified': False}, {'forced_cleanup': True},
                     {'native_save_success_attested': 1})
        for fields in mutations:
            with self.subTest(fields=fields), self.assertRaises(p.ProfileError):
                self.store.commit('campaign', 'run-source', replace(self.evidence, **fields))
        with self.assertRaises(p.ProfileError):
            self.store.commit('campaign', 'run-source', {'game_exitcode': 0})

    def test_missing_ownership_marker_is_rejected(self):
        self.seed()
        (self.root / 'run-source' / p.OWNER).unlink()
        with self.assertRaises(p.ProfileError):
            self.store.commit('campaign', 'run-source', self.evidence)

    def test_bad_ownership_markers_are_rejected(self):
        self.seed()
        path = self.root / 'run-source' / p.OWNER
        original = json.loads(path.read_bytes())
        for mutation in ({'root': 'somewhere-else'}, {'sandbox': 'run-other'},
                         {'nonce': 'not-a-nonce'}, {'format': True}, {'extra': 1}):
            path.write_bytes(p._encoded({**original, **mutation}))
            with self.subTest(mutation=mutation), self.assertRaises(p.ProfileError):
                self.store.commit('campaign', 'run-source', self.evidence)

    def test_duplicate_json_keys_and_nonfinite_numbers_rejected(self):
        for value in (b'{"format":1,"format":1}', b'{"x":NaN}',
                      b'{"x":Infinity}', b'{"x":-Infinity}', b'{"x":1e999}',
                      b'{"x":1.0}', b'[' * 2000 + b'0' + b']' * 2000, b'\xff'):
            with self.subTest(value=value), self.assertRaises(p.ProfileError):
                p._json(value)

    def test_empty_save_directory_is_rejected(self):
        sandbox = self.store.new_sandbox('run-empty')
        (sandbox / p.SAVE_DIR).mkdir(parents=True)
        with self.assertRaises(p.ProfileError):
            self.store.commit('campaign', 'run-empty', self.evidence)

    def test_unknown_source_file_is_rejected(self):
        save = self.seed()
        (save.parent / 'unrecognized.txt').write_text('not a save')
        with self.assertRaises(p.ProfileError):
            self.store.commit('campaign', 'run-source', self.evidence)

    def test_save_named_directory_is_rejected(self):
        save = self.seed()
        (save.parent / 'SavedGame2.SFOFSaveGame').mkdir()
        with self.assertRaises(p.ProfileError):
            self.store.commit('campaign', 'run-source', self.evidence)

    def test_windows_device_names_and_ambiguous_names_rejected(self):
        for name in ('CON.SFOFSaveGame', 'com1.SFOFSaveGame', 'NUL.SFOFSaveGame',
                     '..\\SavedGame1.SFOFSaveGame', 'a:b.SFOFSaveGame',
                     'Save.SFOFSaveGame ', 'Save..SFOFSaveGame'):
            self.assertFalse(p._save_name(name), name)

    def test_save_count_is_bounded_before_reading_blobs(self):
        save = self.seed()
        for i in range(2, p.MAX_FILES + 2):
            (save.parent / f'SavedGame{i}.SFOFSaveGame').write_bytes(synthetic_save())
        with mock.patch.object(p, '_read', wraps=p._read) as reader:
            with self.assertRaises(p.ProfileError):
                self.store.commit('campaign', 'run-source', self.evidence)
            self.assertEqual(reader.call_count, 1)  # ownership marker only

    def test_source_case_collisions_are_rejected(self):
        save = self.seed()
        original_entries = p._entries
        def entries(directory, maximum):
            if directory == save.parent:
                return [save, save.with_name(save.name.lower())]
            return original_entries(directory, maximum)
        with mock.patch.object(p, '_entries', side_effect=entries), self.assertRaises(p.ProfileError):
            self.store.commit('campaign', 'run-source', self.evidence)

    def test_total_checkpoint_bytes_are_bounded(self):
        save = self.seed()
        (save.parent / 'SavedGame2.SFOFSaveGame').write_bytes(save.read_bytes())
        with mock.patch.object(p, 'MAX_TOTAL', len(save.read_bytes()) * 2 - 1):
            with self.assertRaises(p.ProfileError):
                self.store.commit('campaign', 'run-source', self.evidence)

    def test_hardlinked_source_is_rejected(self):
        save = self.seed()
        os.link(save, self.root / 'hardlink')
        with self.assertRaises(p.ProfileError):
            self.store.commit('campaign', 'run-source', self.evidence)

    def test_reparse_flags_on_leaf_or_ancestor_are_rejected(self):
        save = self.seed()
        original = p.Path.lstat
        for unsafe in (save, save.parent.parent):
            def lstat(path, *args, **kwargs):
                info = original(path, *args, **kwargs)
                if path == unsafe:
                    return SimpleNamespace(st_mode=info.st_mode, st_nlink=info.st_nlink,
                                           st_file_attributes=0x400)
                return info
            with self.subTest(unsafe=unsafe.name), mock.patch.object(p.Path, 'lstat', lstat):
                with self.assertRaises(p.ProfileError):
                    self.store.commit('campaign', 'run-source', self.evidence)

    def test_symlink_file_mode_is_rejected(self):
        save = self.seed()
        original = p.Path.lstat
        def lstat(path, *args, **kwargs):
            info = original(path, *args, **kwargs)
            if path == save:
                return SimpleNamespace(st_mode=stat.S_IFLNK | 0o777, st_nlink=1,
                                       st_file_attributes=0)
            return info
        with mock.patch.object(p.Path, 'lstat', lstat), self.assertRaises(p.ProfileError):
            self.store.commit('campaign', 'run-source', self.evidence)

    def test_truncated_containers_rejected(self):
        data = synthetic_save()
        for size in (0, 4, 12, p.HEADER - 1, p.HEADER, len(data) - 1):
            with self.subTest(size=size), self.assertRaises(p.ProfileError):
                p.validate_save(data[:size])

    def test_bad_native_header_and_payload_rejected(self):
        changes = ((0, b'FAKE'), (4, struct.pack('<I', 2)),
                   (8, struct.pack('<I', p.HEADER + 4)),
                   (20, struct.pack('<I', 0xffffffff)),
                   (p.HEADER + 23, struct.pack('<I', 2)))
        for offset, replacement in changes:
            data = bytearray(synthetic_save())
            data[offset:offset + len(replacement)] = replacement
            with self.subTest(offset=offset), self.assertRaises(p.ProfileError):
                p.validate_save(data)
        for data in (synthetic_save() + b'trailer', 'not bytes'):
            with self.assertRaises(p.ProfileError):
                p.validate_save(data)

    def test_image_bound_is_not_just_file_length(self):
        with mock.patch.object(p, 'MAX_IMAGE', 22), self.assertRaises(p.ProfileError):
            p.validate_save(synthetic_save(23))

    def test_saved_data_corruption_rejected_before_restore_creation(self):
        _, version = self.checkpoint()
        save = self.manifest_path(version).parent / 'saves' / 'SavedGame1.SFOFSaveGame'
        save.write_bytes(synthetic_save(fill=8))
        with self.assertRaises(p.ProfileError):
            self.store.restore('campaign', 'run-refused')
        self.assertFalse((self.root / 'run-refused').exists())

    def test_latest_manifest_digest_mismatch_rejected(self):
        _, version = self.checkpoint()
        path = self.manifest_path(version)
        path.write_bytes(path.read_bytes() + b' ')
        with self.assertRaises(p.ProfileError):
            self.store.load('campaign')

    def test_manifest_wrong_build_profile_evidence_or_unknown_fields(self):
        _, version = self.checkpoint()
        original = json.loads(self.manifest_path(version).read_bytes())
        mutations = ({'build_sha256': 'f' * 64}, {'profile': 'other'}, {'format': True},
                     {'completion_evidence_kind': 'shape_only'}, {'extra': 'unknown'},
                     {'normal_exit': {'game_exitcode': 0}})
        for mutation in mutations:
            self.change_manifest(version, lambda m: (m.clear(), m.update(original), m.update(mutation)))
            with self.subTest(mutation=mutation), self.assertRaises(p.ProfileError):
                self.store.load('campaign')

    def test_manifest_duplicate_case_names_rejected(self):
        _, version = self.checkpoint()
        def change(m):
            m['files'].append({**m['files'][0], 'name': m['files'][0]['name'].lower()})
        self.change_manifest(version, change)
        with self.assertRaises(p.ProfileError):
            self.store.load('campaign')

    def test_manifest_boolean_cannot_impersonate_numeric_zero(self):
        _, version = self.checkpoint(image_size=0)
        self.change_manifest(version, lambda m: m['files'][0].update(image_size=False))
        with self.assertRaises(p.ProfileError):
            self.store.load('campaign')

    def test_manifest_file_metadata_or_name_mismatch_rejected(self):
        _, version = self.checkpoint()
        original = json.loads(self.manifest_path(version).read_bytes())
        for mutation in ({'sha256': '0' * 64}, {'size': 1}, {'extra': 1},
                         {'name': '../SavedGame1.SFOFSaveGame'}):
            def change(m):
                m.clear()
                m.update(json.loads(json.dumps(original)))
                m['files'][0].update(mutation)
            self.change_manifest(version, change)
            with self.subTest(mutation=mutation), self.assertRaises(p.ProfileError):
                self.store.load('campaign')

    def test_unknown_immutable_files_rejected(self):
        _, version = self.checkpoint()
        directory = self.manifest_path(version).parent
        for parent in (directory, directory / 'saves'):
            extra = parent / 'unrecognized'
            extra.write_bytes(b'not trusted')
            with self.subTest(parent=parent.name), self.assertRaises(p.ProfileError):
                self.store.load('campaign')
            extra.unlink()

    def test_bad_latest_markers_rejected(self):
        _, version = self.checkpoint()
        path = self.manifest_path(version).parent.parent / 'latest.json'
        original = json.loads(path.read_bytes())
        for mutation in ({'format': True}, {'version': '../escape'},
                         {'manifest_sha256': 'bad'}, {'extra': 1}):
            path.write_bytes(p._encoded({**original, **mutation}))
            with self.subTest(mutation=mutation), self.assertRaises(p.ProfileError):
                self.store.load('campaign')

    def test_explicit_version_cannot_traverse(self):
        self.checkpoint()
        for version in ('../escape', '/', 'v2026', 0):
            with self.subTest(version=version), self.assertRaises(p.ProfileError):
                self.store.load('campaign', version)

    def test_manifest_read_size_is_bounded(self):
        _, version = self.checkpoint()
        self.manifest_path(version).write_bytes(b' ' * (1024 * 1024 + 1))
        with self.assertRaises(p.ProfileError):
            self.store.load('campaign')

    def test_failed_publication_leaves_previous_checkpoint_valid(self):
        save, version = self.checkpoint()
        save.write_bytes(synthetic_save(fill=9))
        for point in ('before_version_rename', 'after_version_rename', 'before_latest_replace'):
            def fault(phase):
                if phase == point:
                    raise OSError('synthetic interrupted publication')
            with self.subTest(point=point), self.assertRaises(OSError):
                self.store.commit('campaign', 'run-source', self.evidence, _fault=fault)
            self.assert_latest(version)

    def test_failed_file_write_leaves_previous_checkpoint_valid(self):
        _, version = self.checkpoint()
        original = p._write_new
        def write(path, data):
            original(path, data)
            if path.parent.name == 'saves':
                raise OSError('synthetic failed write')
        with mock.patch.object(p, '_write_new', write), self.assertRaises(OSError):
            self.store.commit('campaign', 'run-source', self.evidence)
        self.assert_latest(version)

    def test_readback_rejects_corruption_before_latest_replace(self):
        _, version = self.checkpoint()
        folder = self.root / 'profiles' / 'campaign'
        before = {d.name for d in folder.iterdir()}
        def fault(phase):
            if phase == 'after_version_rename':
                new = [d for d in folder.iterdir() if d.name not in before and d.name.startswith('v')]
                self.assertEqual(len(new), 1)
                (new[0] / 'saves' / 'SavedGame1.SFOFSaveGame').write_bytes(synthetic_save(fill=19))
        with self.assertRaises(p.ProfileError):
            self.store.commit('campaign', 'run-source', self.evidence, _fault=fault)
        self.assert_latest(version)

    def test_corrupt_latest_is_never_silently_repaired_by_commit(self):
        _, version = self.checkpoint()
        latest = self.manifest_path(version).parent.parent / 'latest.json'
        latest.write_bytes(b'incomplete json')
        with self.assertRaises(p.ProfileError):
            self.store.commit('campaign', 'run-source', self.evidence)
        self.assertEqual(latest.read_bytes(), b'incomplete json')


if __name__ == '__main__':
    unittest.main(verbosity=2)
