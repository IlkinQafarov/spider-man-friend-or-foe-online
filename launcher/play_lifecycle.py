"""Private finite QA for the REAL native --play path; not a shipping dependency.

REVIEW BEFORE RUNNING. This script launches both existing isolated game copies,
with real-human input flags and unlimited native network frames, over loopback.
It supplies no OS input and uses no Steam account. The default is idle QA;
--campaign deliberately drives native menus/hub with a research bot, and
--tutorial-ui exercises the first-area hint using agreed network input.
After >=300 matching sampled frames it requests WM_CLOSE of both positively
owned games, requiring actual game and launcher exit0. Timeout/job termination
is always failure. Default mode is idle/lifecycle QA; the tutorial regression
adds native popup ownership evidence, without physical-input usability claims.

Example (root worker runs after review, elevated as game sandbox requires):
    python launcher/play_lifecycle.py --name play-lifecycle-001
Pure parser/environment checks only:
    python launcher/play_lifecycle.py --selftest
All results/logs remain private; existing copies, originals and saves are not
modified by this Python script. Native launcher redirects game writes into NEW
sandboxes, retaining its own kill-on-close child job. Foreground/pause behavior
is observed as implemented; the script never changes focus or supplies OS input.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import stat
import struct
import subprocess
import sys
import tempfile
import time
import uuid

HERE = Path(__file__).absolute().parent
ROOT = HERE.parent
CRITICAL = "c46b90afe36571cf3f4fbe3d4e7918da833297f78119d40504e96b4f531b36ea"
BASELINE = "5c85b8c90c60b6d615a1aa50ae95d7b0acd32bbbe24bc68c282c136e5e7af42b"
SEATS = ("host", "peer")
LANGUAGES = ("English", "Russian")
HOOK = re.compile(r"^HOOK_READY pid=(\d+) sha256=([0-9a-f]{64}) selftest=0 timeout=0s play=1$", re.M)
EXIT = re.compile(r"^PROCESS_EXIT pid=(\d+) code=(\d+)$", re.M)
ROW = re.compile(
    r"^NET state tick=(\d+) rng=([0-9a-f]{8}) "
    r"pos0=([0-9a-f]{8},[0-9a-f]{8},[0-9a-f]{8}) "
    r"pos1=([0-9a-f]{8},[0-9a-f]{8},[0-9a-f]{8}) "
    r"slots=0,1 input=([0-9a-f]{8},[0-9a-f]{8}) "
    r"world=(\d+),([0-9a-f]{8}) progress=([0-9a-f]{8}) area=([0-9a-f]{8})$"
)
ROW_PAIRED = re.compile(ROW.pattern.replace("slots=0,1", r"slots=(?:0,1|1,0)"), re.M)
ROW_NATIVE_AI = re.compile(
    ROW.pattern.replace("slots=0,1", r"slots=(?:0,1|1,0|0,-1|-1,0)"), re.M)
FAILURE = re.compile(r"NET FAIL|EXCEPTION|FAIL closed|TIMEOUT:", re.I)
FIXTURE = re.compile(r"KEYBOARD_FIXTURE|DISCONNECT_NEUTRAL|MUTATION_FIXTURE|TRANSITION PARTY_INSIDE|JITTER_FIXTURE|PAUSE_FIXTURE|PAUSE ASSERT")


class Failed(RuntimeError):
    pass


def tutorial_ui_evidence(logs, complete=False):
    """Require native observations on both peers, never just a fixture intent."""
    if len(logs) != 2:
        raise Failed("Tutorial UI requires exactly two peers")
    forms = (
        r"OPEN tick=(\d+) area=Tokyo/1_Level/1_Area scene=14",
        r"PEER_IGNORED tick=(\d+) scene=14 attempts=3 host=0 peer=0",
        r"HOST_ACCEPT tick=(\d+) scene=14 host=1 peer=0",
        r"CLOSED tick=(\d+) scene=14 host=1 peer=0",
    )
    observed = []
    for seat, value in zip(SEATS, logs):
        lines = re.findall(r"^TUTORIAL_UI (.*)$", value, re.M)
        if len(lines) > len(forms):
            raise Failed(seat + ": duplicate tutorial UI observation")
        ticks = []
        for form, line in zip(forms, lines):
            match = re.fullmatch(form, line)
            if not match:
                raise Failed(seat + ": malformed/out-of-order tutorial UI observation")
            ticks.append(int(match[1]))
        if any(right <= left for left, right in zip(ticks, ticks[1:])):
            raise Failed(seat + ": tutorial UI ticks must strictly increase")
        if len(ticks) >= 3 and ticks[2] - ticks[0] < 120:
            raise Failed(seat + ": tutorial host Accept was not withheld for120 ticks")
        if lines:
            if value.count("CAMPAIGN ASSERT NEW_GAME operation=0 selection=0") != 1:
                raise Failed(seat + ": tutorial UI requires native New Game evidence")
            if not re.search(r"^CAMPAIGN ASSERT GAMEPLAY path=Tokyo/1_Level/1_Area area=\d+ flow=128 sidekick=\d+ prepared=1$", value, re.M):
                raise Failed(seat + ": tutorial UI did not start in the retail first area")
        if "LEVEL_END " in value or "LEVEL_END_TEST" in value:
            raise Failed(seat + ": tutorial UI contaminated by the shortened level runner")
        if complete and len(lines) != len(forms):
            raise Failed(seat + ": missing tutorial UI observation")
        observed.append((lines, ticks))
    if len(observed) != 2:
        raise Failed("Tutorial UI requires exactly two peers")
    common = min(len(item[0]) for item in observed)
    if observed[0][0][:common] != observed[1][0][:common]:
        raise Failed("Tutorial UI observations differ between peers")
    return {"complete": all(len(item[0]) == len(forms) for item in observed),
            "ticks": observed[0][1][:common]}


def no_reparse(path, new=False):
    path = Path(os.path.abspath(path))
    for ancestor in reversed((path, *path.parents)):
        if new and ancestor == path:
            if ancestor.exists() or ancestor.is_symlink():
                raise Failed("Expected NEW path: " + ancestor.name)
            continue
        info = ancestor.lstat()
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise Failed("Reparse/symlink rejected: " + ancestor.name)
    return path


def environment(source, seat, diagnostics=True, campaign=False, deferred=False, profile=None, hub_spawn=-1, skip_nis=False, auto_together=True, net_port="49375", tutorial_ui=False, slot_fixture=None, overwrite_no=False, live_join=False, wait_link=False):
    env = {key: value for key, value in source.items() if not key.upper().startswith("COOP_PROBE_")}
    values = {
        "AUTOTEST": "ExecutionPath", "INPUTBOT": "0", "PLAYBOT": "0", "ACTIONBOT": "0",
        "HUMAN": "1", "HUMAN_FIXTURE": "0", "PATHSYNC": "1", "AMBIENT_SPLIT": "1",
        "CLEAN_EXIT": "1", "NET_MODE": "host" if seat == "host" else "join", "NET_FRAMES": "0",
        "NET_ADDRESS": "127.0.0.1", "NET_PORT": net_port, "AREA": "Tokyo/1_Level/1_Area",
        "CONTENT_SHA256": CRITICAL,
        "DIAGNOSTICS": "1" if diagnostics else "0",
    }
    if campaign:
        # Retail route: New Game -> hub -> mission; the campaign bot drives only host menus/hub.
        values.pop("AUTOTEST")
        values.pop("AREA")
        values.update(CAMPAIGN="1", CAMPAIGN_FIXTURE="1")
        if deferred:
            # Launcher flow: the game opens first; the role arrives later in <sandbox>/connect.json.
            values["NET_MODE"] = "deferred"
        if profile:
            values["PROFILE"] = str(profile)
        if not auto_together:
            values["AUTO_TOGETHER"] = "0"
        if os.environ.get("FOF_RESEARCH_MOVIE_SKIP_TEST") == "1":
            values["MOVIE_SKIP_TEST"] = "1"
        if os.environ.get("FOF_RESEARCH_HUB_JUMP_TEST") == "1":
            values["HUB_JUMP_TEST"] = "1"
        if os.environ.get("FOF_RESEARCH_LEVEL_END_TEST") == "1":
            values["LEVEL_END_TEST"] = "1"
            if os.environ.get("FOF_RESEARCH_LEVEL_END_BOSS_WAIT"):
                values["LEVEL_END_BOSS_WAIT"] = os.environ["FOF_RESEARCH_LEVEL_END_BOSS_WAIT"]
            if os.environ.get("FOF_RESEARCH_LEVEL_END_DONE"):
                values["LEVEL_END_DONE"] = os.environ["FOF_RESEARCH_LEVEL_END_DONE"]
        if os.environ.get("FOF_RESEARCH_NIS_GATE_TEST") == "1":
            values["NIS_GATE_TEST"] = "1"
        if os.environ.get("FOF_RESEARCH_HUB_WALK_DPAD") == "1":
            values["HUB_WALK_DPAD"] = "1"
        if os.environ.get("FOF_RESEARCH_HOST_WAIT_LINK") == "1" and seat == "host":
            values["HOST_WAIT_LINK"] = "1"
            if os.environ.get("FOF_RESEARCH_HOST_CONTINUE") == "1":
                values["HOST_CONTINUE"] = "1"
        if hub_spawn >= 0:
            values["HUB_SPAWN"] = str(hub_spawn)
        if skip_nis:
            values["SKIP_NIS"] = "1"
        if source.get("FOF_RESEARCH_LIVE_RECORD_TEST") == "1":
            values["LIVE_RECORD_TEST"] = "1"
        if os.environ.get("FOF_RESEARCH_HUB_SOUND") == "0":
            values["HUB_PARTNER_SOUND"] = "0"
        if os.environ.get("FOF_RESEARCH_SIDEKICK_TEST") == "1":
            values["HUB_SIDEKICK_TEST"] = "1"
        # The launcher passes its translated partner-wait notice; tests may pass one to check a language renders.
        # Test the built-in US key table on a PC that has the US layout.
        if os.environ.get("FOF_RESEARCH_BUILTIN_US_KEYS") == "1":
            values["BUILTIN_US_KEYS"] = "1"
        if os.environ.get("FOF_RESEARCH_WAIT_TEXT"):
            values["WAIT_TEXT"] = os.environ["FOF_RESEARCH_WAIT_TEXT"]
    if live_join:
        values["LIVE_JOIN_TEST"] = "1"
    if wait_link:
        if not campaign or not deferred:
            raise Failed("Wait-link mode requires a deferred campaign peer")
        values["HOST_WAIT_LINK"] = "1"
    if tutorial_ui:
        if not campaign or any(key in values for key in ("LEVEL_END_TEST", "HUB_JUMP_TEST", "NIS_GATE_TEST", "HOST_WAIT_LINK", "HOST_CONTINUE", "HUB_SPAWN", "HUB_SIDEKICK_TEST")):
            raise Failed("Tutorial UI regression requires the fresh retail first-area route")
        values["TUTORIAL_UI_TEST"] = "1"
    if slot_fixture is not None:
        if not campaign or slot_fixture not in (0, 1, 2):
            raise Failed("Save-slot fixture requires campaign and a row in0..2")
        values["SLOT_FIXTURE"] = str(slot_fixture)
    if overwrite_no:
        if slot_fixture is None:
            raise Failed("Overwrite No requires an explicit slot fixture")
        values["OVERWRITE_FIXTURE_NO"] = "1"
    env.update({"COOP_PROBE_" + key: value for key, value in values.items()})
    return env


def slot_rows(value):
    if value == "none":
        return ()
    parts = value.split(",")
    if not parts or any(part not in ("0", "1", "2") for part in parts) or len(set(parts)) != len(parts):
        raise argparse.ArgumentTypeError("Use distinct zero-based rows0,1,2 or 'none'")
    return tuple(sorted(map(int, parts)))


def native_save_slot(data):
    # Test fixture parsing only. This does not attest that native writes succeeded.
    header = 8232
    if len(data) < header + 512 or data[:4] != b"RGMH" or struct.unpack_from("<II", data, 4) != (1, header):
        raise Failed("Invalid bounded native seed container")
    image = struct.unpack_from("<I", data, 20)[0]
    if image > 16 * 1024 * 1024 or len(data) != header + image + 512:
        raise Failed("Invalid seed image/payload length")
    offset = header + image
    row = struct.unpack_from("<I", data, offset)[0]
    if row > 2:
        raise Failed("Invalid native seed slot index")
    return row, offset


def retarget_seed(data, row):
    if row not in (0, 1, 2):
        raise Failed("Invalid destination seed slot")
    _, offset = native_save_slot(data)
    result = bytearray(data)
    struct.pack_into("<I", result, offset, row)
    return bytes(result)


def save_inventory(profile):
    folder = profile / "Documents" / "Activision" / "Spider-Man FoF" / "Saved Games"
    if not folder.exists():
        if profile.exists():
            no_reparse(profile)
        return {}
    no_reparse(folder)
    result = {}
    for path in folder.iterdir():
        no_reparse(path)
        if not path.is_file() or path.stat().st_nlink != 1:
            raise Failed("Unexpected linked/non-file save fixture entry")
        if path.stat().st_size > 8232 + 16 * 1024 * 1024 + 512:
            raise Failed("Oversized save fixture entry")
        data = path.read_bytes()
        result[path.name] = {"sha256": hashlib.sha256(data).hexdigest(), "size": len(data), "mtime_ns": path.stat().st_mtime_ns}
    return result


def seed_rows(profile, data, rows_to_seed):
    if not rows_to_seed:
        return {}
    no_reparse(profile, new=True)
    folder = profile / "Documents" / "Activision" / "Spider-Man FoF" / "Saved Games"
    folder.mkdir(parents=True)
    for row in rows_to_seed:
        with (folder / ("SavedGame" + str(row + 1) + ".SFOFSaveGame")).open("xb") as stream:
            stream.write(retarget_seed(data, row))
    return save_inventory(profile)


def load_native_fixture_seed(path):
    source = no_reparse(Path(path).absolute())
    try:
        relative = source.relative_to(ROOT)
    except ValueError:
        raise Failed("Slot fixture seed must come from a workspace game sandbox")
    if not relative.parts[0].startswith("run-") or not re.fullmatch(r"SavedGame[123]\.SFOFSaveGame", source.name):
        raise Failed("Slot fixture seed must be a native save retained in a run-* sandbox/profile")
    info = source.stat()
    if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_size > 8232 + 16 * 1024 * 1024 + 512:
        raise Failed("Invalid linked/oversized native fixture seed")
    data = source.read_bytes()
    row, _ = native_save_slot(data)
    if row != int(source.name[9]) - 1:
        raise Failed("Native fixture seed filename/index mismatch")
    return data


def slot_evidence(logs, target, seeded, overwrite_no=False, complete=False):
    """Native choice evidence, separate from file preservation and process gates."""
    if len(logs) != 2 or len(seeded) != 2 or target not in (0, 1, 2):
        raise Failed("Save-slot evidence requires two peers and a valid host slot")
    full = len(seeded[1]) == 3
    used = target in seeded[0]
    outcome = "full" if full else "no" if overwrite_no else "yes" if used else "empty"
    peer_target = next((row for row in range(3) if row not in seeded[1]), None)
    done = []
    for index, value in enumerate(logs):
        role = index + 1
        if outcome in ("no", "full") and re.search(r"CAMPAIGN ASSERT (?:PROFILE|HUB|MISSION_COMMIT|GAMEPLAY)", value):
            raise Failed(SEATS[index] + ": canceled save choice initialized/loaded a profile")
        if outcome == "full":
            found = re.findall(r"^CAMPAIGN SLOT_REFUSED reason=partner_has_no_empty_slots saves_unchanged=1$", value, re.M)
            if len(found) > 1 or "CAMPAIGN OVERWRITE_CHOICE" in value:
                raise Failed("Full partner slots unexpectedly entered an overwrite choice")
            ready = bool(found)
        else:
            expected_slot = target if index == 0 else peer_target
            choices = re.findall(r"^CAMPAIGN OVERWRITE_CHOICE answer=(yes|no) role=(\d+) own_slot=(\d+)$", value, re.M)
            if len(choices) > 1 or any(answer != ("no" if overwrite_no else "yes") or int(r) != role or int(row) != expected_slot for answer, r, row in choices):
                raise Failed(SEATS[index] + ": incorrect/duplicate native overwrite choice")
            if used:
                wait = "CAMPAIGN OVERWRITE_WAIT host_slot=" + str(target) + " peer_slot=" + str(peer_target) + " role=" + str(role)
                ready = len(choices) == 1 and wait in value
                if index == 0:
                    prompt = "CAMPAIGN OVERWRITE_PROMPT source=STATE_OVERWRITE_GAME_PROMPT configured=1 slot=" + str(target)
                    ready = ready and prompt in value and value.index(wait) < value.index(prompt) < value.index("CAMPAIGN OVERWRITE_CHOICE")
            else:
                if choices or "CAMPAIGN OVERWRITE_WAIT" in value or "CAMPAIGN OVERWRITE_PROMPT" in value:
                    raise Failed("Empty slot unexpectedly entered overwrite confirmation")
                ready = True
            if outcome not in ("no", "full"):
                profiles = re.findall(r"^CAMPAIGN ASSERT PROFILE slot=(\d+) native_init=1 operation=0$", value, re.M)
                if len(profiles) > 1 or any(int(row) != expected_slot for row in profiles):
                    raise Failed(SEATS[index] + ": incorrect/duplicate native initialized slot")
                ready = ready and profiles == [str(expected_slot)]
        if complete and not ready:
            raise Failed(SEATS[index] + ": missing native save-slot outcome evidence")
        done.append(ready)
    return {"outcome": outcome, "complete": len(done) == 2 and all(done), "host_slot": target, "peer_slot": peer_target}


def phase_stop_evidence(logs, target, seeded, complete=False):
    if len(logs) != 2 or target not in seeded[0] or len(seeded[1]) >= 3:
        raise Failed("Phase STOP requires a used host row and an empty peer row")
    peer_row = next(row for row in range(3) if row not in seeded[1])
    waits = ["CAMPAIGN OVERWRITE_WAIT host_slot=" + str(target) + " peer_slot=" + str(peer_row) + " role=" + str(role) for role in (1, 2)]
    prompt = "CAMPAIGN OVERWRITE_PROMPT source=STATE_OVERWRITE_GAME_PROMPT configured=1 slot=" + str(target)
    for value in logs:
        if re.search(r"CAMPAIGN ASSERT (?:PROFILE|HUB|MISSION_COMMIT|GAMEPLAY)|CAMPAIGN OVERWRITE_CHOICE", value):
            raise Failed("Phase STOP forwarded a save choice/profile/load before cancellation")
    ready = all(wait in value for wait, value in zip(waits, logs)) and prompt in logs[0]
    if ready and logs[0].index(waits[0]) >= logs[0].index(prompt):
        raise Failed("Phase STOP prompt preceded its mutually held slot")
    if complete:
        if not ready or "NET user stop requested sent=1" not in logs[0]:
            raise Failed("Phase STOP lacks held prompt/host ordered STOP evidence")
        terminal = re.findall(r"^NET peer ended session normally phase=\d+; deferred owned WM_CLOSE$", logs[1], re.M)
        posted = "NET peer phase stop WM_CLOSE posted; native callbacks unwind before teardown"
        if len(terminal) != 1 or logs[1].count(posted) != 1 or logs[1].index(terminal[0]) >= logs[1].index(posted):
            raise Failed("Phase STOP lacks exact deferred peer close/unwind evidence")
        tail = logs[1].split(terminal[0], 1)[1]
        if re.search(r"CAMPAIGN ASSERT|CAMPAIGN OVERWRITE_|CAMPAIGN SAVE_ACK|CAMPAIGN AUTOSAVE_NOTICE", tail):
            raise Failed("Native shared menu/save work continued after terminal phase STOP")
    return {"outcome": "phase_stop", "complete": ready, "host_slot": target, "peer_slot": peer_row}


def text(path):
    """Ignore only a trailing unfinished write; malformed complete rows fail."""
    if not path.exists():
        return ""
    no_reparse(path)
    value = path.read_text(encoding="utf-8", errors="replace")
    return value[:value.rfind("\n") + 1]


def rows(value, stride=1, *, allow_nonzero_start=False, allow_native_ai=False,
         allow_swapped_slots=False):
    found = {}
    previous = None if allow_nonzero_start else -stride
    row_pattern = ROW_NATIVE_AI if allow_native_ai else ROW_PAIRED if allow_swapped_slots else ROW
    for line in value.splitlines():
        if not line.startswith("NET state "):
            continue
        match = row_pattern.fullmatch(line)
        if not match:
            raise Failed("Malformed state row (requires progress, area and logical slots0,1): " + line[:180])
        tick = int(match[1])
        expected = tick if previous is None else previous + stride
        if tick != expected or tick > 0xffffffff:
            raise Failed("Missing, duplicated or out-of-order state tick: " + str(tick))
        if int(match[6]) < 2:
            raise Failed("State row does not contain both party actors")
        for encoded in (match[3], match[4]):
            if not all(math.isfinite(struct.unpack("<f", bytes.fromhex(word)[::-1])[0])
                       for word in encoded.split(",")):
                raise Failed("Nonfinite party pose at tick " + str(tick))
        previous = tick
        found[tick] = line
    return found


def require_paired_rows(rows_by_tick, minimum_tick):
    checked = [line for tick, line in rows_by_tick.items() if tick >= minimum_tick]
    if not checked or any(ROW_PAIRED.fullmatch(line) is None for line in checked):
        raise Failed("State after LIVE_JOIN COMPLETE must contain both logical party slots")


def compare(first, second, stride=1):
    common = sorted(set(first) & set(second))
    if common != list(range(0, len(common) * stride, stride)):
        raise Failed("Common samples are not consecutive from tick0")
    for tick in common:
        if first[tick] != second[tick]:
            raise Failed("Paired state differs at common tick " + str(tick))
    return common[-1] + 1 if common else 0


def compare_suffix(first, second, stride=1, minimum_tick=None):
    """Compare a shared post-rejoin suffix while retaining its absolute ticks."""
    common = sorted(set(first) & set(second))
    if minimum_tick is not None:
        common = [tick for tick in common if tick >= minimum_tick]
    if common and common != list(range(common[0], common[0] + len(common) * stride, stride)):
        raise Failed("Common post-rejoin samples are not consecutive at their absolute ticks")
    for tick in common:
        if minimum_tick is not None and any(
                " slots=0,1 " not in row and " slots=1,0 " not in row
                for row in (first[tick], second[tick])):
            raise Failed("Post-admission state still contains an AI controller at tick " + str(tick))
        if first[tick] != second[tick]:
            raise Failed("Paired post-rejoin state differs at absolute tick " + str(tick))
    base_tick = minimum_tick if minimum_tick is not None else common[0] if common else 0
    return max(0, common[-1] - base_tick + 1) if common else 0


def live_join_complete_tick(host_log, replacement_log, *, required=True, require_drop=True):
    """Validate explicit in-mission handoff evidence without accepting a hub resume."""
    marker = re.compile(r"^LIVE_JOIN COMPLETE tick=(\d+) preserved=1$", re.M)
    if require_drop:
        host_parts = host_log.rsplit("NET PARTNER_LOST", 1)
        if len(host_parts) != 2:
            if required:
                raise Failed("Host log lacks NET PARTNER_LOST for the live join")
            return None
        host_after_join = host_parts[1]
    else:
        frozen = list(re.finditer(r"^LIVE_JOIN FROZEN(?: .*)?$", host_log, re.M))
        if len(frozen) != 1:
            if required:
                raise Failed("Initial live join requires one LIVE_JOIN FROZEN marker")
            return None
        host_after_join = host_log[frozen[0].end():]
    host_ticks = marker.findall(host_after_join)
    replacement_ticks = marker.findall(replacement_log)
    if not host_ticks or not replacement_ticks:
        if required:
            raise Failed("Host and replacement must both report LIVE_JOIN COMPLETE")
        return None
    if len(host_ticks) != 1 or len(replacement_ticks) != 1 or host_ticks[0] != replacement_ticks[0]:
        raise Failed("Host/replacement LIVE_JOIN COMPLETE ticks must match exactly once")
    if int(host_ticks[0]) == 0:
        raise Failed("Live join completion reset the host mission tick to zero")
    if re.search(r"JOIN_IN_MISSION|Helicarrier|resume=1|MISSION_COMMIT", host_after_join, re.I):
        raise Failed("Host attempted the old Helicarrier mission-resume path after disconnect")
    return int(host_ticks[0])


class Win32:
    """Only handles opened from this run's own launcher output are game owners."""
    class Basic(C.Structure):
        _fields_ = [(key, C.c_void_p) for key in ("exit", "peb", "affinity", "priority", "pid", "parent")]

    def __init__(self):
        self.kernel = C.WinDLL("kernel32", use_last_error=True)
        self.user = C.WinDLL("user32", use_last_error=True)
        self.nt = C.WinDLL("ntdll")
        self.kernel.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
        self.kernel.OpenProcess.restype = W.HANDLE
        self.kernel.CloseHandle.argtypes = [W.HANDLE]
        self.kernel.CloseHandle.restype = W.BOOL
        self.kernel.GetProcessTimes.argtypes = [W.HANDLE] + [C.POINTER(W.FILETIME)] * 4
        self.kernel.GetProcessTimes.restype = W.BOOL
        self.kernel.GetExitCodeProcess.argtypes = [W.HANDLE, C.POINTER(W.DWORD)]
        self.kernel.GetExitCodeProcess.restype = W.BOOL
        self.kernel.WaitForSingleObject.argtypes = [W.HANDLE, W.DWORD]
        self.kernel.WaitForSingleObject.restype = W.DWORD
        self.kernel.QueryFullProcessImageNameW.argtypes = [W.HANDLE, W.DWORD, W.LPWSTR, C.POINTER(W.DWORD)]
        self.kernel.QueryFullProcessImageNameW.restype = W.BOOL
        self.nt.NtQueryInformationProcess.argtypes = [W.HANDLE, C.c_int, C.POINTER(self.Basic), W.ULONG, C.POINTER(W.ULONG)]
        self.nt.NtQueryInformationProcess.restype = C.c_long
        self.callback = C.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
        self.user.EnumWindows.argtypes = [self.callback, W.LPARAM]
        self.user.EnumWindows.restype = W.BOOL
        self.user.GetWindowThreadProcessId.argtypes = [W.HWND, C.POINTER(W.DWORD)]
        self.user.GetWindowThreadProcessId.restype = W.DWORD
        self.user.PostMessageW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM]
        self.user.PostMessageW.restype = W.BOOL

    def creation(self, handle):
        times = [W.FILETIME() for _ in range(4)]
        if not self.kernel.GetProcessTimes(handle, *map(C.byref, times)):
            raise Failed("Cannot query owned process creation time")
        return times[0].dwLowDateTime | (times[0].dwHighDateTime << 32)

    def capture(self, pid, launcher, expected):
        # QUERY_LIMITED_INFORMATION and SYNCHRONIZE; no write/injection rights.
        handle = self.kernel.OpenProcess(0x1000 | 0x100000, False, pid)
        if not handle:
            raise Failed("Cannot retain reported owned game handle")
        try:
            basic, returned = self.Basic(), W.ULONG()
            if self.nt.NtQueryInformationProcess(handle, 0, C.byref(basic), C.sizeof(basic), C.byref(returned)) != 0:
                raise Failed("Cannot verify reported game parent")
            if basic.pid != pid or basic.parent != launcher.pid:
                raise Failed("Reported game is not a child of this owned launcher")
            if self.creation(handle) < self.creation(int(launcher._handle)):
                raise Failed("Reported game predates its owned launcher")
            buffer, size = C.create_unicode_buffer(32768), W.DWORD(32768)
            if not self.kernel.QueryFullProcessImageNameW(handle, 0, buffer, C.byref(size)):
                raise Failed("Cannot verify owned game executable path")
            if os.path.normcase(os.path.abspath(buffer.value)) != os.path.normcase(os.path.abspath(expected)):
                raise Failed("Reported game image is outside this seat's exact isolated copy")
            return {"pid": pid, "handle": handle}
        except BaseException:
            self.kernel.CloseHandle(handle)
            raise

    def exit(self, game):
        state = self.kernel.WaitForSingleObject(game["handle"], 0)
        if state == 258:
            return None
        if state != 0:
            raise Failed("Owned game wait failed")
        code = W.DWORD()
        if not self.kernel.GetExitCodeProcess(game["handle"], C.byref(code)):
            raise Failed("Owned game exit query failed")
        return code.value

    def close_windows(self, games):
        active = {game["pid"]: game for game in games if self.exit(game) is None}
        windows = {pid: [] for pid in active}

        @self.callback
        def collect(window, _):
            pid = W.DWORD()
            self.user.GetWindowThreadProcessId(window, C.byref(pid))
            if pid.value in windows:
                windows[pid.value].append(window)
            return True

        if not self.user.EnumWindows(collect, 0):
            raise Failed("Cannot enumerate owned game windows")
        sent = {}
        # Collect first, then post both seats' messages in the same short pass.
        for pid, handles in windows.items():
            sent[pid] = 0
            for window in handles:
                if self.exit(active[pid]) is None and self.user.PostMessageW(window, 0x10, 0, 0):
                    sent[pid] += 1
        return sent


def run(args):
    started = time.monotonic()
    deadline, work_end = started + args.seconds, started + args.seconds - 12
    name = args.name or datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:10]
    no_reparse(HERE)
    results = HERE / "play-results"
    if not results.exists():
        results.mkdir()
    no_reparse(results)
    result = no_reparse(results / name, new=True)
    result.mkdir()
    boxes = [ROOT / ("run-play-lifecycle-" + name + "-" + seat) for seat in SEATS]
    records = [ROOT / "compatibility" / ("qa-play-lifecycle-" + name + "-" + seat + ".json") for seat in SEATS]
    launcher_logs = [result / (seat + "-launcher.log") for seat in SEATS]
    summary = {"schema": "coop-fof-real-play-lifecycle-v1", "result": "FAIL", "name": name,
        "scope": "Finite idle real-input-path lifecycle on one PC over loopback. No Steam, physical-input usability, campaign or WAN claim.",
        "seconds_limit": args.seconds, "required_common_frames": args.frames, "common_frames": 0,
        "one_sided_stop": args.one_sided_stop or args.stop_at_overwrite, "log_stride": 120 if args.quiet else 1, "logged_common_samples": 0,
        "preflight": [], "game_pids": [None, None], "game_exitcodes": [None, None],
        "launcher_returncodes": [None, None], "close_requests": {}, "forced_cleanup": [], "errors": []}
    if args.tutorial_ui:
        summary["scope"] = "First retail mission tutorial ownership regression, native campaign bot over loopback; no physical-input or WAN claim."
        summary["tutorial_ui"] = {"complete": False, "ticks": []}
    if args.live_join:
        summary["scope"] = "Native in-mission reconnect preservation over loopback; requires matching host/replacement handoff markers and absolute-tick state suffix."
        summary["live_join"] = {"complete_tick": None, "host_tick_continued": False}
    slot_test = args.slot_fixture is not None
    negative_slot = slot_test and (args.overwrite_no or len(args.seed_peer_slots) == 3 or args.stop_at_overwrite)
    if slot_test:
        summary["scope"] = "Native save-slot choice regression in generated QA profiles over loopback; not save-write attestation or a user-save test."
        summary["slot_fixture"] = {"complete": False}
        if negative_slot:
            summary["required_common_frames"] = 0
    owned, launchers, streams, games = [], [], [], [None, None]
    api, lane = None, None
    events = (result / "events.jsonl").open("x", encoding="utf-8", buffering=1)

    def event(kind, **fields):
        record = {"event": kind, "elapsed": round(time.monotonic() - started, 3), **fields}
        value = json.dumps(record, sort_keys=True)
        events.write(value + "\n")
        print(value, flush=True)

    proxy = None

    def popen(command, log, env, label):
        stream = log.open("xb")
        streams.append(stream)
        process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=stream, stderr=subprocess.STDOUT,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        owned.append((label, process))
        return process

    try:
        if os.name != "nt":
            raise Failed("Windows is required")
        lane_source = no_reparse(ROOT / "qa" / "game_lane.py")
        lane_spec = importlib.util.spec_from_file_location("coop_game_lane", lane_source)
        lane_module = importlib.util.module_from_spec(lane_spec)
        lane_spec.loader.exec_module(lane_module)
        api = Win32()
        for path in (*boxes, *records):
            no_reparse(path, new=True)
        for path in (ROOT / "probe" / "native-launcher.exe", ROOT / "probe" / "native-probe.dll",
                     ROOT / "compatibility" / "preflight.py", ROOT / "compatibility" / "baseline.json"):
            no_reparse(path)
        if hashlib.sha256((ROOT / "compatibility" / "baseline.json").read_bytes()).hexdigest() != BASELINE:
            raise Failed("Unpinned compatibility baseline")
        clean = {k: v for k, v in os.environ.items() if not k.upper().startswith("COOP_PROBE_")}
        for i, language in enumerate(LANGUAGES):
            process = popen([sys.executable, str(ROOT / "compatibility" / "preflight.py"), "verify",
                "--game-dir", str(ROOT / "test" / language.lower()), "--output", str(records[i])],
                result / (SEATS[i] + "-preflight.log"), clean, SEATS[i] + "-preflight")
            last_report = 0
            while process.poll() is None:
                now = time.monotonic()
                if now >= work_end:
                    raise Failed("Global deadline during full content preflight")
                if now - last_report >= 10:
                    event("PREFLIGHT_WAIT", seat=SEATS[i])
                    last_report = now
                time.sleep(0.1)
            if process.returncode != 0:
                raise Failed(SEATS[i] + ": full content preflight rejected")
            no_reparse(records[i])
            verified = json.loads(records[i].read_text(encoding="utf-8"))
            if (verified.get("schema") != "coop-fof-preflight-v1" or verified.get("status") != "pass"
                    or verified.get("language") != language or verified.get("critical_digest") != CRITICAL
                    or verified.get("critical_files") != 1257 or verified.get("locale_files") != 34):
                raise Failed(SEATS[i] + ": invalid fresh preflight record")
            summary["preflight"].append(verified)
            event("PREFLIGHT_PASS", seat=SEATS[i], language=language)
        lane = lane_module.GameLane(args.seconds).acquire()
        summary["gameslot"] = {"owner": lane.owner, "slots": lane.slots, "ttl_seconds": lane.ttl}
        host_profile = boxes[0].parent / (boxes[0].name + "-profile")
        slot_profiles = [host_profile, boxes[1].parent / (boxes[1].name + "-profile")]
        seed_inventories = [{}, {}]
        if slot_test:
            seed_data = load_native_fixture_seed(args.seed_save) if args.seed_save else None
            for i, selected in enumerate((args.seed_host_slots, args.seed_peer_slots)):
                seed_inventories[i] = seed_rows(slot_profiles[i], seed_data, selected)
            summary["seed_inventories"] = seed_inventories
            event("QA_SLOT_PROFILES_SEEDED", host_rows=args.seed_host_slots, peer_rows=args.seed_peer_slots)
        elif args.seed_save:
            # Start-together host with an existing campaign: its slot list shows a used slot, the partner's is empty.
            saves = host_profile / "Documents" / "Activision" / "Spider-Man FoF" / "Saved Games"
            saves.mkdir(parents=True)
            (saves / "SavedGame1.SFOFSaveGame").write_bytes(Path(args.seed_save).read_bytes())
            event("HOST_PROFILE_SEEDED", slots=[1])
        # Fake internet (qa/latency_proxy.py): the joiner connects through a delaying relay on port 49376.
        peer_port = "49375"
        if args.net_delay_ms is not None:
            proxy_log = result / "latency-proxy.jsonl"
            proxy_out = (result / "latency-proxy.log").open("w", encoding="utf-8")
            streams.append(proxy_out)
            proxy = subprocess.Popen([sys.executable, "-I", str(ROOT / "qa" / "latency_proxy.py"), "--listen", "49376", "--target", "49375",
                                      "--delay-ms", str(args.net_delay_ms), "--jitter-ms", str(args.net_jitter_ms),
                                      "--stall-every", str(args.net_stall_every), "--stall-ms", str(args.net_stall_ms), "--log", str(proxy_log)],
                                     cwd=ROOT, stdout=proxy_out, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
            peer_port = "49376"
            event("FAKE_INTERNET", one_way_delay_ms=args.net_delay_ms, jitter_ms=args.net_jitter_ms, stall_every_s=args.net_stall_every, stall_ms=args.net_stall_ms)
        for i, seat in enumerate(SEATS):
            process = popen([str(ROOT / "probe" / "native-launcher.exe"), "--play",
                str(ROOT / "test" / LANGUAGES[i].lower() / "Game.exe"), str(boxes[i])],
                launcher_logs[i], environment(os.environ, seat, not args.quiet, args.campaign, args.deferred_connect > 0 or args.join_at_hub,
                            slot_profiles[i] if slot_test else host_profile if (args.join_at_hub or args.together) and seat == "host" else None, args.hub_spawn,
                            args.skip_cutscenes == "both" or (args.skip_cutscenes == "host" and seat == "host"),
                            not args.no_auto_together, "49375" if seat == "host" else peer_port, args.tutorial_ui, args.slot_fixture, args.overwrite_no,
                            live_join=args.live_join,
                            wait_link=args.live_first_join and seat == "peer"), seat + "-launcher")
            launchers.append(process)
            lane.register_launcher(process)
        last_report = 0
        frozen_since, connected = None, False
        rejoin_stage = 0  # 0 joined once, 1 peer closed, 2 replacement peer started, 3 rejoined
        while True:
            outputs, parsed = [], []
            for i, seat in enumerate(SEATS):
                launcher_text = text(launcher_logs[i])
                matches = HOOK.findall(launcher_text)
                if len(matches) > 1:
                    raise Failed(seat + ": duplicate owned HOOK_READY")
                if matches and games[i] is None:
                    games[i] = api.capture(int(matches[0][0]), launchers[i], ROOT / "test" / LANGUAGES[i].lower() / "Game.exe")
                    summary["game_pids"][i] = games[i]["pid"]
                    event("OWNED_GAME_CAPTURED", seat=seat)
                probe_text = text(boxes[i] / "probe.log")
                if FAILURE.search(probe_text + launcher_text):
                    bad = FAILURE.search(probe_text + launcher_text)
                    source = probe_text + launcher_text
                    raise Failed(seat + ": " + source[bad.start():].splitlines()[0][:240])
                # --campaign deliberately uses the campaign menu/hub bot; nothing else may be a fixture.
                checked = probe_text.replace("CAMPAIGN CONFIG enabled=1 fixture=1 research_autotest=0", "") if args.campaign else probe_text
                if FIXTURE.search(checked) or "fixture=1" in checked:
                    raise Failed(seat + ": unexpected input/test fixture marker in real-play QA")
                dropped = (args.rejoin or args.rejoin_in_mission) and seat == "peer" and rejoin_stage == 1
                if launchers[i].poll() is not None and not dropped:
                    raise Failed(seat + ": launcher exited before requested close")
                if games[i] is not None and api.exit(games[i]) is not None and not dropped:
                    raise Failed(seat + ": game exited before requested close")
                outputs.append(probe_text)
                stride = 120 if args.quiet else 1
                if args.live_join:
                    # The replacement can begin at the host's existing mission tick.
                    parsed.append(rows(probe_text, stride, allow_nonzero_start=i==1, allow_native_ai=True))
                else:
                    # For live join, parsing the entire host log also rejects a zero/reset tick after disconnect.
                    rows_text = (probe_text.rsplit("NET PARTNER_LOST", 1)[-1]
                                 if args.rejoin_in_mission and not args.live_join and
                                 i == 0 and rejoin_stage >= 2 else probe_text)
                    parsed.append(rows(rows_text, stride))
            if args.rejoin and rejoin_stage == 0 and connected and "CAMPAIGN ASSERT JOIN_COMPLETE" in outputs[1]:
                # Both in the hub after the first join: the joiner leaves; the host must keep playing alone.
                if api.close_windows([games[1]]).get(games[1]["pid"], 0) < 1:
                    raise Failed("peer: close request for the drop was not posted")
                rejoin_stage = 1
                event("PEER_DROPPED_AT_HUB")
            if args.rejoin_in_mission and rejoin_stage == 0 and connected and summary["common_frames"] >= 600 and all("CAMPAIGN ASSERT GAMEPLAY" in output for output in outputs):
                # Both playing a mission: the joiner leaves mid-mission; the host keeps playing alone in the mission.
                if api.close_windows([games[1]]).get(games[1]["pid"], 0) < 1:
                    raise Failed("peer: close request for the drop was not posted")
                rejoin_stage = 1
                event("PEER_DROPPED_IN_MISSION", common_frames=summary["common_frames"])
            if rejoin_stage == 1 and "NET PARTNER_LOST" in outputs[0] and launchers[1].poll() is not None:
                if launchers[1].returncode != 0:
                    raise Failed("peer: dropped launcher exit " + str(launchers[1].returncode))
                boxes[1] = ROOT / ("run-play-lifecycle-" + name + "-peer2")
                launcher_logs[1] = result / "peer2-launcher.log"
                games[1] = None
                launchers[1] = popen([str(ROOT / "probe" / "native-launcher.exe"), "--play",
                    str(ROOT / "test" / LANGUAGES[1].lower() / "Game.exe"), str(boxes[1])],
                    launcher_logs[1], environment(os.environ, "peer", not args.quiet, args.campaign, True, None,
                        skip_nis=args.skip_cutscenes == "both", auto_together=not args.no_auto_together,
                        net_port=peer_port, live_join=args.live_join), "peer2-launcher")
                lane.register_launcher(launchers[1])
                connected, rejoin_stage = False, 2
                event("HOST_CONTINUED_ALONE_REPLACEMENT_PEER_STARTED")
            host_solo_frames = max(parsed[0]) + 1 if parsed[0] else 0
            host_in_canonical_mission = bool(re.search(
                r"^CAMPAIGN ASSERT GAMEPLAY path=Tokyo/1_Level/1_Area area=\d+ flow=128 sidekick=\d+ prepared=1$",
                outputs[0], re.M))
            first_join_ready = (not args.live_first_join or
                                (host_solo_frames >= 600 and host_in_canonical_mission))
            if args.join_at_hub and not connected and all(games) and first_join_ready:
                # Both linked at once; the host bot plays New Game alone and the joiner waits at its main menu.
                for i, role in enumerate(("host", "join")):
                    temporary = boxes[i] / "connect.json.tmp"
                    temporary.write_text(json.dumps({"role": role}, separators=(",", ":")), encoding="utf-8")
                    os.replace(temporary, boxes[i] / "connect.json")
                connected = True
                if rejoin_stage == 2:
                    rejoin_stage = 3
                event("LIVE_FIRST_JOIN_ROLES_WRITTEN" if args.live_first_join else "JOIN_AT_HUB_ROLES_WRITTEN",
                      rejoin=rejoin_stage == 3, host_solo_frames=host_solo_frames if args.live_first_join else None)
            if args.deferred_connect and not connected:
                if any(re.search(r"^NET (?:opening|READY)", output, re.M) for output in outputs):
                    raise Failed("A deferred game connected before its role was written")
                if all("NET DEFERRED_WAIT" in output for output in outputs):
                    frozen_since = frozen_since or time.monotonic()
                    if time.monotonic() - frozen_since >= args.deferred_connect:
                        for i, role in enumerate(("host", "join")):
                            temporary = boxes[i] / "connect.json.tmp"
                            temporary.write_text(json.dumps({"role": role}, separators=(",", ":")), encoding="utf-8")
                            os.replace(temporary, boxes[i] / "connect.json")
                        connected = True
                        event("DEFERRED_CONNECT_WRITTEN", waited_seconds=round(time.monotonic() - frozen_since, 1))
            stride = 120 if args.quiet else 1
            if args.live_first_join and connected or args.live_join and rejoin_stage >= 2:
                complete_tick = live_join_complete_tick(
                    outputs[0], outputs[1], required=False, require_drop=not args.live_first_join)
                common = (compare_suffix(parsed[0], parsed[1], stride, complete_tick)
                          if complete_tick is not None else 0)
                if complete_tick is not None and summary["live_join"]["complete_tick"] is None:
                    summary["live_join"]["complete_tick"] = complete_tick
                    event("LIVE_JOIN_PRESERVED", tick=complete_tick)
            else:
                common = compare(*parsed, stride=stride)
            summary["logged_common_samples"] = len(set(parsed[0]) & set(parsed[1]))
            summary["common_frames"] = common
            if slot_test:
                summary["slot_fixture"] = (phase_stop_evidence(outputs, args.slot_fixture, (args.seed_host_slots, args.seed_peer_slots)) if args.stop_at_overwrite
                    else slot_evidence(outputs, args.slot_fixture, (args.seed_host_slots, args.seed_peer_slots), args.overwrite_no))
                if negative_slot and all(games) and summary["slot_fixture"]["complete"]:
                    if any(parsed):
                        raise Failed("Canceled save-slot choice unexpectedly simulated gameplay")
                    for i in range(2):
                        if save_inventory(slot_profiles[i]) != seed_inventories[i]:
                            raise Failed(SEATS[i] + ": canceled save-slot choice changed the save inventory")
                    event("NEGATIVE_SLOT_OUTCOME_CONFIRMED", outcome=summary["slot_fixture"]["outcome"])
                    break
            ready = [bool(re.search(r"^NET READY role=" + str(i + 1) + r"$", outputs[i], re.M)) for i in range(2)]
            if args.live_first_join:
                resumed = (connected and live_join_complete_tick(
                    outputs[0], outputs[1], required=False, require_drop=False) is not None)
            elif args.live_join:
                resumed = (rejoin_stage == 3 and
                           live_join_complete_tick(outputs[0], outputs[1], required=False,
                                                   require_drop=True) is not None)
            else:
                resumed = not args.rejoin_in_mission or (rejoin_stage == 3 and "MISSION_COMMIT" in outputs[1] and "resume=1" in outputs[1])
            tutorial_ready = True
            if args.tutorial_ui:
                summary["tutorial_ui"] = tutorial_ui_evidence(outputs)
                tutorial_ready = summary["tutorial_ui"]["complete"]
            if all(games) and all(ready) and common >= args.frames and (not (args.rejoin or args.rejoin_in_mission) or rejoin_stage == 3) and resumed and tutorial_ready and (not slot_test or summary["slot_fixture"]["complete"]):
                event("MATCHING_IDLE_THRESHOLD", common_frames=common)
                break
            now = time.monotonic()
            if now >= work_end:
                raise Failed("Global frame deadline: ready=" + str(ready) + ", samples=" + str(list(map(len, parsed)))
                    + ". Real foreground/pause behavior was preserved; no input fixture was enabled.")
            if now - last_report >= 10:
                event("REAL_PLAY_WAIT", ready=ready, sample_counts=list(map(len, parsed)), common_frames=common)
                last_report = now
            time.sleep(0.05)
        closing = games[:1] if args.one_sided_stop or args.stop_at_overwrite else games
        summary["close_requests"] = api.close_windows(closing)
        if any(summary["close_requests"].get(game["pid"], 0) < 1 for game in closing):
            raise Failed("Each selected owned game requires a successful WM_CLOSE post")
        event("HOST_ONLY_WM_CLOSE" if args.one_sided_stop or args.stop_at_overwrite else "BOTH_OWNED_GAMES_WM_CLOSE")
        close_end = min(deadline - 4, time.monotonic() + 18)
        while any(process.poll() is None for process in launchers) or any(api.exit(game) is None for game in games):
            if time.monotonic() >= close_end:
                raise Failed("Owned games/launchers did not both exit normally after WM_CLOSE")
            time.sleep(0.05)
        for i, seat in enumerate(SEATS):
            summary["launcher_returncodes"][i] = launchers[i].returncode
            summary["game_exitcodes"][i] = api.exit(games[i])
            exits = EXIT.findall(text(launcher_logs[i]))
            if exits != [(str(games[i]["pid"]), "0")] or launchers[i].returncode != 0 or summary["game_exitcodes"][i] != 0:
                raise Failed(seat + ": need matching PROCESS_EXIT=0, retained game exit0 and launcher exit0")
        final_logs = [text(box / "probe.log") for box in boxes]
        if args.rejoin_in_mission and not args.live_join:
            final_logs[0] = final_logs[0].rsplit("NET PARTNER_LOST", 1)[-1]  # the host's new session after the drop
        stride = 120 if args.quiet else 1
        if args.live_join:
            live_tick = live_join_complete_tick(final_logs[0], final_logs[1], required=True,
                                                require_drop=not args.live_first_join)
            host_full_rows = rows(final_logs[0], stride, allow_native_ai=True)
            if args.live_first_join:
                frozen = list(re.finditer(r"^LIVE_JOIN FROZEN(?: .*)?$", final_logs[0], re.M))
                if len(frozen) != 1:
                    raise Failed("Initial live join requires exactly one LIVE_JOIN FROZEN marker")
                host_after_join = final_logs[0][frozen[0].end():]
                host_after_rows = rows(host_after_join, stride, allow_nonzero_start=True, allow_native_ai=True)
                if not host_full_rows or not host_after_rows or max(host_full_rows) < live_tick:
                    raise Failed("Initial live-join host mission timeline did not continue through completion")
                host_suffix_rows = {tick: line for tick, line in host_full_rows.items() if tick >= live_tick}
            else:
                host_parts = final_logs[0].rsplit("NET PARTNER_LOST", 1)
                if len(host_parts) != 2:
                    raise Failed("Live-join host log lacks its disconnect boundary")
                host_before_rows = rows(host_parts[0], stride, allow_native_ai=True)
                host_after_rows = rows(host_parts[1], stride, allow_nonzero_start=True, allow_native_ai=True)
                if (not host_before_rows or not host_after_rows or
                        min(host_after_rows) <= max(host_before_rows) or
                        max(host_full_rows) < live_tick):
                    raise Failed("Host mission ticks did not continue through live join")
                host_suffix_rows = host_after_rows
            replacement_rows = rows(final_logs[1], stride, allow_nonzero_start=True, allow_native_ai=True)
            summary["live_join"] = {"complete_tick": live_tick, "host_tick_continued": True}
            summary["common_frames"] = compare_suffix(host_suffix_rows, replacement_rows, stride, live_tick)
        else:
            summary["common_frames"] = compare(*(rows(value, stride) for value in final_logs), stride=stride)
        if not negative_slot and summary["common_frames"] < args.frames:
            raise Failed("Final common sample count regressed")
        if slot_test:
            summary["slot_fixture"] = (phase_stop_evidence(final_logs, args.slot_fixture, (args.seed_host_slots, args.seed_peer_slots), complete=True) if args.stop_at_overwrite
                else slot_evidence(final_logs, args.slot_fixture, (args.seed_host_slots, args.seed_peer_slots), args.overwrite_no, complete=True))
            for i in range(2):
                actual = save_inventory(slot_profiles[i])
                if negative_slot:
                    if actual != seed_inventories[i] or rows(final_logs[i], 120 if args.quiet else 1):
                        raise Failed(SEATS[i] + ": canceled save choice changed saves or entered gameplay")
                else:
                    target = summary["slot_fixture"]["host_slot" if i == 0 else "peer_slot"]
                    name_to_write = "SavedGame" + str(target + 1) + ".SFOFSaveGame"
                    for name, record in seed_inventories[i].items():
                        if name != name_to_write and actual.get(name) != record:
                            raise Failed(SEATS[i] + ": unrelated seeded slot changed")
                    if set(actual) != set(seed_inventories[i]) | {name_to_write}:
                        raise Failed(SEATS[i] + ": save outcome has unexpected/missing files")
                    folder = slot_profiles[i] / "Documents" / "Activision" / "Spider-Man FoF" / "Saved Games"
                    if native_save_slot((folder / name_to_write).read_bytes())[0] != target:
                        raise Failed(SEATS[i] + ": native save filename/payload index mismatch")
            summary["slot_file_outcome_verified"] = True
        if args.tutorial_ui:
            summary["tutorial_ui"] = tutorial_ui_evidence(final_logs, complete=True)
        if args.one_sided_stop or args.stop_at_overwrite:
            if "NET user stop requested sent=1" not in final_logs[0] or "NET peer ended session normally" not in final_logs[1]:
                raise Failed("One-sided stop requires explicit host stop and peer normal-close evidence")
            summary["one_sided_stop_confirmed"] = True
        if args.quiet:
            # game-frame-40000N.bmp: the black-screen recorder's picture after a window activation (up to five per
            # session, by design also in normal play); a focus change on the desktop is not a quiet-mode violation.
            if any(p for box in boxes for p in box.glob("game-frame-*.bmp") if not p.name.startswith("game-frame-40000")):
                raise Failed("Quiet play unexpectedly captured diagnostic game images")
            summary["quiet_no_capture"] = True
        # Shutdown failures remain failures too; no filtering of peer-close errors.
        for i, value in enumerate(final_logs):
            bad = FAILURE.search(value + text(launcher_logs[i]))
            if bad:
                raise Failed(SEATS[i] + ": failure/exception recorded during normal shutdown; inspect full logs")
        summary["result"] = "PASS"
    except (RuntimeError, OSError, ValueError, TypeError, KeyError, subprocess.SubprocessError) as exc:
        summary["errors"].append(str(exc))
        event("LIFECYCLE_FAIL", reason=str(exc))
    except KeyboardInterrupt:
        summary["errors"].append("Interrupted; cleaning up only this run's owned handles")
    finally:
        if proxy is not None:
            try:
                proxy.terminate()
                proxy.wait(timeout=5)
            except (OSError, subprocess.TimeoutExpired):
                pass
        if api is not None:
            try:
                if any(game is not None and api.exit(game) is None for game in games):
                    api.close_windows([game for game in games if game is not None])
            except (Failed, OSError) as exc:
                summary["errors"].append("Cleanup close: " + str(exc))
        grace = min(deadline - 2, time.monotonic() + 3)
        while any(process.poll() is None for _, process in owned) and time.monotonic() < grace:
            time.sleep(0.05)
        for label, process in owned:
            if process.poll() is None:
                summary["forced_cleanup"].append(label)
                try:
                    process.terminate()  # Only owned Popen handles; native launcher closes its game job.
                except OSError:
                    pass
        for label, process in owned:
            try:
                process.wait(timeout=max(0.0, min(1.0, deadline - time.monotonic())))
            except subprocess.TimeoutExpired:
                try:
                    process.kill()
                except OSError:
                    pass
                summary["errors"].append(label + ": owned handle cleanup exceeded deadline")
        for i, process in enumerate(launchers):
            summary["launcher_returncodes"][i] = process.poll()
        children_stopped = True
        for i, game in enumerate(games):
            if game is not None:
                try:
                    summary["game_exitcodes"][i] = api.exit(game)
                    children_stopped = children_stopped and summary["game_exitcodes"][i] is not None
                finally:
                    api.kernel.CloseHandle(game["handle"])
        if lane is not None:
            try:
                lane.release(children_stopped=children_stopped and all(p.poll() is not None for p in launchers))
            except RuntimeError as exc:
                summary["errors"].append(str(exc))
        for stream in streams:
            stream.close()
        if summary["errors"] or summary["forced_cleanup"]:
            summary["result"] = "FAIL"
        summary["elapsed_seconds"] = round(time.monotonic() - started, 3)
        summary["sandboxes"] = [box.name for box in boxes]
        summary["preflight_records"] = [record.name for record in records]
        with (result / "summary.json").open("x", encoding="utf-8") as output:
            json.dump(summary, output, indent=2)
            output.write("\n")
        event("LIFECYCLE_RESULT", result=summary["result"], common_frames=summary["common_frames"], result_directory=result.name)
        events.close()
    return 0 if summary["result"] == "PASS" else 1


def selftest():
    row = "NET state tick=0 rng=12345678 pos0=00000000,00000000,00000000 pos1=00000000,00000000,00000000 slots=0,1 input=00000000,00000000 world=2,12345678 progress=11112222 area=33334444"
    assert compare(rows(row), rows(row)) == 1
    ai_row = row.replace("slots=0,1", "slots=0,-1")
    assert rows(ai_row, allow_native_ai=True)[0] == ai_row
    try:
        rows(ai_row)
    except Failed:
        pass
    else:
        raise AssertionError("Default parser accepted native AI")
    try:
        compare_suffix(rows(ai_row, allow_native_ai=True), rows(ai_row, allow_native_ai=True), minimum_tick=0)
    except Failed:
        pass
    else:
        raise AssertionError("Post-admission parser accepted native AI")
    for invalid in (row + "\n" + row, row.replace("tick=0", "tick=2"), row.replace("slots=0,1", "slots=1,1"),
                    row.replace("00000000,00000000,00000000", "7fc00000,00000000,00000000", 1),
                    row.replace(" progress=11112222 area=33334444", "")):
        try:
            rows(invalid)
        except Failed:
            pass
        else:
            raise AssertionError("Invalid real-play row accepted")
    try:
        compare(rows(row), rows(row.replace("progress=11112222", "progress=55556666")))
    except Failed:
        pass
    else:
        raise AssertionError("Divergence accepted")
    env = environment({"COOP_PROBE_TRANSITION_FIXTURE": "1", "coop_probe_inputbot": "1", "PATH": "own"}, "peer")
    assert "COOP_PROBE_TRANSITION_FIXTURE" not in env and "coop_probe_inputbot" not in env
    assert env["COOP_PROBE_HUMAN_FIXTURE"] == "0" and env["COOP_PROBE_NET_FRAMES"] == "0"
    assert env["COOP_PROBE_NET_MODE"] == "join" and env["COOP_PROBE_INPUTBOT"] == "0"
    sparse = "\n".join(row.replace("tick=0", "tick=" + str(tick)) for tick in (0, 120, 240, 360))
    assert compare(rows(sparse, 120), rows(sparse, 120), 120) == 361
    absolute = "\n".join(row.replace("tick=0", "tick=" + str(tick)) for tick in (600, 720))
    assert list(rows(absolute, 120, allow_nonzero_start=True)) == [600, 720]
    assert compare_suffix({600: row, 720: row}, {600: row, 720: row}, 120, 600) == 121
    try:
        rows(absolute + "\n" + row, 120, allow_nonzero_start=True)
    except Failed:
        pass
    else:
        raise AssertionError("Reset tick after an absolute post-rejoin sequence accepted")
    try:
        rows(sparse.replace("tick=240", "tick=360"), 120)
    except Failed:
        pass
    else:
        raise AssertionError("Missing quiet sample accepted")
    assert environment({}, "host", False)["COOP_PROBE_DIAGNOSTICS"] == "0"
    campaign = environment({}, "peer", True, True)
    assert campaign["COOP_PROBE_CAMPAIGN"] == "1" and campaign["COOP_PROBE_CAMPAIGN_FIXTURE"] == "1"
    assert "COOP_PROBE_AUTOTEST" not in campaign and "COOP_PROBE_AREA" not in campaign
    assert environment({}, "host", True, True, True)["COOP_PROBE_NET_MODE"] == "deferred"
    live_host = environment({}, "host", campaign=True, live_join=True)
    live_replacement = environment({}, "peer", campaign=True, deferred=True, live_join=True)
    first_host = environment({}, "host", campaign=True, deferred=True, live_join=True)
    first_peer = environment({}, "peer", campaign=True, deferred=True, live_join=True, wait_link=True)
    assert live_host["COOP_PROBE_LIVE_JOIN_TEST"] == "1"
    assert live_replacement["COOP_PROBE_LIVE_JOIN_TEST"] == "1"
    assert first_host["COOP_PROBE_LIVE_JOIN_TEST"] == "1" and "COOP_PROBE_HOST_WAIT_LINK" not in first_host
    assert first_peer["COOP_PROBE_LIVE_JOIN_TEST"] == "1" and first_peer["COOP_PROBE_HOST_WAIT_LINK"] == "1"
    valid_live_host = "NET state tick=0\nNET PARTNER_LOST\nLIVE_JOIN COMPLETE tick=600 preserved=1\n"
    valid_live_peer = "LIVE_JOIN COMPLETE tick=600 preserved=1\n"
    assert live_join_complete_tick(valid_live_host, valid_live_peer) == 600
    initial_live_host = ("LIVE_JOIN FROZEN tick=600\n"
                         "NET state tick=600\nLIVE_JOIN COMPLETE tick=601 preserved=1\n")
    initial_live_peer = "LIVE_JOIN COMPLETE tick=601 preserved=1\n"
    assert live_join_complete_tick(initial_live_host, initial_live_peer, require_drop=False) == 601
    for host_log, peer_log in (
            (valid_live_host.replace("tick=600", "tick=0"), valid_live_peer.replace("tick=600", "tick=0")),
            (valid_live_host, valid_live_peer.replace("tick=600", "tick=601")),
            (valid_live_host + "JOIN_IN_MISSION resume=1\n", valid_live_peer),
            (initial_live_host.replace("LIVE_JOIN FROZEN tick=600\n", ""),
             initial_live_peer),
            (initial_live_host + "CAMPAIGN ASSERT MISSION_COMMIT resume=1\n", initial_live_peer)):
        try:
            initial = "LIVE_JOIN FROZEN" in host_log
            live_join_complete_tick(host_log, peer_log, require_drop=not initial)
        except Failed:
            pass
        else:
            raise AssertionError("Invalid live-join preservation evidence accepted")
    tutorial = environment({"COOP_PROBE_LEVEL_END_TEST": "1"}, "peer", campaign=True, tutorial_ui=True)
    assert tutorial["COOP_PROBE_TUTORIAL_UI_TEST"] == "1" and "COOP_PROBE_LEVEL_END_TEST" not in tutorial
    prefix = ("CAMPAIGN ASSERT NEW_GAME operation=0 selection=0\n"
              "CAMPAIGN ASSERT GAMEPLAY path=Tokyo/1_Level/1_Area area=0 flow=128 sidekick=1 prepared=1\n")
    markers = ("TUTORIAL_UI OPEN tick=808 area=Tokyo/1_Level/1_Area scene=14\n"
               "TUTORIAL_UI PEER_IGNORED tick=840 scene=14 attempts=3 host=0 peer=0\n"
               "TUTORIAL_UI HOST_ACCEPT tick=940 scene=14 host=1 peer=0\n"
               "TUTORIAL_UI CLOSED tick=941 scene=14 host=1 peer=0\n")
    good = prefix + markers
    assert tutorial_ui_evidence([good, good], True) == {"complete": True, "ticks": [808, 840, 940, 941]}
    assert not tutorial_ui_evidence([good, prefix + markers.splitlines()[0] + "\n"])["complete"]
    for bad in (prefix, good + markers.splitlines()[-1] + "\n", good.replace("peer=0", "peer=1", 1),
                good.replace("attempts=3", "attempts=2"), good.replace("tick=940", "tick=900"),
                good.replace("tick=941", "tick=940"), good.replace("Tokyo/1_Level/1_Area", "Tokyo/1_Level/4_Area"),
                good.replace("CAMPAIGN ASSERT NEW_GAME", "OTHER ASSERT NEW_GAME"), good + "LEVEL_END test\n",
                prefix + markers.replace("tick=941", "tick=942")):
        try:
            tutorial_ui_evidence([good, bad], True)
        except Failed:
            pass
        else:
            raise AssertionError("Invalid tutorial ownership evidence accepted")
    assert slot_rows("2,0") == (0, 2) and slot_rows("none") == ()
    for bad in ("", "0,0", "3", "1, 2", "-1", "None"):
        try:
            slot_rows(bad)
        except argparse.ArgumentTypeError:
            pass
        else:
            raise AssertionError("Invalid seed row list accepted")
    fixture = environment({}, "host", campaign=True, slot_fixture=2, overwrite_no=True)
    assert fixture["COOP_PROBE_SLOT_FIXTURE"] == "2" and fixture["COOP_PROBE_OVERWRITE_FIXTURE_NO"] == "1"
    seed = bytearray(8232 + 2 + 512)
    seed[:4] = b"RGMH"
    struct.pack_into("<II", seed, 4, 1, 8232)
    struct.pack_into("<I", seed, 20, 2)
    seed[123] = 0xa4
    for row in range(3):
        moved = retarget_seed(seed, row)
        assert native_save_slot(moved) == (row, 8234)
        assert moved[:8234] == seed[:8234] and moved[8238:] == seed[8238:]
    bad_seeds = [bytes(seed[:-1]), bytes(seed) + b"x", b"FAIL" + bytes(seed[4:])]
    for offset, number in ((4, 2), (8, 1), (20, 0xffffffff), (8234, 3)):
        bad = bytearray(seed)
        struct.pack_into("<I", bad, offset, number)
        bad_seeds.append(bad)
    for bad in bad_seeds:
        try:
            native_save_slot(bad)
        except Failed:
            pass
        else:
            raise AssertionError("Malformed native seed accepted")
    def used_log(role, answer="yes", profile=True):
        value = "CAMPAIGN OVERWRITE_WAIT host_slot=0 peer_slot=0 role=" + str(role) + "\n"
        if role == 1:
            value += "CAMPAIGN OVERWRITE_PROMPT source=STATE_OVERWRITE_GAME_PROMPT configured=1 slot=0\n"
        value += "CAMPAIGN OVERWRITE_CHOICE answer=" + answer + " role=" + str(role) + " own_slot=0\n"
        if profile:
            value += "CAMPAIGN ASSERT PROFILE slot=0 native_init=1 operation=0\n"
        return value
    yes = [used_log(1), used_log(2)]
    no = [used_log(1, "no", False), used_log(2, "no", False)]
    empty = ["CAMPAIGN ASSERT PROFILE slot=2 native_init=1 operation=0\n", "CAMPAIGN ASSERT PROFILE slot=1 native_init=1 operation=0\n"]
    full = ["CAMPAIGN SLOT_REFUSED reason=partner_has_no_empty_slots saves_unchanged=1\n"] * 2
    assert slot_evidence(yes, 0, ((0,), ()), complete=True)["outcome"] == "yes"
    assert slot_evidence(no, 0, ((0,), ()), True, True)["outcome"] == "no"
    assert slot_evidence(empty, 2, ((), (0,)), complete=True)["peer_slot"] == 1
    assert slot_evidence(full, 0, ((0,), (0, 1, 2)), complete=True)["outcome"] == "full"
    bad_outcomes = [
        ([yes[0], ""], 0, ((0,), ()), False),
        ([yes[0].replace("configured=1", "configured=0"), yes[1]], 0, ((0,), ()), False),
        ([yes[0], yes[1].replace("answer=yes", "answer=no")], 0, ((0,), ()), False),
        ([yes[0], yes[1].replace("own_slot=0", "own_slot=1")], 0, ((0,), ()), False),
        ([yes[0], yes[1].replace("slot=0 native_init", "slot=1 native_init")], 0, ((0,), ()), False),
        ([yes[0] + yes[0], yes[1]], 0, ((0,), ()), False),
        ([no[0] + "CAMPAIGN ASSERT PROFILE slot=0 native_init=1 operation=0\n", no[1]], 0, ((0,), ()), True),
        ([no[0], no[1].replace("answer=no", "answer=yes")], 0, ((0,), ()), True),
        ([full[0], ""], 0, ((0,), (0, 1, 2)), False),
        ([full[0], full[1] + "CAMPAIGN ASSERT HUB path=x\n"], 0, ((0,), (0, 1, 2)), False),
        ([empty[0] + "CAMPAIGN OVERWRITE_WAIT\n", empty[1]], 2, ((), (0,)), False),
    ]
    for logs, target, occupied, answer_no in bad_outcomes:
        try:
            slot_evidence(logs, target, occupied, answer_no, complete=True)
        except Failed:
            pass
        else:
            raise AssertionError("Invalid save-slot outcome accepted")
    held = ["CAMPAIGN OVERWRITE_WAIT host_slot=0 peer_slot=0 role=1\n"
            "CAMPAIGN OVERWRITE_PROMPT source=STATE_OVERWRITE_GAME_PROMPT configured=1 slot=0\n",
            "CAMPAIGN OVERWRITE_WAIT host_slot=0 peer_slot=0 role=2\n"]
    assert phase_stop_evidence(held, 0, ((0,), ()))["complete"]
    peer_stop = "NET peer ended session normally phase=12; deferred owned WM_CLOSE\n"
    peer_post = "NET peer phase stop WM_CLOSE posted; native callbacks unwind before teardown\n"
    stopped = [held[0] + "NET user stop requested sent=1\n", held[1] + peer_stop + peer_post]
    assert phase_stop_evidence(stopped, 0, ((0,), ()), True)["outcome"] == "phase_stop"
    bad_stops = [
        [held[0], stopped[1]], [stopped[0], held[1] + peer_stop],
        [stopped[0], held[1] + peer_post + peer_stop],
        [stopped[0], stopped[1].replace("phase=12", "frame=12")],
        [stopped[0], stopped[1] + peer_stop],
        [stopped[0], stopped[1] + "CAMPAIGN ASSERT PROFILE slot=0 native_init=1 operation=0\n"],
        [stopped[0] + "CAMPAIGN OVERWRITE_CHOICE answer=yes role=1 own_slot=0\n", stopped[1]],
        [stopped[0], stopped[1] + "CAMPAIGN SAVE_ACK ready=1\n"],
    ]
    for logs in bad_stops:
        try:
            phase_stop_evidence(logs, 0, ((0,), ()), True)
        except Failed:
            pass
        else:
            raise AssertionError("Invalid phase STOP evidence accepted")
    # Synthetic parser data is never launched in the game. Only isolated temporary
    # folders are created; native test runs require their seed from a retained run-*.
    with tempfile.TemporaryDirectory(prefix="run-slot-selftest-", dir=ROOT) as temporary:
        temporary = Path(temporary).resolve()
        assert temporary.is_relative_to(ROOT.resolve())
        profile = temporary / "host-profile"
        before = seed_rows(profile, seed, (0, 2))
        assert len(before) == 2 and save_inventory(profile) == before
        folder = profile / "Documents" / "Activision" / "Spider-Man FoF" / "Saved Games"
        first = folder / "SavedGame1.SFOFSaveGame"
        assert load_native_fixture_seed(first) == bytes(seed)
        assert native_save_slot((folder / "SavedGame3.SFOFSaveGame").read_bytes())[0] == 2
        first.write_bytes(retarget_seed(seed, 1))
        assert save_inventory(profile) != before
        try:
            load_native_fixture_seed(first)
        except Failed:
            pass
        else:
            raise AssertionError("Seed filename/index contradiction accepted")
        first.write_bytes(bytes(seed))
        from contextlib import redirect_stderr
        import io
        from unittest.mock import patch
        seen = []
        def pretend_run(options):
            seen.append(options)
            return 0
        clean_env = {key: value for key, value in os.environ.items() if not key.startswith("FOF_RESEARCH_")}
        base = ["--campaign", "--together"]
        with patch.dict(globals(), {"run": pretend_run}), patch.dict(os.environ, clean_env, clear=True):
            assert main(base + ["--slot-fixture", "2"]) == 0
            assert seen[-1].seed_host_slots == () and seen[-1].seed_peer_slots == ()
            used = base + ["--slot-fixture", "0", "--seed-save", str(first)]
            assert main(used) == 0 and seen[-1].seed_host_slots == (0,)
            assert main(used + ["--overwrite-no"]) == 0 and seen[-1].overwrite_no
            assert main(used + ["--stop-at-overwrite"]) == 0 and seen[-1].stop_at_overwrite
            assert main(used + ["--seed-peer-slots", "0,1,2"]) == 0 and seen[-1].seed_peer_slots == (0, 1, 2)
            live_base = ["--campaign", "--join-at-hub", "--rejoin-in-mission", "--live-join"]
            assert main(live_base) == 0 and seen[-1].live_join
            first_join_args = ["--campaign", "--join-at-hub", "--live-first-join"]
            assert main(first_join_args) == 0 and seen[-1].live_first_join and seen[-1].live_join
            invalid = [
                ["--slot-fixture", "0"], base + ["--overwrite-no"],
                base + ["--slot-fixture", "0", "--seed-host-slots", "0"],
                base + ["--slot-fixture", "2", "--seed-save", str(first), "--overwrite-no"],
                used + ["--overwrite-no", "--seed-peer-slots", "0,1,2"],
                base + ["--slot-fixture", "3"],
                base + ["--slot-fixture", "0", "--tutorial-ui", "--frames", "600"],
                used + ["--stop-at-overwrite", "--overwrite-no"],
                used + ["--stop-at-overwrite", "--seed-peer-slots", "0,1,2"],
                ["--live-join"], ["--campaign", "--live-join"],
                ["--campaign", "--join-at-hub", "--live-join"],
                ["--live-first-join"], ["--campaign", "--live-first-join"],
                ["--campaign", "--join-at-hub", "--live-first-join", "--rejoin"],
                ["--campaign", "--join-at-hub", "--live-first-join", "--rejoin-in-mission"],
            ]
            for command in invalid:
                before_calls = len(seen)
                with redirect_stderr(io.StringIO()):
                    try:
                        main(command)
                    except SystemExit as exc:
                        assert exc.code == 2
                    else:
                        raise AssertionError("Unsafe slot CLI combination accepted")
                assert len(seen) == before_calls
            with patch.dict(os.environ, {"FOF_RESEARCH_LEVEL_END_TEST": "1"}), redirect_stderr(io.StringIO()):
                try:
                    main(used)
                except SystemExit as exc:
                    assert exc.code == 2
                else:
                    raise AssertionError("Inherited alternate route accepted for slot regression")
    print("OWN_PLAY_LIFECYCLE_SELFTEST_PASS rows/environment/quiet/live-join+first-join+absolute-ticks/tutorial+10negative/slot4positive+11negative/phase-stop+8negative/seed7negative+retarget+inventory/CLI5positive+10negative")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name")
    parser.add_argument("--seconds", type=int, default=240)
    parser.add_argument("--frames", type=int, default=300)
    parser.add_argument("--one-sided-stop", action="store_true", help="Close only the owned host; peer must receive explicit normal-stop and exit0")
    parser.add_argument("--quiet", action="store_true", help="Verify sparse logging every120 ticks and no diagnostic images; live checksums still run every tick")
    parser.add_argument("--net-delay-ms", type=float, help="Fake internet: one-way delay between the seats (round trip = twice)")
    parser.add_argument("--net-jitter-ms", type=float, default=0.0)
    parser.add_argument("--net-stall-every", type=float, default=0.0, help="Fake internet: seconds between stalls")
    parser.add_argument("--net-stall-ms", type=float, default=0.0)
    parser.add_argument("--no-auto-together", action="store_true",
                        help="Keep the plain join-at-hub route even when the host is still in the main menu")
    parser.add_argument("--join-at-hub", action="store_true",
                        help="With --campaign: host plays New Game alone to the hub, then the joiner catches up and both play on")
    parser.add_argument("--rejoin-in-mission", action="store_true",
                        help="With --join-at-hub: both play into a mission, the joiner closes its game there; a new joiner joins while the host is still in the mission and both resume at the host's area")
    parser.add_argument("--live-join", action="store_true",
                        help="With --campaign --join-at-hub --rejoin-in-mission: require a preserved in-mission join without returning the host to the Helicarrier")
    parser.add_argument("--live-first-join", action="store_true",
                        help="With --campaign --join-at-hub: host starts alone in the mission, then waits for the first peer to join without reloading the host area")
    parser.add_argument("--rejoin", action="store_true",
                        help="With --join-at-hub: after the first join the joiner closes its game; the host keeps playing and a new joiner rejoins at the hub")
    parser.add_argument("--skip-cutscenes", nargs="?", const="both", choices=("both", "host"), default=None,
                        help="Campaign bot skips movies/cutscenes the game lets players skip: on both seats (faster runs) or only the host (the peer watches; checks that the hub waits for it)")
    parser.add_argument("--hub-spawn", type=int, default=-1, metavar="INDEX",
                        help="Research: after the hub is reached, call the game's spawn-character native once more with INDEX (both seats)")
    parser.add_argument("--together", action="store_true",
                        help="With --campaign: both games start together from the main menu (host keeps saves in a profile)")
    parser.add_argument("--seed-save", metavar="FILE",
                        help="With --together: put this save in the host profile's slot 1 first")
    parser.add_argument("--slot-fixture", type=int, choices=(0, 1, 2), help="Research: host selects this zero-based New Game row")
    parser.add_argument("--overwrite-no", action="store_true", help="With --slot-fixture on a seeded used row: host answers No; require unchanged saves")
    parser.add_argument("--stop-at-overwrite", action="store_true", help="With a seeded used --slot-fixture: close only host while peer waits for overwrite answer; require deferred peer phase STOP and unchanged saves")
    parser.add_argument("--seed-host-slots", type=slot_rows, help="With --slot-fixture: zero-based rows copied from native --seed-save, or none")
    parser.add_argument("--seed-peer-slots", type=slot_rows, help="With --slot-fixture: peer rows seeded from native --seed-save, or none")
    parser.add_argument("--deferred-connect", type=int, default=0, metavar="SECONDS",
                        help="With --campaign: open both games first, keep their menus frozen for SECONDS, then hand out the roles")
    parser.add_argument("--campaign", action="store_true", help="Use the retail campaign route (bot drives host menus/hub) instead of the research scenario")
    parser.add_argument("--tutorial-ui", action="store_true", help="With --campaign --together: exercise the first-area hint with partner-only then host Accept; requires at least600 frames")
    parser.add_argument("--selftest", action="store_true", help="Own parser/environment fixtures only; no game launch")
    args = parser.parse_args(argv)
    if args.selftest:
        return selftest()
    if args.name and not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", args.name):
        parser.error("--name must be lowercase letters/digits with single separating hyphens")
    if args.name and len(args.name) > 48:
        parser.error("--name must have at most48 characters")
    if args.deferred_connect:
        # Retired (run partner-deferred-243): since join-at-hub an opened game is an ordinary solo game, and the
        # launcher's Join always restarts the friend's game fresh. Two opened games both played New Game alone before
        # the roles arrived, which real play never does. --join-at-hub covers "open, play, then link".
        parser.error("--deferred-connect is retired: an opened game now plays alone and Join restarts the friend's game; use --join-at-hub")
    if args.deferred_connect and (not args.campaign or not 1 <= args.deferred_connect <= 300):
        parser.error("--deferred-connect must be 1..300 seconds and requires --campaign")
    if args.together and (not args.campaign or args.join_at_hub or args.deferred_connect):
        parser.error("--together requires --campaign and excludes --join-at-hub/--deferred-connect")
    if args.seed_save and not ((args.together or args.join_at_hub) and Path(args.seed_save).is_file()):
        parser.error("--seed-save requires --together or --join-at-hub and an existing file")
    if args.rejoin_in_mission and (args.rejoin or not args.join_at_hub):
        parser.error("--rejoin-in-mission requires --join-at-hub and excludes --rejoin")
    if args.live_first_join and (not args.campaign or not args.join_at_hub or args.rejoin or args.rejoin_in_mission):
        parser.error("--live-first-join requires --campaign --join-at-hub and excludes --rejoin/--rejoin-in-mission")
    if args.live_first_join:
        args.live_join = True
    elif args.live_join and (not args.campaign or not args.join_at_hub or not args.rejoin_in_mission):
        parser.error("--live-join requires --campaign --join-at-hub --rejoin-in-mission")
    if args.rejoin and not args.join_at_hub:
        parser.error("--rejoin requires --join-at-hub")
    if args.join_at_hub and (not args.campaign or args.deferred_connect):
        parser.error("--join-at-hub requires --campaign and excludes --deferred-connect")
    seeded_options = args.seed_host_slots is not None or args.seed_peer_slots is not None or args.overwrite_no or args.stop_at_overwrite
    if seeded_options and args.slot_fixture is None:
        parser.error("Slot seeding/overwrite controls require --slot-fixture")
    args.seed_host_slots = args.seed_host_slots if args.seed_host_slots is not None else (0,) if args.seed_save else ()
    args.seed_peer_slots = args.seed_peer_slots if args.seed_peer_slots is not None else ()
    if args.slot_fixture is not None:
        if (not args.campaign or not args.together or args.join_at_hub or args.rejoin or args.rejoin_in_mission
                or args.tutorial_ui or args.hub_spawn >= 0 or args.no_auto_together or args.one_sided_stop):
            parser.error("--slot-fixture requires --campaign --together and excludes alternate starts/tutorial/rejoin/one-sided stop")
        if (args.seed_host_slots or args.seed_peer_slots) and not args.seed_save:
            parser.error("Seeded rows require --seed-save from a native run-* sandbox")
        if args.overwrite_no and (args.slot_fixture not in args.seed_host_slots or len(args.seed_peer_slots) == 3):
            parser.error("--overwrite-no requires a seeded used host row and at least one empty peer row")
        if args.stop_at_overwrite and (args.overwrite_no or args.slot_fixture not in args.seed_host_slots or len(args.seed_peer_slots) == 3):
            parser.error("--stop-at-overwrite requires a seeded used host row, an empty peer row, and excludes --overwrite-no")
        forbidden = ("LEVEL_END_TEST", "LEVEL_END_DONE", "LEVEL_END_BOSS_WAIT", "HUB_JUMP_TEST", "NIS_GATE_TEST", "HOST_WAIT_LINK", "HOST_CONTINUE", "SIDEKICK_TEST")
        if any(os.environ.get("FOF_RESEARCH_" + flag) for flag in forbidden):
            parser.error("--slot-fixture rejects inherited alternate-route research settings")
    if args.tutorial_ui:
        if (not args.campaign or not args.together or args.join_at_hub or args.rejoin or args.rejoin_in_mission
                or args.seed_save or args.hub_spawn >= 0 or args.no_auto_together or args.frames < 600):
            parser.error("--tutorial-ui requires fresh --campaign --together, --frames>=600, and no alternate start/seed/rejoin")
        forbidden = ("LEVEL_END_TEST", "LEVEL_END_DONE", "LEVEL_END_BOSS_WAIT", "HUB_JUMP_TEST", "NIS_GATE_TEST", "HOST_WAIT_LINK", "HOST_CONTINUE", "SIDEKICK_TEST")
        if any(os.environ.get("FOF_RESEARCH_" + flag) for flag in forbidden):
            parser.error("--tutorial-ui rejects inherited level-end/alternate-route research settings")
    # The research level runner plays several mission ends in one run, so it may run longer.
    level_runner = os.environ.get("FOF_RESEARCH_LEVEL_END_TEST") == "1"
    if not 60 <= args.seconds <= (3600 if level_runner else 1500 if args.join_at_hub else 900 if args.campaign else 240) or not 300 <= args.frames <= (60000 if level_runner else 3600):
        parser.error("--seconds must be60..240 (60..900 with --campaign); --frames must be300..3600")
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
