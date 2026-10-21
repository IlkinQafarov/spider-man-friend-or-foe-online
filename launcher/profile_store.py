"""Unintegrated native-save checkpoint foundation; no game execution.

Normal exit AND native save completion are caller attestations, never inferred
from container shape. Runtime integration must establish both before commit().
This module neither observes native callbacks nor implements campaign resume.
Version immutability is an API guarantee, not protection against a hostile local
writer. Do not use concurrently with an actor replacing directories or links.
Atomic publication covers tested process/write failures; power-loss durability
and live native save/load callbacks remain unverified. Restore failures may
leave an incomplete NEW sandbox; launch only after restore() returns normally.
"""
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
import hashlib
import json
import os
import re
import stat
import struct
import uuid

SAVE_DIR = Path('Documents/Activision/Spider-Man FoF/Saved Games')
OWNER = '.fof-owned-sandbox.json'
BUILD = 'ebd30047d7e58c95bf7dc4bd1fb4a5780e16aa472b61c25a8c421632662083db'
HEADER = 8232
MAX_IMAGE = 16 * 1024 * 1024
MAX_TOTAL = 64 * 1024 * 1024
MAX_FILES = 16
IDENTIFIER = re.compile(r'[a-z0-9][a-z0-9_-]{0,47}\Z')
VERSION = re.compile(r'v[0-9]{8}T[0-9]{12}Z-[a-f0-9]{32}\Z')
SAVE_NAME = re.compile(r'[A-Za-z0-9_-]{1,80}\.SFOFSaveGame\Z', re.I)
RESERVED = {'con', 'prn', 'aux', 'nul'} | {f'{s}{n}' for s in ('com', 'lpt') for n in range(1, 10)}
PROJECT_ROOT = Path(os.path.abspath(__file__)).parents[1]
TEST_ROOT = PROJECT_ROOT / 'qa' / 'profile-test-work'


class ProfileError(ValueError):
    pass


def _regular_chain(path, leaf_directory=False):
    """Reject symlinks/reparse points in every existing absolute component."""
    path = Path(os.path.abspath(path))
    for item in reversed([path, *path.parents]):
        try:
            info = item.lstat()
        except FileNotFoundError:
            raise ProfileError('Missing path component: ' + str(item))
        if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
            raise ProfileError('Links/reparse points are not allowed: ' + str(item))
        directory = item != path or leaf_directory
        if directory and not stat.S_ISDIR(info.st_mode):
            raise ProfileError('Expected regular directory: ' + str(item))
        if not directory and (not stat.S_ISREG(info.st_mode) or info.st_nlink != 1):
            raise ProfileError('Expected singly linked regular file: ' + str(item))
    return path


def _read(path, limit):
    path = _regular_chain(path)
    # On Windows deny concurrent writes/deletion while reading the actual file.
    # Reparse traversal has already been rejected for the complete ancestor chain.
    if os.name == 'nt':
        import ctypes
        from ctypes import wintypes
        import msvcrt
        api = ctypes.WinDLL('kernel32', use_last_error=True)
        api.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                   ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        api.CreateFileW.restype = wintypes.HANDLE
        api.GetFileAttributesW.argtypes = [wintypes.LPCWSTR]
        api.GetFileAttributesW.restype = wintypes.DWORD
        api.CloseHandle.argtypes = [wintypes.HANDLE]
        handle = api.CreateFileW(str(path), 0x80000000, 1, None, 3, 0x00200000, None)
        if handle == ctypes.c_void_p(-1).value:
            raise ProfileError('Cannot exclusively read checkpoint file')
        try:
            _regular_chain(path)
            fd = msvcrt.open_osfhandle(int(handle), os.O_RDONLY | os.O_BINARY)
        except BaseException:
            api.CloseHandle(handle)
            raise
        stream = os.fdopen(fd, 'rb')
    else:
        fd = os.open(path, os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0))
        stream = os.fdopen(fd, 'rb')
    with stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
            raise ProfileError('Checkpoint is not a regular file')
        data = stream.read(limit + 1)
    if len(data) > limit:
        raise ProfileError('Checkpoint size exceeds limit')
    return data


def _json(data):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ProfileError('Duplicate JSON key')
            result[key] = value
        return result
    try:
        def reject_constant(value):
            raise ProfileError('Only integral JSON numbers are supported')
        return json.loads(data.decode('utf-8'), object_pairs_hook=unique,
                          parse_constant=reject_constant, parse_float=reject_constant)
    except (UnicodeError, ValueError, RecursionError) as exc:
        raise ProfileError('Invalid checkpoint JSON') from exc


def _encoded(value):
    return (json.dumps(value, sort_keys=True, indent=2) + '\n').encode('utf-8')


def _write_new(path, data):
    _regular_chain(path.parent, True)
    if path.exists() or path.is_symlink():
        raise ProfileError('Refusing to overwrite: ' + str(path))
    with path.open('xb') as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def _identifier(value):
    if not isinstance(value, str) or not IDENTIFIER.fullmatch(value) or value in RESERVED:
        raise ProfileError('Invalid simple profile identifier')
    return value


def _save_name(value):
    return (isinstance(value, str) and SAVE_NAME.fullmatch(value)
            and value.split('.', 1)[0].lower() not in RESERVED)


def _entries(directory, maximum):
    result = []
    for entry in directory.iterdir():
        result.append(entry)
        if len(result) > maximum:
            raise ProfileError('Directory entry count exceeds bounds')
    return result


def validate_save(data):
    if not isinstance(data, (bytes, bytearray, memoryview)):
        raise ProfileError('Save must be a byte container')
    if len(data) < HEADER + 512 or data[:4] != b'RGMH':
        raise ProfileError('Invalid/truncated RGMH save')
    if struct.unpack_from('<II', data, 4) != (1, HEADER):
        raise ProfileError('Unsupported native container header')
    image_size = struct.unpack_from('<I', data, 20)[0]
    if image_size > MAX_IMAGE or len(data) != HEADER + image_size + 512:
        raise ProfileError('Invalid image size or exact payload length')
    if struct.unpack_from('<I', data, HEADER + image_size)[0] != 0:
        raise ProfileError('Unsupported observed payload discriminator')
    return {'size': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
            'image_size': image_size, 'payload_size': 512, 'payload_discriminator': 0}


@dataclass(frozen=True)
class NormalExitEvidence:
    game_pid: int
    launcher_pid: int
    game_exitcode: int
    launcher_exitcode: int
    owned_processes_verified: bool
    forced_cleanup: bool = False
    native_save_success_attested: bool = False

    def validate(self):
        if (type(self.game_pid) is not int or not 0 < self.game_pid <= 0xffffffff
                or type(self.launcher_pid) is not int or not 0 < self.launcher_pid <= 0xffffffff
                or self.game_pid == self.launcher_pid
                or type(self.game_exitcode) is not int or self.game_exitcode != 0
                or type(self.launcher_exitcode) is not int or self.launcher_exitcode != 0
                or self.owned_processes_verified is not True or self.forced_cleanup is not False
                or self.native_save_success_attested is not True):
            raise ProfileError('Owned normal exits0 and native save completion must be attested')


class ProfileStore:
    def __init__(self, root):
        self.root = _regular_chain(root, True)
        if (self.root != PROJECT_ROOT and not
                (self.root.parent == TEST_ROOT and self.root.name.startswith('case-')
                 and IDENTIFIER.fullmatch(self.root.name))):
            raise ProfileError('Store root must be this checkout or its dedicated synthetic test case')
        self.store = self.root / 'profiles'
        if not self.store.exists():
            self.store.mkdir()
        _regular_chain(self.store, True)

    def _sandbox_path(self, relative):
        if not isinstance(relative, str) or '\\' in relative or ':' in relative:
            raise ProfileError('Sandbox must be a project-relative slash path')
        pieces = relative.split('/')
        if any(not p or p in ('.', '..') for p in pieces):
            raise ProfileError('Invalid sandbox path')
        valid_run = len(pieces) == 1 and pieces[0].startswith('run-') and IDENTIFIER.fullmatch(pieces[0])
        valid_session = (len(pieces) == 3 and pieces[0] == 'sessions'
                         and IDENTIFIER.fullmatch(pieces[1]) and pieces[1] not in RESERVED
                         and pieces[2] == 'sandbox')
        if not (valid_run or valid_session):
            raise ProfileError('Only mod-owned run-* or sessions/<id>/sandbox paths are allowed')
        return self.root.joinpath(*pieces)

    def new_sandbox(self, relative):
        target = self._sandbox_path(relative)
        # Create missing parent directories individually; validate existing ones.
        for item in reversed([target.parent, *target.parent.parents]):
            if item == self.root or self.root in item.parents:
                if not item.exists():
                    _regular_chain(item.parent, True)
                    item.mkdir()
                _regular_chain(item, True)
        if target.exists() or target.is_symlink():
            raise ProfileError('Sandbox must be NEW')
        _regular_chain(target.parent, True)
        target.mkdir()
        _write_new(target / OWNER, _encoded({'format': 1, 'root': str(self.root),
                    'sandbox': relative, 'nonce': uuid.uuid4().hex}))
        return target

    def _owned_sandbox(self, relative):
        path = _regular_chain(self._sandbox_path(relative), True)
        marker = _json(_read(path / OWNER, 4096))
        if (not isinstance(marker, dict) or set(marker) != {'format', 'root', 'sandbox', 'nonce'}
                or type(marker.get('format')) is not int or marker['format'] != 1
                or marker.get('root') != str(self.root) or marker.get('sandbox') != relative
                or not re.fullmatch('[a-f0-9]{32}', str(marker.get('nonce', '')))):
            raise ProfileError('Sandbox ownership marker does not match')
        return path

    def commit(self, profile, sandbox, evidence, *, _fault=lambda phase: None):
        _identifier(profile)
        if not isinstance(evidence, NormalExitEvidence):
            raise ProfileError('Explicit normal-exit evidence is required')
        evidence.validate()
        source = self._owned_sandbox(sandbox) / SAVE_DIR
        _regular_chain(source, True)
        blobs = {}
        for item in sorted(_entries(source, MAX_FILES), key=lambda p: p.name.lower()):
            if not _save_name(item.name):
                raise ProfileError('Unknown file in native save directory')
            if item.name.lower() in {name.lower() for name in blobs}:
                raise ProfileError('Duplicate native save name')
            blob = _read(item, HEADER + MAX_IMAGE + 512)
            validate_save(blob)
            blobs[item.name] = blob
            if len(blobs) > MAX_FILES or sum(map(len, blobs.values())) > MAX_TOTAL:
                raise ProfileError('Checkpoint set exceeds bounds')
        if not blobs:
            raise ProfileError('No native saves to checkpoint')
        folder = self.store / profile
        if not folder.exists():
            _regular_chain(self.store, True)
            folder.mkdir()
        _regular_chain(folder, True)
        # Verify the previous latest before any publication; never silently repair it.
        if (folder / 'latest.json').exists() or (folder / 'latest.json').is_symlink():
            self.load(profile)
        stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
        version = 'v' + stamp + '-' + uuid.uuid4().hex
        staged = folder / ('.stage-' + uuid.uuid4().hex)
        staged.mkdir()
        (staged / 'saves').mkdir()
        files = []
        for name, blob in blobs.items():
            _write_new(staged / 'saves' / name, blob)
            files.append({'name': name, **validate_save(blob)})
        manifest = {'format': 1, 'build_sha256': BUILD, 'version': version,
                    'profile': profile, 'files': files,
                    'completion_evidence_kind': 'caller_attested_owned_normal_exit',
                    'normal_exit': asdict(evidence)}
        manifest_bytes = _encoded(manifest)
        _write_new(staged / 'manifest.json', manifest_bytes)
        _fault('before_version_rename')
        destination = folder / version
        if destination.exists():
            raise ProfileError('Immutable version already exists')
        staged.rename(destination)
        _fault('after_version_rename')
        # Detect failed writes or staged-data corruption before moving latest.
        self._load_version(profile, version, hashlib.sha256(manifest_bytes).hexdigest())
        latest = {'format': 1, 'version': version,
                  'manifest_sha256': hashlib.sha256(manifest_bytes).hexdigest()}
        temporary = folder / ('.latest-' + uuid.uuid4().hex + '.json')
        _write_new(temporary, _encoded(latest))
        _fault('before_latest_replace')
        if (folder / 'latest.json').exists() or (folder / 'latest.json').is_symlink():
            _regular_chain(folder / 'latest.json')
        _regular_chain(folder, True)
        os.replace(temporary, folder / 'latest.json')
        return version

    def load(self, profile, version=None):
        _identifier(profile)
        folder = _regular_chain(self.store / profile, True)
        marker = _json(_read(folder / 'latest.json', 4096))
        if (not isinstance(marker, dict) or set(marker) != {'format', 'version', 'manifest_sha256'}
                or type(marker['format']) is not int or marker['format'] != 1
                or not isinstance(marker['version'], str)
                or not VERSION.fullmatch(marker['version'])
                or not re.fullmatch('[a-f0-9]{64}', str(marker['manifest_sha256']))):
            raise ProfileError('Invalid latest checkpoint marker')
        selected = marker['version'] if version is None else version
        expected = marker['manifest_sha256'] if selected == marker['version'] else None
        return self._load_version(profile, selected, expected)

    def _load_version(self, profile, selected, expected_digest):
        if not isinstance(selected, str) or not VERSION.fullmatch(selected):
            raise ProfileError('Invalid immutable version identifier')
        folder = _regular_chain(self.store / profile, True)
        directory = _regular_chain(folder / selected, True)
        raw_manifest = _read(directory / 'manifest.json', 1024 * 1024)
        if expected_digest is not None and hashlib.sha256(raw_manifest).hexdigest() != expected_digest:
            raise ProfileError('Latest manifest digest mismatch')
        manifest = _json(raw_manifest)
        if (not isinstance(manifest, dict)
                or set(manifest) != {'format', 'build_sha256', 'version', 'profile', 'files',
                                     'completion_evidence_kind', 'normal_exit'}
                or type(manifest.get('format')) is not int or manifest['format'] != 1
                or manifest.get('profile') != profile or manifest.get('version') != selected
                or manifest.get('build_sha256') != BUILD
                or manifest.get('completion_evidence_kind') != 'caller_attested_owned_normal_exit'):
            raise ProfileError('Unsupported checkpoint manifest')
        try:
            NormalExitEvidence(**manifest['normal_exit']).validate()
        except (KeyError, TypeError) as exc:
            raise ProfileError('Invalid normal-exit manifest evidence') from exc
        records = manifest.get('files')
        if not isinstance(records, list) or not 1 <= len(records) <= MAX_FILES:
            raise ProfileError('Invalid checkpoint file count')
        saves = _regular_chain(directory / 'saves', True)
        blobs = {}
        for record in records:
            if (not isinstance(record, dict) or not isinstance(record.get('name'), str)
                    or not _save_name(record['name'])
                    or record['name'].lower() in {name.lower() for name in blobs}):
                raise ProfileError('Invalid checkpoint filename')
            blob = _read(saves / record['name'], HEADER + MAX_IMAGE + 512)
            if (record != {'name': record['name'], **validate_save(blob)}
                    or any(type(record[k]) is not int for k in
                           ('size', 'image_size', 'payload_size', 'payload_discriminator'))):
                raise ProfileError('Checkpoint file digest or shape mismatch')
            blobs[record['name']] = blob
            if sum(map(len, blobs.values())) > MAX_TOTAL:
                raise ProfileError('Checkpoint set exceeds bounds')
        if ({p.name for p in _entries(saves, MAX_FILES)} != set(blobs)
                or {p.name for p in _entries(directory, 2)} != {'saves', 'manifest.json'}):
            raise ProfileError('Unknown files in immutable checkpoint')
        return manifest, blobs

    def restore(self, profile, sandbox, version=None):
        manifest, blobs = self.load(profile, version)
        destination = self.new_sandbox(sandbox)
        current = destination
        for piece in SAVE_DIR.parts:
            _regular_chain(current, True)
            current = current / piece
            current.mkdir()
        for name, blob in blobs.items():
            _write_new(current / name, blob)
        _write_new(destination / '.fof-restored-checkpoint.json', _encoded({
            'profile': profile, 'version': manifest['version'], 'build_sha256': BUILD}))
        return destination
