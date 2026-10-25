"""Private finite QA for the REAL native --play path; not a shipping dependency.

REVIEW BEFORE RUNNING. This script launches both existing isolated game copies,
with real-human input flags and unlimited native network frames, over loopback.
It supplies no input, uses no Steam account, and never enables bots or fixtures.
After >=300 matching sampled frames it requests WM_CLOSE of both positively
owned games, requiring actual game and launcher exit0. Timeout/job termination
is always failure. This is idle/lifecycle QA, not control usability or co-op QA.

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
FAILURE = re.compile(r"NET FAIL|EXCEPTION|FAIL closed|TIMEOUT:", re.I)
FIXTURE = re.compile(r"KEYBOARD_FIXTURE|DISCONNECT_NEUTRAL|MUTATION_FIXTURE|TRANSITION PARTY_INSIDE|JITTER_FIXTURE|PAUSE_FIXTURE|PAUSE ASSERT")


class Failed(RuntimeError):
    pass


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


def environment(source, seat, diagnostics=True, campaign=False, deferred=False, profile=None, hub_spawn=-1, skip_nis=False, auto_together=True, net_port="49375"):
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
    env.update({"COOP_PROBE_" + key: value for key, value in values.items()})
    return env


def text(path):
    """Ignore only a trailing unfinished write; malformed complete rows fail."""
    if not path.exists():
        return ""
    no_reparse(path)
    value = path.read_text(encoding="utf-8", errors="replace")
    return value[:value.rfind("\n") + 1]


def rows(value, stride=1):
    found = {}
    previous = -stride
    for line in value.splitlines():
        if not line.startswith("NET state "):
            continue
        match = ROW.fullmatch(line)
        if not match:
            raise Failed("Malformed state row (requires progress, area and logical slots0,1): " + line[:180])
        tick = int(match[1])
        if tick != previous + stride or tick > 0xffffffff:
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


def compare(first, second, stride=1):
    common = sorted(set(first) & set(second))
    if common != list(range(0, len(common) * stride, stride)):
        raise Failed("Common samples are not consecutive from tick0")
    for tick in common:
        if first[tick] != second[tick]:
            raise Failed("Paired state differs at common tick " + str(tick))
    return common[-1] + 1 if common else 0


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
        "one_sided_stop": args.one_sided_stop, "log_stride": 120 if args.quiet else 1, "logged_common_samples": 0,
        "preflight": [], "game_pids": [None, None], "game_exitcodes": [None, None],
        "launcher_returncodes": [None, None], "close_requests": {}, "forced_cleanup": [], "errors": []}
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
        if args.seed_save:
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
                            host_profile if (args.join_at_hub or args.together) and seat == "host" else None, args.hub_spawn,
                            args.skip_cutscenes == "both" or (args.skip_cutscenes == "host" and seat == "host"),
                            not args.no_auto_together, "49375" if seat == "host" else peer_port), seat + "-launcher")
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
                dropped = args.rejoin and seat == "peer" and rejoin_stage == 1
                if launchers[i].poll() is not None and not dropped:
                    raise Failed(seat + ": launcher exited before requested close")
                if games[i] is not None and api.exit(games[i]) is not None and not dropped:
                    raise Failed(seat + ": game exited before requested close")
                outputs.append(probe_text)
                parsed.append(rows(probe_text, 120 if args.quiet else 1))
            if args.rejoin and rejoin_stage == 0 and connected and "CAMPAIGN ASSERT JOIN_COMPLETE" in outputs[1]:
                # Both in the hub after the first join: the joiner leaves; the host must keep playing alone.
                if api.close_windows([games[1]]).get(games[1]["pid"], 0) < 1:
                    raise Failed("peer: close request for the drop was not posted")
                rejoin_stage = 1
                event("PEER_DROPPED_AT_HUB")
            if rejoin_stage == 1 and "NET PARTNER_LOST" in outputs[0] and launchers[1].poll() is not None:
                if launchers[1].returncode != 0:
                    raise Failed("peer: dropped launcher exit " + str(launchers[1].returncode))
                boxes[1] = ROOT / ("run-play-lifecycle-" + name + "-peer2")
                launcher_logs[1] = result / "peer2-launcher.log"
                games[1] = None
                launchers[1] = popen([str(ROOT / "probe" / "native-launcher.exe"), "--play",
                    str(ROOT / "test" / LANGUAGES[1].lower() / "Game.exe"), str(boxes[1])],
                    launcher_logs[1], environment(os.environ, "peer", not args.quiet, args.campaign, True, None, auto_together=not args.no_auto_together, net_port=peer_port), "peer2-launcher")
                lane.register_launcher(launchers[1])
                connected, rejoin_stage = False, 2
                event("HOST_CONTINUED_ALONE_REPLACEMENT_PEER_STARTED")
            if args.join_at_hub and not connected and all(games):
                # Both linked at once; the host bot plays New Game alone and the joiner waits at its main menu.
                for i, role in enumerate(("host", "join")):
                    temporary = boxes[i] / "connect.json.tmp"
                    temporary.write_text(json.dumps({"role": role}, separators=(",", ":")), encoding="utf-8")
                    os.replace(temporary, boxes[i] / "connect.json")
                connected = True
                if rejoin_stage == 2:
                    rejoin_stage = 3
                event("JOIN_AT_HUB_ROLES_WRITTEN", rejoin=rejoin_stage == 3)
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
            common = compare(*parsed, stride=120 if args.quiet else 1)
            summary["logged_common_samples"] = len(set(parsed[0]) & set(parsed[1]))
            summary["common_frames"] = common
            ready = [bool(re.search(r"^NET READY role=" + str(i + 1) + r"$", outputs[i], re.M)) for i in range(2)]
            if all(games) and all(ready) and common >= args.frames and (not args.rejoin or rejoin_stage == 3):
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
        closing = games[:1] if args.one_sided_stop else games
        summary["close_requests"] = api.close_windows(closing)
        if any(summary["close_requests"].get(game["pid"], 0) < 1 for game in closing):
            raise Failed("Each selected owned game requires a successful WM_CLOSE post")
        event("HOST_ONLY_WM_CLOSE" if args.one_sided_stop else "BOTH_OWNED_GAMES_WM_CLOSE")
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
        summary["common_frames"] = compare(*(rows(value, 120 if args.quiet else 1) for value in final_logs), stride=120 if args.quiet else 1)
        if summary["common_frames"] < args.frames:
            raise Failed("Final common sample count regressed")
        if args.one_sided_stop:
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
    print("OWN_PLAY_LIFECYCLE_SELFTEST_PASS rows/invalid/divergence/environment/quiet-stride")
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
    parser.add_argument("--deferred-connect", type=int, default=0, metavar="SECONDS",
                        help="With --campaign: open both games first, keep their menus frozen for SECONDS, then hand out the roles")
    parser.add_argument("--campaign", action="store_true", help="Use the retail campaign route (bot drives host menus/hub) instead of the research scenario")
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
    if args.rejoin and not args.join_at_hub:
        parser.error("--rejoin requires --join-at-hub")
    if args.join_at_hub and (not args.campaign or args.deferred_connect):
        parser.error("--join-at-hub requires --campaign and excludes --deferred-connect")
    # The research level runner plays several mission ends in one run, so it may run longer.
    level_runner = os.environ.get("FOF_RESEARCH_LEVEL_END_TEST") == "1"
    if not 60 <= args.seconds <= (3600 if level_runner else 1500 if args.join_at_hub else 900 if args.campaign else 240) or not 300 <= args.frames <= (60000 if level_runner else 3600):
        parser.error("--seconds must be60..240 (60..900 with --campaign); --frames must be300..3600")
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
