"""Record a bounded live campaign input journal and replay it in a fresh Russian copy.

By default, recording uses the two-seat lifecycle runner. ``--solo-record`` records
native-AI mode from one fresh Russian copy. Every game run uses an owned sandbox and
retained launcher/game handles.
"""
import argparse
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
FRAMES = 1800
SCHEMA_VERSION = 2
PARTY_MODE_NATIVE_AI = 0
PARTY_MODE_TWO_HUMANS = 1
MAX_FRAMES = 216000
AREA_BYTES = 96
PROGRESS_BYTES = 508
METADATA_BODY = 624
METADATA_RECORD = 632
FRAME_BODY = 260
FRAME_RECORD = 268
MAX_JOURNAL_BYTES = METADATA_RECORD + 4 + MAX_FRAMES * FRAME_RECORD
CRITICAL = "c46b90afe36571cf3f4fbe3d4e7918da833297f78119d40504e96b4f531b36ea"
EXECUTABLE = "ebd30047d7e58c95bf7dc4bd1fb4a5780e16aa472b61c25a8c421632662083db"
HOOK = re.compile(r"^HOOK_READY pid=(\d+) sha256=([0-9a-f]{64}) selftest=0 timeout=0s play=1$", re.M)
PROCESS_EXIT = re.compile(r"^PROCESS_EXIT pid=(\d+) code=(\d+)$", re.M)


class ReplayFailure(RuntimeError):
    pass


def _load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _plain_path(path, new=False):
    path = Path(os.path.abspath(path))
    for ancestor in reversed((path, *path.parents)):
        if new and ancestor == path:
            if ancestor.exists() or ancestor.is_symlink():
                raise ReplayFailure("Expected a fresh path: " + ancestor.name)
            continue
        info = ancestor.lstat()
        if info.st_file_attributes & 0x400:
            raise ReplayFailure("Reparse point in QA path: " + ancestor.name)
    return path


def _fnv64(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value


def _canonical_area(raw):
    nul = raw.find(b"\0")
    if nul <= 0 or any(raw[nul + 1:]):
        return False
    try:
        path = raw[:nul].decode("ascii")
    except UnicodeDecodeError:
        return False
    if path.startswith("/") or path.endswith("/") or "\\" in path:
        return False
    for component in path.split("/"):
        if component in ("", ".", "..") or not re.fullmatch(r"[A-Za-z0-9_-]+", component):
            return False
    return True


def parse_journal(data, expected_frames=FRAMES):
    """Validate the bounded schema-2 wire image and return its basic metadata."""
    if not isinstance(data, (bytes, bytearray)) or len(data) < METADATA_RECORD + 4 or len(data) > MAX_JOURNAL_BYTES:
        raise ReplayFailure("Journal length outside bounded format")
    if _fnv64(data[:METADATA_BODY]) != struct.unpack_from("<Q", data, METADATA_BODY)[0]:
        raise ReplayFailure("Metadata checksum mismatch")
    area = data[:AREA_BYTES]
    if not _canonical_area(area):
        raise ReplayFailure("Journal area path is invalid or noncanonical")
    base_tick, seed, sidekick, party_mode, schema = struct.unpack_from("<IIIII", data, AREA_BYTES)
    if schema != SCHEMA_VERSION or party_mode not in (PARTY_MODE_NATIVE_AI, PARTY_MODE_TWO_HUMANS):
        raise ReplayFailure("Journal schema or party mode is unsupported")
    count = struct.unpack_from("<I", data, METADATA_RECORD)[0]
    if count > MAX_FRAMES or count != expected_frames:
        raise ReplayFailure("Journal frame count differs from the requested bounded run")
    expected_size = METADATA_RECORD + 4 + count * FRAME_RECORD
    if len(data) != expected_size:
        raise ReplayFailure("Journal has a truncated or trailing record")
    if count and base_tick + count - 1 > 0xffffffff:
        raise ReplayFailure("Journal ticks overflow uint32")
    offset = METADATA_RECORD + 4
    transitions = []
    previous_mode = party_mode
    for index in range(count):
        record = data[offset:offset + FRAME_RECORD]
        body = record[:FRAME_BODY]
        if _fnv64(body) != struct.unpack_from("<Q", record, FRAME_BODY)[0]:
            raise ReplayFailure("Frame checksum mismatch at index " + str(index))
        tick = struct.unpack_from("<I", body, 0)[0]
        if tick != base_tick + index:
            raise ReplayFailure("Duplicate, missing, or out-of-order tick at index " + str(index))
        actions = struct.unpack_from("<56f", body, 4)
        if any(not math.isfinite(value) or value < 0.0 or value > 1.0 for value in actions):
            raise ReplayFailure("Invalid action value at index " + str(index))
        pointer_x, pointer_y, pointer_flags = struct.unpack_from("<III", body, 228)
        status, host_slot, frame_mode = struct.unpack_from("<III", body, 240)
        if pointer_x > 65535 or pointer_y > 65535 or pointer_flags & ~3:
            raise ReplayFailure("Invalid pointer sample at index " + str(index))
        if pointer_flags & 2 and not pointer_flags & 1:
            raise ReplayFailure("Inactive pointer click at index " + str(index))
        if not pointer_flags and (pointer_x or pointer_y):
            raise ReplayFailure("Inactive pointer has coordinates at index " + str(index))
        if (status & ~0x1f or host_slot > 1 or frame_mode not in
                (PARTY_MODE_NATIVE_AI, PARTY_MODE_TWO_HUMANS) or
                (index == 0 and frame_mode != party_mode)):
            raise ReplayFailure("Invalid frame status, slot, or party-mode transition at index " + str(index))
        if frame_mode == PARTY_MODE_NATIVE_AI and (host_slot != 0 or any(value != 0.0 for value in actions[28:])):
            raise ReplayFailure("Native-AI frame contains partner action at index " + str(index))
        if index and frame_mode != previous_mode:
            transitions.append({"tick": tick, "from": previous_mode, "to": frame_mode})
        previous_mode = frame_mode
        offset += FRAME_RECORD
    return {"area": data[:AREA_BYTES].split(b"\0", 1)[0].decode("ascii"),
            "base_tick": base_tick, "seed": seed, "sidekick": sidekick,
            "party_mode": party_mode, "schema_version": schema, "frames": count,
            "bytes": len(data), "mode_transitions": transitions}


def mutate_post_state(data):
    """Change frame zero's postState and reseal its record checksum."""
    changed = bytearray(data)
    offset = METADATA_RECORD + 4
    post_state_offset = offset + 252
    old = struct.unpack_from("<Q", changed, post_state_offset)[0]
    struct.pack_into("<Q", changed, post_state_offset, old ^ 1)
    struct.pack_into("<Q", changed, offset + FRAME_BODY,
                     _fnv64(changed[offset:offset + FRAME_BODY]))
    return bytes(changed)


def _fresh_run_name(prefix):
    return prefix + "-" + uuid.uuid4().hex[:16]


def hook_digest_valid(digest):
    return digest == EXECUTABLE


def _clean_record_environment():
    env = {key: value for key, value in os.environ.items()
           if not key.upper().startswith(("COOP_PROBE_", "FOF_RESEARCH_"))}
    env["FOF_RESEARCH_LIVE_RECORD_TEST"] = "1"
    return env


def _verify_record_run(name, return_code, record_started_ns=None):
    result_dir = _plain_path(ROOT / "launcher" / "play-results" / name)
    summary_path = _plain_path(result_dir / "summary.json")
    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    events_path = _plain_path(result_dir / "events.jsonl")
    events = [json.loads(line) for line in events_path.read_text(encoding="utf-8").splitlines() if line]
    outcomes = [item for item in events if item.get("event") == "LIFECYCLE_RESULT"]
    if (summary.get("name") != name or len(outcomes) != 1 or outcomes[0].get("result") != "PASS" or
            outcomes[0].get("common_frames", 0) < FRAMES):
        raise ReplayFailure("Record lifecycle evidence does not identify this completed passing run")
    if return_code != 0 or summary.get("result") != "PASS":
        raise ReplayFailure("Live recording lifecycle did not pass")
    if summary.get("common_frames", 0) < FRAMES or summary.get("required_common_frames") != FRAMES:
        raise ReplayFailure("Live recording did not reach the required matching frames")
    if summary.get("forced_cleanup") or summary.get("errors"):
        raise ReplayFailure("Live recording used forced cleanup or reported errors")
    if summary.get("game_exitcodes") != [0, 0] or summary.get("launcher_returncodes") != [0, 0]:
        raise ReplayFailure("Live recording peers did not exit cleanly")
    preflight = summary.get("preflight")
    if not isinstance(preflight, list) or len(preflight) != 2:
        raise ReplayFailure("Fresh English/Russian preflight records are missing")
    expected_records = ["qa-play-lifecycle-" + name + "-" + seat + ".json" for seat in ("host", "peer")]
    expected_sandboxes = ["run-play-lifecycle-" + name + "-" + seat for seat in ("host", "peer")]
    if summary.get("preflight_records") != expected_records or summary.get("sandboxes") != expected_sandboxes:
        raise ReplayFailure("Record summary paths do not belong to the requested lifecycle run")
    if {item.get("seat") for item in events if item.get("event") == "PREFLIGHT_PASS"} != {"host", "peer"}:
        raise ReplayFailure("Record lifecycle lacks both fresh preflight completion events")
    for seat, expected, item in zip(("host", "peer"), ("English", "Russian"), preflight):
        if (item.get("status") != "pass" or item.get("language") != expected or
                item.get("critical_digest") != CRITICAL or item.get("critical_files") != 1257 or
                item.get("locale_files") != 34):
            raise ReplayFailure("Fresh " + expected + " preflight did not pass")
        record = _plain_path(ROOT / "compatibility" / ("qa-play-lifecycle-" + name + "-" + seat + ".json"))
        record_data = json.loads(record.read_text(encoding="utf-8"))
        if record_data != item or record.stat().st_mtime_ns > summary_path.stat().st_mtime_ns:
            raise ReplayFailure("Preflight file does not match the record-run summary: " + seat)
        if record_started_ns is not None and record.stat().st_mtime_ns < record_started_ns:
            raise ReplayFailure("Preflight record is stale: " + seat)
    host_box = _plain_path(ROOT / ("run-play-lifecycle-" + name + "-host"))
    journal = _plain_path(host_box / "live-join-journal.bin")
    if journal.stat().st_size > MAX_JOURNAL_BYTES:
        raise ReplayFailure("Recorded journal exceeds the format limit")
    data = journal.read_bytes()
    metadata = parse_journal(data)
    if metadata["party_mode"] != PARTY_MODE_TWO_HUMANS:
        raise ReplayFailure("Paired lifecycle record must begin in two-human mode")
    return journal, data, metadata, summary


def _replay_env(lifecycle):
    source = {key: value for key, value in os.environ.items()
              if not key.upper().startswith(("COOP_PROBE_", "FOF_RESEARCH_"))}
    # The helper consults os.environ for research-only route options. Temporarily
    # remove those process-level values so only its requested standard route applies.
    saved = {key: value for key, value in os.environ.items() if key.upper().startswith("FOF_RESEARCH_")}
    try:
        for key in saved:
            os.environ.pop(key, None)
        env = lifecycle.environment(source, "host", diagnostics=True, campaign=True,
                                    deferred=True, skip_nis=True)
    finally:
        os.environ.update(saved)
    return {key: value for key, value in env.items()
            if not key.upper().startswith("FOF_RESEARCH_")}


def _launch_env(lifecycle, *, journal_path=None, record_mode=None):
    env = _replay_env(lifecycle)
    if record_mode is not None:
        if record_mode not in (2, 3):
            raise ReplayFailure("Unsupported standalone recording mode")
        env["COOP_PROBE_LIVE_RECORD_TEST"] = str(record_mode)
    else:
        if journal_path is None:
            raise ReplayFailure("A replay journal is required")
        env["COOP_PROBE_LIVE_REPLAY_TEST"] = str(Path(journal_path).resolve())
        env["COOP_PROBE_LIVE_REPLAY_FRAMES"] = str(FRAMES)
    if (env.get("COOP_PROBE_NET_MODE") != "deferred" or env.get("COOP_PROBE_CAMPAIGN") != "1" or
            env.get("COOP_PROBE_CAMPAIGN_FIXTURE") != "1" or env.get("COOP_PROBE_SKIP_NIS") != "1"):
        raise ReplayFailure("Standalone launch must stay in deferred local mode")
    return env


def _role_files_absent(sandbox):
    return not any((sandbox / name).exists() or (sandbox / name).is_symlink()
                   for name in ("connect.json", "connect.json.tmp"))


def _finish_owned_game(api, game, process):
    """Query the retained game handle before closing it, on every cleanup path."""
    try:
        process_stopped = process is None or process.poll() is not None
        game_stopped = game is None or api is None or api.exit(game) is not None
        return process_stopped and game_stopped
    finally:
        if game is not None and api is not None:
            api.kernel.CloseHandle(game["handle"])


def run_owned_game(marker_pattern, run_name, seconds, *, journal_path=None, record_mode=None):
    if os.name != "nt":
        raise ReplayFailure("Windows is required for live game replay")
    lane_module = _load_module("coop_game_lane", ROOT / "qa" / "game_lane.py")
    lifecycle = _load_module("coop_play_lifecycle", ROOT / "launcher" / "play_lifecycle.py")
    sandbox = _plain_path(ROOT / ("run-live-join-replay-" + run_name), new=True)
    log_path = _plain_path(ROOT / "analysis" / ("live-join-replay-" + run_name + ".log"), new=True)
    game_path = _plain_path(ROOT / "test" / "russian" / "Game.exe")
    launcher_path = _plain_path(ROOT / "probe" / "native-launcher.exe")
    env = _launch_env(lifecycle, journal_path=journal_path, record_mode=record_mode)

    lane = lane_module.GameLane(seconds + 120).acquire()
    process = None
    game = None
    api = None
    stream = None
    deferred_wait_seen = False
    marker_seen = False
    close_attempted = False
    forced_cleanup = False
    text = ""
    observed_marker = None
    expected_exits = []
    try:
        api = lifecycle.Win32()
        stream = log_path.open("xb")
        process = subprocess.Popen([str(launcher_path), "--play", str(game_path), str(sandbox)],
                                   cwd=ROOT, env=env, stdout=stream, stderr=subprocess.STDOUT,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        lane.register_launcher(process)
        deadline = time.monotonic() + seconds
        close_deadline = None
        while time.monotonic() < (close_deadline or deadline):
            text = log_path.read_text(encoding="utf-8", errors="replace")
            hook = HOOK.findall(text)
            if len(hook) > 1:
                raise ReplayFailure("Duplicate launcher HOOK_READY")
            if hook and game is None:
                if not hook_digest_valid(hook[0][1]):
                    raise ReplayFailure("Launcher reported an unexpected Russian executable digest")
                game = api.capture(int(hook[0][0]), process, game_path)
            probe_log = sandbox / "probe.log"
            probe_text = probe_log.read_text(encoding="utf-8", errors="replace") if probe_log.exists() else ""
            if sandbox.exists() and not _role_files_absent(sandbox):
                raise ReplayFailure("Standalone deferred run unexpectedly created a role file")
            if re.search(r"NET FAIL|EXCEPTION|FAIL closed|TIMEOUT:", text + probe_text, re.I):
                raise ReplayFailure("Standalone run logged NET FAIL, exception, launcher failure, or timeout")
            if game is not None and "NET DEFERRED_WAIT" in probe_text:
                deferred_wait_seen = True
            if re.search(r"^NET (?:DEFERRED_CONNECT|opening|READY)", probe_text, re.M):
                raise ReplayFailure("Standalone deferred run unexpectedly opened live transport")

            marker_match = marker_pattern.search(probe_text)
            if marker_match:
                observed_marker = marker_match.group(0)
                if not game:
                    raise ReplayFailure("Standalone marker arrived before owned game capture")
                if not marker_seen:
                    marker_seen = True
                    close_deadline = time.monotonic() + 25
                # Runtime normally posts WM_CLOSE with its marker. If the game
                # remains open, make one owned-window close request as a fallback.
                if not close_attempted and api.exit(game) is None and process.poll() is None:
                    api.close_windows([game])
                    close_attempted = True
            if process.poll() is not None:
                break
            if game is not None and api.exit(game) is not None and not marker_seen:
                raise ReplayFailure("Standalone game exited before its expected marker")
            time.sleep(0.25)
        if process.poll() is None:
            raise ReplayFailure("Replay did not exit within its bounded time")
        text = log_path.read_text(encoding="utf-8", errors="replace")
        probe_log = sandbox / "probe.log"
        probe_text = probe_log.read_text(encoding="utf-8", errors="replace") if probe_log.exists() else ""
        if not deferred_wait_seen or not marker_seen:
            raise ReplayFailure("Standalone run did not remain deferred through its marker and clean close")
        if not _role_files_absent(sandbox):
            raise ReplayFailure("Standalone deferred run created a role file")
        marker_matches = list(marker_pattern.finditer(probe_text))
        if len(marker_matches) != 1:
            raise ReplayFailure("Standalone run must emit its expected marker exactly once")
        observed_marker = marker_matches[0].group(0)
        exits = PROCESS_EXIT.findall(text)
        if (len(exits) != 1 or game is None or exits[0][0] != str(game["pid"]) or
                exits[0][1] != "0" or process.returncode != 0):
            raise ReplayFailure("Standalone launcher/game did not report a normal exit0")
        if game is None or api.exit(game) != 0:
            raise ReplayFailure("Standalone game handle did not report clean exit0")
        if re.search(r"NET FAIL|EXCEPTION|FAIL closed|TIMEOUT:", text + probe_text, re.I):
            raise ReplayFailure("Standalone run logged NET FAIL, exception, launcher failure, or timeout")
        expected_exits.append({"launcher": process.returncode, "game": api.exit(game),
                               "marker": observed_marker})
        return {"name": run_name, "result": "PASS", "exits": expected_exits,
                "sandbox": sandbox.name, "log": log_path.name}
    finally:
        if process is not None and process.poll() is None:
            if game is not None and api is not None and api.exit(game) is None:
                try:
                    api.close_windows([game])
                    process.wait(timeout=20)
                except (OSError, subprocess.TimeoutExpired):
                    pass
            if process.poll() is None:
                forced_cleanup = True
                try:
                    process.terminate()
                    process.wait(timeout=10)
                except (OSError, subprocess.TimeoutExpired):
                    try:
                        process.kill()
                        process.wait(timeout=10)
                    except (OSError, subprocess.TimeoutExpired):
                        pass
        try:
            stopped = _finish_owned_game(api, game, process)
        except Exception:
            stopped = False
        if stream is not None:
            stream.close()
        try:
            lane.release(children_stopped=stopped)
        except RuntimeError:
            if not forced_cleanup:
                raise


def _fresh_preflight(result_dir, language="Russian"):
    if language not in ("English", "Russian"):
        raise ReplayFailure("Preflight language must be English or Russian")
    output_path = _plain_path(ROOT / "compatibility" / ("qa-" + result_dir.name + "-" + language.lower() + ".json"), new=True)
    log_path = _plain_path(result_dir / (language.lower() + "-preflight.log"), new=True)
    env = {key: value for key, value in os.environ.items()
           if not key.upper().startswith(("COOP_PROBE_", "FOF_RESEARCH_"))}
    started_ns = time.time_ns()
    command = [sys.executable, "-I", str(ROOT / "compatibility" / "preflight.py"), "verify",
               "--game-dir", str(ROOT / "test" / language.lower()), "--output", str(output_path)]
    with log_path.open("xb") as output:
        process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=output,
                                   stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
        return_code = process.wait()
    if return_code != 0 or not output_path.is_file() or output_path.stat().st_mtime_ns < started_ns:
        raise ReplayFailure("Fresh " + language + " game-copy preflight failed")
    record = json.loads(output_path.read_text(encoding="utf-8"))
    if (record.get("schema") != "coop-fof-preflight-v1" or record.get("status") != "pass" or
            record.get("language") != language or record.get("critical_digest") != CRITICAL or
            record.get("critical_files") != 1257 or record.get("locale_files") != 34):
        raise ReplayFailure("Fresh " + language + " game-copy preflight evidence is invalid")
    return record


def _make_synthetic_journal(modes=(PARTY_MODE_NATIVE_AI, PARTY_MODE_TWO_HUMANS, PARTY_MODE_NATIVE_AI)):
    metadata = bytearray(METADATA_RECORD)
    area = b"Tokyo/1_Level/1_Area"
    metadata[:len(area)] = area
    struct.pack_into("<IIIII", metadata, AREA_BYTES, 50, 8466, 3, modes[0], SCHEMA_VERSION)
    struct.pack_into("<Q", metadata, METADATA_BODY, _fnv64(metadata[:METADATA_BODY]))
    records = bytearray()
    for index, mode in enumerate(modes):
        body = bytearray(FRAME_BODY)
        struct.pack_into("<I", body, 0, 50 + index)
        struct.pack_into("<f", body, 4, 0.25)
        host_slot = 0
        if mode == PARTY_MODE_TWO_HUMANS:
            struct.pack_into("<f", body, 116, 0.5)
            host_slot = 1
        struct.pack_into("<IIIQ", body, 240, 0x15, host_slot, mode, 0x1234 + index)
        records.extend(body)
        records.extend(struct.pack("<Q", _fnv64(body)))
    return bytes(metadata) + struct.pack("<I", len(modes)) + bytes(records)


def selftest():
    transition_meta = {"party_mode": PARTY_MODE_NATIVE_AI, "mode_transitions": [
        {"tick": 973, "from": 0, "to": 1}, {"tick": 1200, "from": 1, "to": 0}]}
    _verify_party_transition_text("LIVE_PARTY MODE tick=973 from=0 to=1 preserved=1\n"
                                  "LIVE_PARTY MODE tick=1200 from=1 to=0 preserved=1\n", transition_meta)
    if not hook_digest_valid(EXECUTABLE) or hook_digest_valid(CRITICAL):
        raise AssertionError("HOOK_READY must validate the executable digest, not the content digest")
    journal = _make_synthetic_journal()
    metadata = parse_journal(journal, 3)
    if metadata["party_mode"] != PARTY_MODE_NATIVE_AI:
        raise AssertionError("Synthetic journal initial party mode mismatch")
    if metadata["mode_transitions"] != [
            {"tick": 51, "from": PARTY_MODE_NATIVE_AI, "to": PARTY_MODE_TWO_HUMANS},
            {"tick": 52, "from": PARTY_MODE_TWO_HUMANS, "to": PARTY_MODE_NATIVE_AI}]:
        raise AssertionError("Synthetic AI-human-AI journal transitions were not parsed")
    _validate_party_transition_modes({
        "party_mode": PARTY_MODE_NATIVE_AI,
        "mode_transitions": [
            {"tick": 900, "from": PARTY_MODE_NATIVE_AI, "to": PARTY_MODE_TWO_HUMANS},
            {"tick": 1200, "from": PARTY_MODE_TWO_HUMANS, "to": PARTY_MODE_NATIVE_AI}]})
    for invalid in (
            {"party_mode": PARTY_MODE_TWO_HUMANS, "mode_transitions": []},
            {"party_mode": PARTY_MODE_NATIVE_AI, "mode_transitions": [
                {"tick": 899, "from": PARTY_MODE_NATIVE_AI, "to": PARTY_MODE_TWO_HUMANS},
                {"tick": 1200, "from": PARTY_MODE_TWO_HUMANS, "to": PARTY_MODE_NATIVE_AI}]},
            {"party_mode": PARTY_MODE_NATIVE_AI, "mode_transitions": [
                {"tick": 900, "from": PARTY_MODE_NATIVE_AI, "to": PARTY_MODE_TWO_HUMANS}]}
    ):
        try:
            _validate_party_transition_modes(invalid)
        except ReplayFailure:
            pass
        else:
            raise AssertionError("Invalid party-mode transition sequence was accepted")
    paired = _make_synthetic_journal((PARTY_MODE_TWO_HUMANS, PARTY_MODE_NATIVE_AI,
                                      PARTY_MODE_TWO_HUMANS))
    if parse_journal(paired, 3)["party_mode"] != PARTY_MODE_TWO_HUMANS:
        raise AssertionError("Synthetic paired journal with a later AI transition was rejected")
    changed = mutate_post_state(journal)
    if changed == journal or parse_journal(changed, 3) != metadata:
        raise AssertionError("Valid-checksum postState mutation must remain structurally readable")

    bad_initial = bytearray(journal)
    first = METADATA_RECORD + 4
    struct.pack_into("<I", bad_initial, first + 248, PARTY_MODE_TWO_HUMANS)
    struct.pack_into("<Q", bad_initial, first + FRAME_BODY,
                     _fnv64(bad_initial[first:first + FRAME_BODY]))
    try:
        parse_journal(bad_initial, 3)
    except ReplayFailure:
        pass
    else:
        raise AssertionError("Initial frame mode differing from metadata was accepted")

    bad_ai_actions = bytearray(journal)
    struct.pack_into("<f", bad_ai_actions, first + 116, 0.5)
    struct.pack_into("<Q", bad_ai_actions, first + FRAME_BODY,
                     _fnv64(bad_ai_actions[first:first + FRAME_BODY]))
    try:
        parse_journal(bad_ai_actions, 3)
    except ReplayFailure:
        pass
    else:
        raise AssertionError("Native-AI frame with partner actions was accepted")

    lifecycle = _load_module("coop_play_lifecycle_selftest", ROOT / "launcher" / "play_lifecycle.py")
    with tempfile.TemporaryDirectory(prefix="fof-live-join-selftest-") as temp:
        sandbox = Path(temp)
        before = set(sandbox.iterdir())
        replay_env = _launch_env(lifecycle, journal_path=sandbox / "fixture-journal.bin")
        solo_record_env = _launch_env(lifecycle, record_mode=2)
        transition_record_env = _launch_env(lifecycle, record_mode=3)
        if (replay_env.get("COOP_PROBE_NET_MODE") != "deferred" or
                replay_env.get("COOP_PROBE_CAMPAIGN") != "1" or
                replay_env.get("COOP_PROBE_SKIP_NIS") != "1" or
                replay_env.get("COOP_PROBE_LIVE_REPLAY_TEST") != str((sandbox / "fixture-journal.bin").resolve())):
            raise AssertionError("Standalone replay environment must remain deferred campaign mode")
        if (solo_record_env.get("COOP_PROBE_NET_MODE") != "deferred" or
                solo_record_env.get("COOP_PROBE_LIVE_RECORD_TEST") != "2" or
                "COOP_PROBE_LIVE_REPLAY_TEST" in solo_record_env):
            raise AssertionError("Solo recorder must be deferred and set only the record mode")
        if (transition_record_env.get("COOP_PROBE_NET_MODE") != "deferred" or
                transition_record_env.get("COOP_PROBE_LIVE_RECORD_TEST") != "3" or
                "COOP_PROBE_LIVE_REPLAY_TEST" in transition_record_env):
            raise AssertionError("Party-transition recorder must be deferred mode 3")
        if set(sandbox.iterdir()) != before or not _role_files_absent(sandbox):
            raise AssertionError("Preparing standalone environments must not create connect role files")

    events = []
    class MockProcess:
        def poll(self):
            events.append("process-exit-query")
            return 0
    class MockKernel:
        def CloseHandle(self, _handle):
            events.append("game-handle-close")
    class MockApi:
        kernel = MockKernel()
        def exit(self, _game):
            events.append("game-exit-query")
            return 0
    if not _finish_owned_game(MockApi(), {"handle": object()}, MockProcess()):
        raise AssertionError("Mock owned game should be stopped")
    if events != ["process-exit-query", "game-exit-query", "game-handle-close"]:
        raise AssertionError("Game exit must be queried before its retained handle is closed")
    args = _argument_parser().parse_args(["--party-transition-record"])
    if not args.party_transition_record or args.solo_record:
        raise AssertionError("Party-transition CLI option was not parsed independently")
    args = _argument_parser().parse_args(["--solo-record"])
    if not args.solo_record or args.party_transition_record:
        raise AssertionError("Native-AI solo-record CLI option changed")
    print("LIVE_JOIN_REPLAY_SELFTEST_PASS digest/deferred/no-role/transitions/mutation/party-option")
    return 0


def _validate_party_transition_modes(metadata):
    if metadata["party_mode"] != PARTY_MODE_NATIVE_AI:
        raise ReplayFailure("Party-transition recording did not begin in native-AI mode")
    transitions = metadata["mode_transitions"]
    expected = [(PARTY_MODE_NATIVE_AI, PARTY_MODE_TWO_HUMANS),
                (PARTY_MODE_TWO_HUMANS, PARTY_MODE_NATIVE_AI)]
    actual = [(item["from"], item["to"]) for item in transitions]
    if actual != expected:
        raise ReplayFailure("Party-transition journal must contain exactly AI-to-human then human-to-AI")
    if transitions[0]["tick"] < 900 or transitions[1]["tick"] < 1200:
        raise ReplayFailure("Party-mode transition occurred before its safe tick threshold")


def _verify_party_transition_record(record, metadata):
    log_path = _plain_path(ROOT / record["sandbox"] / "probe.log")
    _verify_party_transition_text(log_path.read_text(encoding="utf-8", errors="replace"), metadata)


def _verify_party_transition_text(log_text, metadata):
    _validate_party_transition_modes(metadata)
    transitions = metadata["mode_transitions"]
    marker_re = re.compile(r"^LIVE_PARTY MODE tick=(\d+) from=(\d+) to=(\d+) preserved=1$", re.M)
    markers = [(int(tick), int(old), int(new)) for tick, old, new in marker_re.findall(log_text)]
    expected_markers = [(item["tick"], item["from"], item["to"]) for item in transitions]
    if markers != expected_markers:
        raise ReplayFailure("Party transition markers do not exactly match the journal transitions")
    if "LIVE_RECORD SOLO area load" in log_text:
        raise ReplayFailure("Standalone transition recording reloaded its native mission area")
    lines = log_text.splitlines()
    marker_indices = [index for index, line in enumerate(lines) if marker_re.fullmatch(line)]
    if len(marker_indices) != 2:
        raise ReplayFailure("Party transition preservation markers must appear exactly twice")
    between = "\n".join(lines[marker_indices[0] + 1:marker_indices[1]])
    if re.search(r"LIVE_RECORD SOLO area load|CAMPAIGN ASSERT MISSION_COMMIT|LIVE_JOURNAL NATIVE_MISSION_START",
                 between):
        raise ReplayFailure("A native area reload or mission commit occurred between party-mode switches")


def run_solo_record(args, *, party_transition=False):
    if os.name != "nt":
        raise ReplayFailure("Windows is required for native-AI recording")
    if not 120 <= args.seconds <= 1200:
        raise ReplayFailure("--seconds must be between 120 and 1200")
    name = args.name or _fresh_run_name("live-join-solo")
    if not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", name) or len(name) > 28:
        raise ReplayFailure("--name must be lowercase letters/digits with single hyphens, at most 28 chars")
    result_dir = _plain_path(ROOT / "analysis" / ("live-join-replay-" + name), new=True)
    result_dir.mkdir()
    preflight = _fresh_preflight(result_dir, "Russian")

    record_name = _fresh_run_name("solo-record")
    record_marker = re.compile(r"^LIVE_RECORD SAVED frames=1800 bytes=\d+$", re.M)
    record = run_owned_game(record_marker, record_name, args.seconds,
                            record_mode=3 if party_transition else 2)
    journal_path = _plain_path(ROOT / ("run-live-join-replay-" + record_name) / "live-join-journal.bin")
    data = journal_path.read_bytes()
    metadata = parse_journal(data)
    if metadata["party_mode"] != PARTY_MODE_NATIVE_AI:
        raise ReplayFailure("Solo recording journal did not begin in native-AI mode")
    if party_transition:
        _verify_party_transition_record(record, metadata)
    elif metadata["mode_transitions"]:
        raise ReplayFailure("Native-AI solo recording unexpectedly changed party mode")

    replay_name = _fresh_run_name("replay")
    match_marker = "LIVE_REPLAY MATCH frames=" + str(FRAMES)
    replay = run_owned_game(re.compile(r"^" + re.escape(match_marker) + r"$", re.M),
                            replay_name, args.seconds, journal_path=journal_path)

    mutant_name = _fresh_run_name("mutation")
    mutant_path = result_dir / "live-join-journal-mutated.bin"
    changed = mutate_post_state(data)
    if changed == data or parse_journal(changed) != metadata:
        raise ReplayFailure("PostState mutation was not a structurally valid journal")
    mutant_path.write_bytes(changed)
    mutation = run_owned_game(re.compile(r"^LIVE_REPLAY MISMATCH frame=\d+(?: .*)?$", re.M),
                              mutant_name, args.seconds, journal_path=mutant_path)
    summary = {"result": "PASS", "record": record, "preflight": preflight,
               "journal": journal_path.name, "journal_metadata": metadata,
               "normal_replay": replay, "mutated_replay": mutation,
               "mutation_caught": True}
    (result_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, sort_keys=True))
    return 0


def run(args):
    if os.name != "nt":
        raise ReplayFailure("Windows is required")
    if not 120 <= args.seconds <= 1200:
        raise ReplayFailure("--seconds must be between 120 and 1200")
    name = args.name or _fresh_run_name("live-join")
    if not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", name) or len(name) > 28:
        raise ReplayFailure("--name must be lowercase letters/digits with single hyphens, at most 28 chars")
    record_name = args.record_name or "live-join-record-" + name
    if not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", record_name) or len(record_name) > 48:
        raise ReplayFailure("--record-name must be a safe lifecycle run name of at most 48 chars")
    result_dir = _plain_path(ROOT / "analysis" / ("live-join-replay-" + name), new=True)
    result_dir.mkdir()
    record_started_ns = None
    record_code = 0
    if args.record_name is None:
        record_started_ns = time.time_ns()
        record_env = _clean_record_environment()
        command = [sys.executable, "-I", str(ROOT / "launcher" / "play_lifecycle.py"),
                   "--name", record_name, "--campaign", "--together", "--skip-cutscenes",
                   "--frames", str(FRAMES), "--seconds", "300", "--quiet"]
        record_log = result_dir / "record-lifecycle.log"
        with record_log.open("xb") as output:
            record_process = subprocess.Popen(command, cwd=ROOT, env=record_env,
                                              stdout=output, stderr=subprocess.STDOUT,
                                              creationflags=subprocess.CREATE_NO_WINDOW)
            record_code = record_process.wait()
    journal_path, original, metadata, record_summary = _verify_record_run(
        record_name, record_code, record_started_ns)
    replay_name = _fresh_run_name("replay")
    match_marker = "LIVE_REPLAY MATCH frames=" + str(FRAMES)
    replay = run_owned_game(re.compile(r"^" + re.escape(match_marker) + r"$", re.M),
                            replay_name, args.seconds, journal_path=journal_path)

    mutant_name = _fresh_run_name("mutation")
    mutant_path = result_dir / "live-join-journal-mutated.bin"
    changed = mutate_post_state(original)
    if changed == original or parse_journal(changed) != metadata:
        raise ReplayFailure("PostState mutation was not a structurally valid journal")
    mutant_path.write_bytes(changed)
    mutation = run_owned_game(re.compile(r"^LIVE_REPLAY MISMATCH frame=\d+(?: .*)?$", re.M),
                              mutant_name, args.seconds, journal_path=mutant_path)
    summary = {"result": "PASS", "record_name": record_name,
               "record_common_frames": record_summary["common_frames"],
               "journal": journal_path.name, "journal_metadata": metadata,
               "normal_replay": replay, "mutated_replay": mutation,
               "mutation_caught": True}
    (result_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, sort_keys=True))
    return 0


def _argument_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", help="Optional unique lowercase run label")
    parser.add_argument("--record-name", help="Reuse an existing completed lifecycle record with matching fresh preflight evidence")
    record_options = parser.add_mutually_exclusive_group()
    record_options.add_argument("--solo-record", action="store_true", help="Record native-AI mode from one fresh Russian copy")
    record_options.add_argument("--party-transition-record", action="store_true",
                                help="Record native-AI to human-controller and return transitions from one fresh Russian copy")
    parser.add_argument("--seconds", type=int, default=600, help="Bound for each fresh solo replay process")
    parser.add_argument("--selftest", action="store_true", help="Run pure format, digest, environment and role-file fixtures")
    return parser


def main(argv=None):
    args = _argument_parser().parse_args(argv)
    try:
        if args.selftest:
            if args.record_name or args.solo_record or args.party_transition_record:
                raise ReplayFailure("--selftest excludes recording options")
            return selftest()
        if args.solo_record or args.party_transition_record:
            if args.record_name:
                raise ReplayFailure("Standalone recording excludes --record-name")
            # Party-transition recording uses the solo recorder lifecycle and
            # switches only the research record mode from 2 to 3.
            return run_solo_record(args, party_transition=args.party_transition_record)
        return run(args)
    except (RuntimeError, OSError, ValueError, TypeError, KeyError, subprocess.SubprocessError) as exc:
        print(json.dumps({"result": "FAIL", "error": str(exc)}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
