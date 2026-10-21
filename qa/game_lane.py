"""Exclusive QA leases using the user's coop-anything checkout, never vendored.

COOP_ANYTHING_ROOT selects that checkout explicitly. Otherwise an ancestor of
this source tree must contain tools/gameslot/gameslot.py. No games are started
or killed here. In particular, expired foreign leases are refused, not reaped.
"""
import ctypes
from ctypes import wintypes
import importlib.util
import math
import os
from pathlib import Path
import stat
import time
import uuid


class LaneError(RuntimeError):
    pass


def _plain_path(path):
    path = Path(os.path.abspath(path))
    for part in reversed((path, *path.parents)):
        info = part.lstat()
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise LaneError("QA gameslot paths must not contain reparse points")
    return path


def _backend():
    requested = os.environ.get("COOP_ANYTHING_ROOT")
    candidates = [Path(requested)] if requested else list(Path(__file__).absolute().parents)
    for root in candidates:
        source = root / "tools" / "gameslot" / "gameslot.py"
        if not source.is_file():
            continue
        source = _plain_path(source)
        spec = importlib.util.spec_from_file_location("fof_qa_gameslot_backend", source)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        # Respect the shared lock, but never redirect it outside this checkout's
        # private local tree through an inherited GAMESLOT_DIR.
        root = _plain_path(root)
        local = root / "local"
        if local.exists() or local.is_symlink():
            _plain_path(local)
        state = Path(os.path.abspath(module.STATE))
        if not state.is_relative_to(local):
            raise LaneError("GAMESLOT_DIR must remain inside the selected hub's local directory")
        for part in (state, *state.parents):
            if part.exists() or part.is_symlink():
                _plain_path(part)
                break
        return module
    raise LaneError("QA gameslot unavailable: set COOP_ANYTHING_ROOT to a coop-anything checkout")


def _retained_identity(process):
    """Query only the retained Popen handle, never an untrusted log PID."""
    if os.name != "nt" or process.poll() is not None or not hasattr(process, "_handle"):
        raise LaneError("An active retained Windows Popen handle is required")
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetProcessId.argtypes = [wintypes.HANDLE]
    kernel.GetProcessId.restype = wintypes.DWORD
    kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    kernel.GetProcessTimes.restype = wintypes.BOOL
    kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD,
                                               wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    kernel.QueryFullProcessImageNameW.restype = wintypes.BOOL
    handle = wintypes.HANDLE(int(process._handle))
    if kernel.GetProcessId(handle) != process.pid:
        raise LaneError("Retained Popen handle does not match its PID")
    times = [wintypes.FILETIME() for _ in range(4)]
    if not kernel.GetProcessTimes(handle, *[ctypes.byref(t) for t in times]):
        raise LaneError("Cannot query retained launcher creation time")
    image, length = ctypes.create_unicode_buffer(32768), wintypes.DWORD(32768)
    if not kernel.QueryFullProcessImageNameW(handle, 0, image, ctypes.byref(length)):
        raise LaneError("Cannot query retained launcher image")
    expected = process.args[0]
    if os.path.normcase(os.path.abspath(image.value)) != os.path.normcase(os.path.abspath(expected)):
        raise LaneError("Retained launcher image differs from the exact Popen command")
    return {"name": Path(image.value).name.lower(),
            "created": times[0].dwLowDateTime | times[0].dwHighDateTime << 32}


class GameLane:
    def __init__(self, budget_seconds, *, backend=None):
        if not math.isfinite(budget_seconds) or budget_seconds <= 0:
            raise LaneError("A finite positive QA budget is required")
        self.backend = backend
        self.ttl = max(900, math.ceil(budget_seconds) + 120)
        self.owner = "fof-qa-" + str(os.getpid()) + "-" + uuid.uuid4().hex
        self.slots = []
        self.processes = []
        self.held = False

    def acquire(self):
        if self.held:
            raise LaneError("QA lane is already held")
        backend = self.backend or _backend()
        self.backend = backend
        if backend.max_slots() < 2:
            raise LaneError("Two game copies require at least two configured gameslots")
        # The upstream backend reaps expired foreign leases. Disable that path
        # in this private module instance: QA must not kill another owner's game.
        def refuse_foreign(_records):
            raise LaneError("Expired foreign gameslot must be released by its owner; no process was killed")
        backend.kill_registered = refuse_foreign
        try:
            # One nonwaiting, atomic all-slot request; FIFO and RAM checks remain.
            ok, slots, info = backend.try_acquire(self.owner, self.ttl, None, time.time())
            if not ok:
                raise LaneError("QA gameslot busy: " + str(info))
            self.held = True
            self.slots = slots
            if len(slots) != backend.max_slots() or len(slots) < 2:
                raise LaneError("QA gameslot did not grant an exclusive two-copy lane")
        except BaseException:
            backend.release(self.owner)  # Own partial lease only, never foreign records.
            self.held = False
            raise
        return self

    def register_launcher(self, process):
        if not self.held:
            raise LaneError("Acquire the QA gameslot before launching a game")
        self.processes.append(process)  # Retain even when registration fails.
        identity = _retained_identity(process)
        if self.backend.process_info(process.pid) != identity:
            raise LaneError("Gameslot PID identity differs from the retained owned launcher")
        if self.backend.register_pid(self.owner, process.pid) != len(self.slots):
            raise LaneError("Could not register owned launcher on every held gameslot")

    def release(self, *, children_stopped=True):
        if not self.held:
            return
        if not children_stopped or any(p.poll() is None for p in self.processes):
            raise LaneError("Owned launcher/game still active after cleanup; QA lease retained until expiry")
        if self.backend.release(self.owner) != len(self.slots):
            raise LaneError("QA lease release count differs from the exclusive lease")
        self.held = False
