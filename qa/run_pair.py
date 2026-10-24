"""Run the private, bounded English/Russian pair gate through owned launchers."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import time

HERE = Path(__file__).absolute().parent
ROOT = HERE.parent
SEATS = ("host", "peer")
LANGUAGES = ("english", "russian")
EXIT = re.compile(r"^PROCESS_EXIT pid=(\d+) code=(\d+)\s*$", re.M)
READY = re.compile(r"^HOOK_READY pid=(\d+)\b", re.M)
DIVERGENCE = re.compile(
    r"^NET FAIL shared-state divergence observed_tick=(\d+) own=([0-9a-f]{16}) peer=([0-9a-f]{16})$"
)


class Failed(RuntimeError):
    pass


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def no_reparse(path, new=False):
    path = Path(path)
    for part in reversed((path, *path.parents)):
        if new and part == path:
            if part.exists() or part.is_symlink():
                raise Failed("Expected a NEW path: " + part.name)
            continue
        info = part.lstat()
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise Failed("Reparse/symlink rejected: " + part.name)


def clean_environment(source):
    return {key: value for key, value in source.items() if not key.upper().startswith("COOP_PROBE_")}


def environment(source, args, seat, digest):
    env = clean_environment(source)
    options = {
        "AUTOTEST": "ExecutionPath", "INPUTBOT": "0", "PLAYBOT": "0", "ACTIONBOT": "0",
        "HUMAN": "1", "HUMAN_FIXTURE": "1", "PATHSYNC": "1", "AMBIENT_SPLIT": "1",
        "CLEAN_EXIT": "1", "NET_MODE": "host" if seat == "host" else "join",
        "NET_FRAMES": str(args.frames), "NET_ADDRESS": "127.0.0.1", "NET_PORT": "49375",
        "AREA": args.area, "CONTENT_SHA256": digest,
    }
    if args.disconnect_host and seat == "host":
        options["DISCONNECT_FIXTURE"] = "1"
    if args.jitter:
        options["JITTER_FIXTURE"] = "1"
    if args.mutate_host and seat == "host":
        options["MUTATE_FIXTURE"] = "1"
    if args.transition:
        options["TRANSITION_FIXTURE"] = "1"
        options["TRANSITION_COUNT"] = str(args.transition_count)
    if args.pause:
        options["PAUSE_FIXTURE"] = "1"
    if getattr(args, "campaign", False):
        options.pop("AUTOTEST")
        options.pop("AREA")
        options.update(CAMPAIGN="1", CAMPAIGN_FIXTURE="1")
        if seat == "peer" and args.campaign_delay_ms:
            options["CAMPAIGN_DELAY_MS"] = str(args.campaign_delay_ms)
    if getattr(args, "skip_cutscenes", False) and getattr(args, "campaign", False):
        options["SKIP_NIS"] = "1"
    if getattr(args, "mission_end", False):
        options["MISSION_END_FIXTURE"] = "1"
        options["MISSION_CYCLES"] = str(getattr(args, "mission_cycles", 1))
    display = getattr(args, "display_" + seat, None)
    if display:
        mode, _, resolution = display.partition(":")
        options["DISPLAY_MODE"] = mode
        if resolution:
            options["RESOLUTION"] = resolution
    if args.keyboard_host and seat == "host":
        options["KEYBOARD_FIXTURE"] = "1"
    if getattr(args, "swap", False) and seat == "host":
        options["SWAP_FIXTURE"] = "1"
    if args.steam_mock and seat == "peer":
        options["NET_PORT"] = "49376"
    env.update({"COOP_PROBE_" + key: value for key, value in options.items()})
    return env


def actual_exit(text):
    exits, ready = EXIT.findall(text), READY.findall(text)
    if len(exits) != 1 or len(ready) != 1 or exits[0][0] != ready[0]:
        raise Failed("Need one PROCESS_EXIT matching the owned HOOK_READY pid")
    code = int(exits[0][1])
    if not 0 <= code <= 0xffffffff:
        raise Failed("Invalid actual game exit code")
    return code


def stream_result(text, code):
    if code != 0:
        raise Failed("relay did not exit normally")
    records = [json.loads(line) for line in text.splitlines() if line.startswith('{')]
    completed = [record for record in records if record.get('event') == 'STREAM_COMPLETE']
    if len(completed) != 1 or any(record.get('event') == 'RELAY_FAIL' for record in records):
        raise Failed("relay completion missing, duplicated or failed")
    record = completed[0]
    if any(type(record.get(key)) is not int or record[key] <= 0 for key in ('sent', 'received')):
        raise Failed("relay did not carry a complete bidirectional game stream")
    if type(record.get('discarded_after_game_reset')) is not int or record['discarded_after_game_reset'] != 0:
        raise Failed("relay discarded game stream bytes")
    return {key: record[key] for key in ('sent', 'received', 'discarded_after_game_reset')}


def mutation_gate(logs, exits):
    errors = []
    for seat, text in zip(SEATS, logs):
        failures = [line for line in text.splitlines() if "NET FAIL" in line]
        matches = [DIVERGENCE.fullmatch(line) for line in failures]
        if len(matches) != 1 or matches[0] is None or int(matches[0][1]) != 240:
            errors.append(seat + ": expected only shared-state divergence observed_tick=240")
        elif matches[0][2] == matches[0][3]:
            errors.append(seat + ": divergence reported equal states")
        if "SCRIPT_COMPLETE" in text:
            errors.append(seat + ": mutation unexpectedly completed normally")
        remaining = "\n".join(line for line in text.splitlines() if not DIVERGENCE.fullmatch(line))
        if re.search(r"EXCEPTION|TIMEOUT:|FAIL closed|\bFAIL ", remaining, re.I):
            errors.append(seat + ": unexpected exception, failure, or timeout")
    if "NET MUTATION_FIXTURE corrupted sampled state at tick=240" not in logs[0]:
        errors.append("host: mutation fixture marker absent")
    if "NET MUTATION_FIXTURE" in logs[1]:
        errors.append("peer: host-only mutation fixture unexpectedly active")
    if exits != [0, 0]:
        errors.append("both actual game exits must be zero")
    return {"gate": "shared-state-mutation-detection", "result": "FAIL" if errors else "PASS",
            "expected_divergence_tick": 240, "errors": errors,
            "scope": "Same-machine intentional sampled-state corruption and bounded detection only."}


def assess_pair(logs, launcher_logs, exits, launcher_codes, args, assessor):
    if args.mutate_host:
        gate = mutation_gate(logs, exits)
    else:
        extra = {"combat": False} if getattr(args, "campaign", False) else {}
        frames = args.frames
        if getattr(args, "mission_end", False):
            # The finite exchange ends at the mutual hub return, not at a fixed tick.
            ends = [re.findall(r"^NET SCRIPT_COMPLETE frames=([0-9]+);", text, re.M) for text in logs]
            if len(ends) == 2 and all(len(end) == 1 for end in ends) and ends[0] == ends[1] and 0 < int(ends[0][0]) < args.frames:
                frames = int(ends[0][0])
        gate = assessor(logs, frames, exits, native_axes=True, disconnected=args.disconnect_host, transition=args.transition, transition_count=args.transition_count, progress=True, **extra)
    errors = list(gate.get("errors", []))
    if getattr(args, "campaign", False):
        errors.extend(campaign_evidence(logs, args.campaign_delay_ms, getattr(args, "mission_end", False), getattr(args, "mission_cycles", 1)))
        gate["gate"] = "retail-campaign-bootstrap"
        gate["scope"] = "Native New Game, hub and first-mission bootstrap plus bounded sampled-state/movement parity on one PC. Full campaign, persistence, hardware and WAN remain unverified."
        if getattr(args, "mission_end", False):
            gate["gate"] = "retail-campaign-mission-end-return"
            gate["sampled_frames"] = frames
            gate["scope"] = ("Native New Game, hub, first-mission start, fixture-invoked retail mission-end routine, results screen, "
                             "synchronized hub reload and equal progress on one PC. Mission content, later missions, saves, hardware and WAN remain unverified.")
    if args.pause:
        for seat, text in zip(SEATS, logs):
            for assertion in ("ENTERED", "WORLD_FROZEN", "RESUMED", "COMPLETE"):
                marker = r"^PAUSE ASSERT " + assertion + r"(?: |$)"
                if len(re.findall(marker, text, re.M)) != 1:
                    errors.append(seat + ": need exactly one pause assertion " + assertion)
    if getattr(args, "swap", False):
        swaps = [re.findall(r"^NET CHARACTER_SWAP tick=(\d+) host_slot=(\d)", text, re.M) for text in logs]
        if len(swaps[0]) != 2 or swaps[0] != swaps[1] or [slot for _, slot in swaps[0]] != ["1", "0"]:
            errors.append("character swap: both seats need the same two swaps (host slot 1, then 0); got " + repr(swaps))
    if args.keyboard_host and not any(int(count)>0 for count in re.findall(r'HUMAN KEYBOARD_FIXTURE tick=\d+ native_provider=[0-9a-f]+ keys_pressed=(\d+)',logs[0])):
        errors.append('host: native keyboard fixture did not produce bound scan-code presses')
    for index, seat in enumerate(SEATS):
        if launcher_codes[index] != 0:
            errors.append(f"{seat}: launcher exit is {launcher_codes[index]}, expected zero")
        if re.search(r"TIMEOUT:|EXCEPTION|FAIL closed", launcher_logs[index], re.I):
            errors.append(seat + ": launcher recorded timeout, exception, or failure")
    gate["errors"] = errors
    gate["result"] = "FAIL" if errors or gate.get("result") != "PASS" else "PASS"
    return gate


def campaign_evidence(logs, delay_ms=0, mission_end=False, cycles=1):
    errors = []
    if len(logs) != 2:
        errors.append("native campaign evidence requires exactly two seats")
    required = ("NEW_GAME", "PROFILE", "HUB", "MISSION_COMMIT", "GAMEPLAY") + (("RETURN_HUB", "RETURN_HUB_SYNC") if mission_end else ())
    if mission_end and cycles == 2:
        required += ("MISSION_COMMIT", "GAMEPLAY")
    bodies = {
        "NEW_GAME": r"operation=0 selection=0",
        "PROFILE": r"slot=[0-2] native_init=1 operation=0",
        "HUB": r"path=FrontEndHub/FrontEnd/FrontEndHub area=97 flow=16",
        "MISSION_COMMIT": r"path=Tokyo/1_Level/1_Area area=0 pending=32",
        "GAMEPLAY": r"path=Tokyo/1_Level/1_Area area=0 flow=128 sidekick=(?:0|[1-9][0-9]*) prepared=1",
        "RETURN_HUB": r"path=FrontEndHub/FrontEnd/FrontEndHub area=97 flow=16 tick=[0-9]+ mission_end=1",
        "RETURN_HUB_SYNC": r"progress=[0-9a-f]{8} tick=[0-9]+",
    }
    observations = []
    for seat, text in zip(SEATS, logs):
        loads = re.findall(r"^CAMPAIGN PRELOAD_(WAIT|FORWARD) sequence=([0-9]+) path=([^ ]+) area=([0-9]+) (.+)$", text, re.M)
        expected_loads = [(event, str(sequence), path, str(area), suffix)
                          for sequence, path, area in ((1, "FrontEndHub/FrontEnd/FrontEndHub", 97),
                                                       (2, "Tokyo/1_Level/1_Area", 0))
                                                      + (((3, "FrontEndHub/FrontEnd/FrontEndHub", 97),) if mission_end else ())
                                                      + (((4, "Tokyo/1_Level/1_Area", 0),) if mission_end and cycles == 2 else ())
                          for event, suffix in (("WAIT", "original_calls=0"),
                                                ("FORWARD", "peer_ready=1 original_calls=1"))]
        if loads != expected_loads:
            errors.append(seat + ": missing, duplicate, reordered or incorrect native preload readiness/forward evidence")
        hub_forward = text.find("CAMPAIGN PRELOAD_FORWARD sequence=1 ")
        mission_wait = text.find("CAMPAIGN PRELOAD_WAIT sequence=2 ")
        mission_forward = text.find("CAMPAIGN PRELOAD_FORWARD sequence=2 ")
        if not (0 <= hub_forward < text.find("CAMPAIGN ASSERT HUB ") and
                0 <= text.find("CAMPAIGN ASSERT MISSION_COMMIT ") < mission_wait < mission_forward < text.find("CAMPAIGN ASSERT GAMEPLAY ")):
            errors.append(seat + ": native load forwarding is not ordered before the corresponding ready assertions")
        first_tick = re.search(r"^NET state tick=0 ", text, re.M)
        if first_tick and first_tick.start() < text.find("CAMPAIGN ASSERT GAMEPLAY "):
            errors.append(seat + ": gameplay sampling began before native campaign readiness")
        delays = re.findall(r"^CAMPAIGN PRELOAD_DELAY delay_ms=([0-9]+)$", text, re.M)
        if delays != ([str(delay_ms)] if seat == "peer" and delay_ms else []):
            errors.append(seat + ": configured peer preload delay was not observed exactly once")
        if delays and text.find("CAMPAIGN PRELOAD_DELAY") > text.find("CAMPAIGN PRELOAD_WAIT"):
            errors.append(seat + ": preload delay occurred after the first load wait")
        markers = re.findall(r"^CAMPAIGN ASSERT ([A-Z_]+)(?: ([^\r\n]*))?$", text, re.M)
        selected = [(kind, data) for kind, data in markers if kind in required]
        if tuple(kind for kind, _ in selected) != required:
            errors.append(seat + ": need exactly one ordered native campaign assertion for each required phase")
        for kind, data in selected:
            if re.fullmatch(bodies[kind], data) is None:
                errors.append(seat + ": invalid native campaign assertion body for " + kind)
        if len(re.findall(r"^CAMPAIGN CONFIG enabled=1 fixture=1 research_autotest=0$", text, re.M)) != 1:
            errors.append(seat + ": explicit retail campaign configuration absent")
        if re.search(r"AUTOTEST selected|TRANSITION PARTY_INSIDE_REAL_EXIT|TRANSITION AREA_CHANGED", text):
            errors.append(seat + ": research bootstrap/travel fixture contaminated the campaign gate")
        if mission_end:
            invoked = re.findall(r"^MISSION_END INVOKED (tick=[0-9]+ scene=8 flow=128)$", text, re.M)
            if len(invoked) != 1:
                errors.append(seat + ": mission-end routine must open results scene 8 exactly once during gameplay")
            selected = selected + [("MISSION_END", invoked[0] if invoked else "")]
        observations.append(selected)
    if len(observations) != 2 or observations[0] != observations[1]:
        errors.append("native campaign choices/readiness differ between peers")
    return errors


def cleanup_owned(processes, deadline):
    """Only handles returned by this run's Popen calls can reach this function."""
    alive = [process for process in processes if process.poll() is None]
    for process in alive:
        try:
            process.terminate()  # Closing this owned launcher closes its kill-on-close job.
        except OSError:
            pass
    for process in alive:
        try:
            process.wait(timeout=min(0.5, max(0.0, deadline - time.monotonic())))
        except subprocess.TimeoutExpired:
            try:
                process.kill()
            except OSError:
                pass
    for process in alive:
        try:
            process.wait(timeout=max(0.0, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            pass


def check_stop_request(result_dir):
    request = result_dir / "stop.request"
    if request.exists() or request.is_symlink():
        no_reparse(request)
        if not request.is_file():
            raise Failed("Invalid local QA stop request")
        raise Failed("CANCELLED: local QA observation stopped before scenario completion")


def run_suite(args, result_dir):
    launcher = ROOT / "probe" / "native-launcher.exe"
    preflight = ROOT / "compatibility" / "preflight.py"
    scorer = ROOT / "analysis" / "native" / "score_pair.py"
    sandboxes = [ROOT / f"run-{args.name}-{seat}" for seat in SEATS]
    records = [ROOT / "compatibility" / f"qa-{args.name}-{seat}.json" for seat in SEATS]
    summary = {
        "schema": "coop-fof-pair-run-v1", "name": args.name, "result": "FAIL", "errors": [],
        "options": {key: getattr(args, key) for key in (
            "frames", "seconds", "area", "disconnect_host", "jitter", "mutate_host", "transition", "transition_count", "keyboard_host", "steam_mock", "pause", "campaign", "campaign_delay_ms", "mission_end", "mission_cycles", "display_host", "display_peer")},
        "runs": [path.name for path in sandboxes], "preflight": [],
        "launcher_returncodes": [None, None], "game_exitcodes": [None, None],
    }
    processes, streams, helpers = [], [], []
    deadline = None
    lane = None
    try:
        lane_path = ROOT / "qa" / "game_lane.py"
        no_reparse(lane_path)
        lane_module = load_module("coop_game_lane", lane_path)
        for path in (launcher, preflight, scorer, ROOT / "probe" / "native-probe.dll"):
            no_reparse(path)
            if not path.is_file():
                raise Failed("Missing required file: " + path.name)
        for path in (*sandboxes, *records):
            no_reparse(path, new=True)
        creationflags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
        base_env = clean_environment(os.environ)
        preflight_errors = []
        for seat, language, output in zip(SEATS, LANGUAGES, records):
            command = [sys.executable, str(preflight), "verify", "--game-dir", str(ROOT / "test" / language),
                       "--output", str(output)]
            entry = {"seat": seat, "record": output.name, "returncode": None}
            summary["preflight"].append(entry)
            with (result_dir / f"{seat}-preflight.log").open("x", encoding="utf-8") as stream:
                try:
                    completed = subprocess.run(command, cwd=ROOT, env=base_env, stdout=stream,
                                               stderr=subprocess.STDOUT, creationflags=creationflags,
                                               timeout=args.seconds + 15, check=False)
                    entry["returncode"] = completed.returncode
                except subprocess.TimeoutExpired:
                    preflight_errors.append(seat + ": preflight timeout")
                    continue
            if completed.returncode != 0 or not output.is_file():
                preflight_errors.append(seat + ": content preflight rejected")
                continue
            no_reparse(output)
            verified = json.loads(output.read_text(encoding="utf-8"))
            if (verified.get("status") != "pass" or verified.get("schema") != "coop-fof-preflight-v1"
                    or not re.fullmatch(r"[0-9a-f]{64}", verified.get("critical_digest", ""))
                    or verified.get("language") not in ("English", "Russian")):
                preflight_errors.append(seat + ": invalid preflight result")
                continue
            entry.update({key: verified[key] for key in ("language", "critical_digest", "locale_digest")})
        if preflight_errors:
            raise Failed("; ".join(preflight_errors))
        if summary["preflight"][0]["critical_digest"] != summary["preflight"][1]["critical_digest"]:
            raise Failed("Critical preflight digests differ")
        assessor = load_module("coop_pair_scorer", scorer).assess
        lane = lane_module.GameLane(args.seconds + 15).acquire()
        summary["gameslot"] = {"owner": lane.owner, "slots": lane.slots, "ttl_seconds": lane.ttl}
        # Reserve the final two seconds of this global launcher deadline for owned-handle cleanup.
        deadline = time.monotonic() + args.seconds + 15
        if args.steam_mock:
            relay = ROOT / 'steam' / 'relay.py'; broker = ROOT / 'steam' / 'mock_service.py'
            for path in (relay, broker): no_reparse(path)
            def helper(name, command):
                stream = (result_dir / f'steam-{name}.log').open('xb'); streams.append(stream)
                process = subprocess.Popen(command, cwd=ROOT, env=base_env, stdout=stream,
                    stderr=subprocess.STDOUT, creationflags=creationflags)
                helpers.append(process)
            helper('broker', [sys.executable, str(broker), '--port', '49480', '--seconds', str(args.seconds+10)])
            ready_deadline = time.monotonic()+5
            while time.monotonic()<ready_deadline:
                if helpers[0].poll() is not None: raise Failed('Mock broker failed')
                if 'MOCK_BROKER_READY' in (result_dir/'steam-broker.log').read_text(errors='replace'): break
                time.sleep(0.02)
            else: raise Failed('Mock broker readiness timeout')
            ready = result_dir/'steam-host-ready.json'
            helper('host', [sys.executable, str(relay), 'host', '--mock', '49480', '--local-port', '49375',
                '--seconds', str(args.seconds+10), '--ready-file', str(ready)])
            ready_deadline = time.monotonic()+5; lobby = None
            while time.monotonic()<ready_deadline:
                if helpers[1].poll() is not None: raise Failed('Mock Steam host failed')
                try:
                    if ready.exists(): lobby=json.loads(ready.read_text())['lobby']; break
                except json.JSONDecodeError: pass
                time.sleep(0.02)
            if not lobby: raise Failed('Mock Steam lobby readiness timeout')
            helper('peer', [sys.executable, str(relay), 'join', '--mock', '49480', '--lobby', str(lobby),
                '--local-port', '49376', '--seconds', str(args.seconds+10)])
        for index, seat in enumerate(SEATS):
            stream = (result_dir / f"{seat}-launcher.log").open("xb")
            streams.append(stream)
            command = [str(launcher), str(ROOT / "test" / LANGUAGES[index] / "Game.exe"),
                       str(sandboxes[index]), str(args.seconds)]
            process = subprocess.Popen(command, cwd=ROOT, env=environment(
                base_env, args, seat, summary["preflight"][index]["critical_digest"]),
                stdout=stream, stderr=subprocess.STDOUT, creationflags=creationflags)
            processes.append(process)
            lane.register_launcher(process)
        while any(process.poll() is None for process in processes):
            check_stop_request(result_dir)
            if helpers and any(process.poll() not in (None, 0) for process in helpers):
                raise Failed('A mock Steam helper failed; stopping only owned processes')
            if any(process.poll() not in (None, 0) for process in processes):
                raise Failed("A launcher failed; stopping only the other owned launcher")
            if time.monotonic() >= deadline - 2:
                raise Failed("TIMEOUT: paired launchers exceeded the bounded run deadline")
            time.sleep(0.1)
        if helpers:
            helper_deadline = min(deadline-2, time.monotonic()+8)
            while any(process.poll() is None for process in helpers[1:]) and time.monotonic()<helper_deadline:
                time.sleep(0.02)
            summary['relay_streams'] = []
            for seat, process in zip(SEATS, helpers[1:]):
                output = (result_dir/f'steam-{seat}.log').read_text(errors='replace')
                record = stream_result(output, process.poll())
                summary['relay_streams'].append({'seat': seat, 'returncode': process.poll(), **record})
            host_stream, peer_stream = summary['relay_streams']
            if host_stream['sent'] != peer_stream['received'] or peer_stream['sent'] != host_stream['received']:
                raise Failed('Relay streams do not have reciprocal byte counts')
            summary['transport'] = 'mock-steam-contract-loopback (not actual Steam/SDR)'
    except (RuntimeError, OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as exc:
        summary["errors"].append(str(exc))
    except KeyboardInterrupt:
        summary["errors"].append("Interrupted; stopping only owned launchers")
    finally:
        if processes or helpers:
            cleanup_owned(processes + helpers, deadline if deadline is not None else time.monotonic() + 2)
        for stream in streams:
            stream.close()
        for index, process in enumerate(processes):
            summary["launcher_returncodes"][index] = process.poll()
        if lane is not None:
            try:
                lane.release(children_stopped=all(p.poll() is not None for p in processes))
            except RuntimeError as exc:
                summary["errors"].append(str(exc))
    launcher_logs, logs = [], []
    for index, seat in enumerate(SEATS):
        launcher_log = result_dir / f"{seat}-launcher.log"
        launcher_text = launcher_log.read_text(encoding="utf-8", errors="replace") if launcher_log.exists() else ""
        launcher_logs.append(launcher_text)
        probe_log = sandboxes[index] / "probe.log"
        if probe_log.exists():
            no_reparse(probe_log)
            logs.append(probe_log.read_text(encoding="utf-8", errors="replace"))
        else:
            logs.append("")
        if launcher_text:
            try:
                summary["game_exitcodes"][index] = actual_exit(launcher_text)
            except Failed as exc:
                summary["errors"].append(seat + ": " + str(exc))
    if len(processes) == 2 and not summary["errors"]:
        gate = assess_pair(logs, launcher_logs, summary["game_exitcodes"],
                           summary["launcher_returncodes"], args, assessor)
        summary["gate"] = gate
        summary["errors"].extend(gate["errors"])
        summary["result"] = gate["result"]
    return summary


def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name")
    parser.add_argument("--frames", type=int, default=3600)
    parser.add_argument("--seconds", type=int, default=240)
    parser.add_argument("--area", default="Tokyo/1_Level/1_Area")
    parser.add_argument("--disconnect-host", action="store_true")
    parser.add_argument("--jitter", action="store_true")
    parser.add_argument("--mutate-host", action="store_true")
    parser.add_argument("--transition", action="store_true")
    parser.add_argument("--transition-count", type=int, default=1)
    parser.add_argument("--keyboard-host", action="store_true")
    parser.add_argument("--steam-mock", action="store_true")
    parser.add_argument("--pause", action="store_true", help="Require native pause/frozen-world/resume assertions on both seats")
    parser.add_argument("--campaign", action="store_true", help="Require native retail New Game/hub/mission bootstrap; excludes the research route")
    parser.add_argument("--campaign-delay-ms", type=int, default=0, help="Delay peer's first campaign preload readiness by 0..2000ms")
    parser.add_argument("--mission-end", action="store_true", help="Campaign only: invoke the retail mission-end routine on both seats at tick1500 (fixture)")
    parser.add_argument("--mission-cycles", type=int, default=1, help="With --mission-end: 2 = mission end, back to the hub, then a second mission until --frames")
    parser.add_argument("--display-host", help="borderless[:WIDTHxHEIGHT] or window:WIDTHxHEIGHT for the host seat")
    parser.add_argument("--display-peer", help="borderless[:WIDTHxHEIGHT] or window:WIDTHxHEIGHT for the peer seat")
    parser.add_argument("--swap", action="store_true", help="Host holds the character-switch action twice; both seats must swap on the same ticks")
    parser.add_argument("--skip-cutscenes", action="store_true", help="Campaign bots skip movies the game lets players skip (faster runs)")
    parser.add_argument("--selftest", action="store_true", help="Own-code fixtures only; no preflights or launches")
    args = parser.parse_args(argv)
    if args.selftest:
        return args
    if not args.name or len(args.name) > 64 or not re.fullmatch(r"[A-Za-z0-9]+(?:-[A-Za-z0-9]+)*", args.name):
        parser.error("--name must contain 1..64 letters/digits with single hyphens between groups")
    # Retail campaign bootstrap includes the 118 s intro movie and a long first-level load.
    max_seconds = 1190 if args.campaign else 300  # mock broker/relay get seconds+10, capped at 1200
    if not 60 <= args.frames <= 12000 or not 60 <= args.seconds <= max_seconds:
        parser.error("--frames must be 60..12000 and --seconds must be 60..300 (60..1190 with --campaign)")
    if args.pause and (args.frames <= 430 or args.mutate_host or args.disconnect_host):
        parser.error("--pause requires --frames greater than430 and excludes mutation/disconnect fixtures")
    if args.mission_cycles not in (1, 2) or (args.mission_cycles != 1 and not args.mission_end) or (args.mission_end and not args.campaign):
        parser.error("--mission-cycles must be 1 or 2 and requires --mission-end, which requires --campaign")
    for value in (args.display_host, args.display_peer):
        if value is not None and not re.fullmatch(r"borderless(?::[0-9]{3,4}x[0-9]{3,4})?|window:[0-9]{3,4}x[0-9]{3,4}", value):
            parser.error("--display-host/--display-peer must be borderless[:WxH] or window:WxH")
    if not 0 <= args.campaign_delay_ms <= 2000 or (args.campaign_delay_ms and not args.campaign):
        parser.error("--campaign-delay-ms must be 0..2000 and requires --campaign")
    if args.campaign and (args.frames < 600 or args.pause or args.disconnect_host or args.mutate_host or args.area != "Tokyo/1_Level/1_Area"):
        parser.error("--campaign requires at least600 ticks and excludes research area/pause/mutation/disconnect options")
    if not 1 <= args.transition_count <= 4 or (args.transition_count != 1 and not args.transition):
        parser.error("--transition-count must be1..4 and requires --transition when greater than1")
    if len(args.area) > 127 or not re.fullmatch(r"[A-Za-z0-9_]+(?:/[A-Za-z0-9_]+)*", args.area):
        parser.error("--area must be an ASCII game area path of at most127 characters")
    return args


def selftest():
    from types import SimpleNamespace
    from unittest.mock import Mock, patch
    complete = {'event': 'STREAM_COMPLETE', 'sent': 128, 'received': 256, 'discarded_after_game_reset': 0}
    assert stream_result(json.dumps(complete), 0)['sent'] == 128
    for text, code in (('', 0), (json.dumps(complete), 1),
            (json.dumps({**complete, 'discarded_after_game_reset': 1}), 0),
            (json.dumps({**complete, 'sent': 0}), 0),
            (json.dumps(complete)+'\n'+json.dumps(complete), 0)):
        try: stream_result(text, code)
        except Failed: pass
        else: raise AssertionError('Incomplete/discarded relay stream accepted')
    args = SimpleNamespace(frames=3600, seconds=240, area="Tokyo/1_Level/1_Area",
                           disconnect_host=True, jitter=True, mutate_host=True, transition=False, transition_count=1, keyboard_host=False, steam_mock=False, pause=False)
    inherited = {"PATH": "kept", "COOP_PROBE_UNREVIEWED": "bad", "coop_probe_game_seconds": "90"}
    host, peer = [environment(inherited, args, seat, "a" * 64) for seat in SEATS]
    assert host["PATH"] == "kept" and "COOP_PROBE_UNREVIEWED" not in host
    assert "coop_probe_game_seconds" not in peer and "COOP_PROBE_GAME_SECONDS" not in host
    assert host["COOP_PROBE_MUTATE_FIXTURE"] == host["COOP_PROBE_DISCONNECT_FIXTURE"] == "1"
    assert "COOP_PROBE_MUTATE_FIXTURE" not in peer and "COOP_PROBE_DISCONNECT_FIXTURE" not in peer
    assert all(env["COOP_PROBE_JITTER_FIXTURE"] == "1" for env in (host, peer))
    assert all(env["COOP_PROBE_INPUTBOT"] == env["COOP_PROBE_PLAYBOT"] == env["COOP_PROBE_ACTIONBOT"] == "0" for env in (host, peer))
    assert actual_exit("HOOK_READY pid=42 sha256=abc\nPROCESS_EXIT pid=42 code=0\n") == 0
    for text in ("", "HOOK_READY pid=42\nPROCESS_EXIT pid=43 code=0\n",
                 "HOOK_READY pid=42\nPROCESS_EXIT pid=42 code=0\nPROCESS_EXIT pid=42 code=124\n"):
        try:
            actual_exit(text)
        except Failed:
            pass
        else:
            raise AssertionError("Bad actual exit accepted")
    divergence = "NET FAIL shared-state divergence observed_tick=240 own=0000000000000001 peer=0000000000000002\n"
    marker = "NET MUTATION_FIXTURE corrupted sampled state at tick=240\n"
    good = [marker + divergence, divergence]
    assert mutation_gate(good, [0, 0])["result"] == "PASS"
    bad_cases = [([marker + divergence, ""], [0, 0]),
                 ([marker + divergence.replace("tick=240", "tick=241"), divergence], [0, 0]),
                 ([good[0] + "NET SCRIPT_COMPLETE", good[1]], [0, 0]),
                 ([good[0], good[1] + "EXCEPTION test"], [0, 0]),
                 ([good[0], good[1] + "NET FAIL frame=244 transport"], [0, 0]),
                 (good, [0, 124])]
    for logs, exits in bad_cases:
        assert mutation_gate(logs, exits)["result"] == "FAIL"
    assert assess_pair(good, ["TIMEOUT: fixture", ""], [0, 0], [0, 0], args, None)["result"] == "FAIL"
    assert assess_pair(good, ["", ""], [0, 0], [5, 0], args, None)["result"] == "FAIL"
    args.mutate_host = False
    calls = []
    def fake_assess(*a, **kw):
        calls.append(kw)
        return {"result": "PASS", "errors": []}
    assert assess_pair(["", ""], ["", ""], [0, 0], [0, 0], args, fake_assess)["result"] == "PASS"
    assert calls == [{"native_axes": True, "disconnected": True, "transition": False, "transition_count": 1, "progress": True}]
    args.pause = True
    host, peer = [environment(inherited, args, seat, "a" * 64) for seat in SEATS]
    assert all(env["COOP_PROBE_PAUSE_FIXTURE"] == "1" for env in (host, peer))
    assertions = "\n".join("PAUSE ASSERT " + item + " tick=" + str(tick) for item, tick in
        (("ENTERED", 243), ("WORLD_FROZEN", 332), ("RESUMED", 404), ("COMPLETE", 430)))
    assert assess_pair([assertions, assertions], ["", ""], [0, 0], [0, 0], args, fake_assess)["result"] == "PASS"
    for damaged in ("", assertions.replace("PAUSE ASSERT RESUMED", "PAUSE OTHER RESUMED"),
                    assertions + "\nPAUSE ASSERT COMPLETE tick=430"):
        assert assess_pair([assertions, damaged], ["", ""], [0, 0], [0, 0], args, fake_assess)["result"] == "FAIL"
    assert arguments(["--name", "fixture", "--frames", "900", "--pause"]).pause
    args.pause = False
    campaign_args = arguments(["--name", "campaign-fixture", "--frames", "600", "--seconds", "300",
                               "--campaign", "--campaign-delay-ms", "500"])
    host, peer = [environment({**inherited, "COOP_PROBE_AUTOTEST": "ExecutionPath",
                              "COOP_PROBE_CAMPAIGN_DELAY_MS": "2000"}, campaign_args, seat, "a" * 64)
                  for seat in SEATS]
    assert all("COOP_PROBE_AUTOTEST" not in env and "COOP_PROBE_AREA" not in env for env in (host, peer))
    assert all(env["COOP_PROBE_CAMPAIGN"] == env["COOP_PROBE_CAMPAIGN_FIXTURE"] == "1" for env in (host, peer))
    assert "COOP_PROBE_CAMPAIGN_DELAY_MS" not in host and peer["COOP_PROBE_CAMPAIGN_DELAY_MS"] == "500"
    phases = ["NEW_GAME operation=0 selection=0", "PROFILE slot=0 native_init=1 operation=0",
              "HUB path=FrontEndHub/FrontEnd/FrontEndHub area=97 flow=16",
              "MISSION_COMMIT path=Tokyo/1_Level/1_Area area=0 pending=32",
              "GAMEPLAY path=Tokyo/1_Level/1_Area area=0 flow=128 sidekick=1 prepared=1"]
    def load_pair(sequence, path, area):
        return "\n".join(f"CAMPAIGN PRELOAD_{event} sequence={sequence} path={path} area={area} {suffix}"
                          for event, suffix in (("WAIT", "original_calls=0"), ("FORWARD", "peer_ready=1 original_calls=1"))) + "\n"
    campaign = "CAMPAIGN CONFIG enabled=1 fixture=1 research_autotest=0\n" + load_pair(1, "FrontEndHub/FrontEnd/FrontEndHub", 97)
    campaign += "\n".join("CAMPAIGN ASSERT " + phase for phase in phases[:-1]) + "\n"
    campaign += load_pair(2, "Tokyo/1_Level/1_Area", 0) + "CAMPAIGN ASSERT " + phases[-1] + "\n"
    assert not campaign_evidence([campaign, campaign])
    for damaged in (campaign.replace("native_init=1", "native_init=0"),
                    campaign.replace("slot=0", "slot=3"), campaign.replace("flow=16", "flow=128"),
                    campaign.replace("sidekick=1", "sidekick=01"),
                    campaign.replace("fixture=1", "fixture=0"),
                    campaign.replace("original_calls=0", "original_calls=1"),
                    campaign.replace("peer_ready=1", "peer_ready=0"),
                    campaign.replace("sequence=2", "sequence=3"),
                    campaign.replace("MISSION_COMMIT", "UNKNOWN"),
                    campaign + "CAMPAIGN ASSERT " + phases[0] + "\n",
                    campaign + "AUTOTEST selected ExecutionPath\n",
                    campaign.replace("CAMPAIGN ASSERT " + phases[0], "CAMPAIGN ASSERT " + phases[1], 1)):
        assert campaign_evidence([damaged, damaged])
    assert campaign_evidence([campaign, campaign.replace("sidekick=1", "sidekick=2")])
    assert campaign_evidence([campaign, campaign.replace("PROFILE slot=0", "PROFILE slot=1")])
    assert not campaign_evidence([campaign.replace("PROFILE slot=0", "PROFILE slot=2")] * 2)
    assert campaign_evidence([campaign, campaign, campaign])
    calls.clear()
    delayed_campaign = campaign.replace("CAMPAIGN PRELOAD_WAIT sequence=1", "CAMPAIGN PRELOAD_DELAY delay_ms=500\nCAMPAIGN PRELOAD_WAIT sequence=1")
    assert not campaign_evidence([campaign, delayed_campaign], 500)
    assert campaign_evidence([campaign, delayed_campaign], 600)
    assert campaign_evidence([campaign, campaign], 500)
    assert assess_pair([campaign, delayed_campaign], ["", ""], [0, 0], [0, 0], campaign_args, fake_assess)["result"] == "PASS"
    assert calls[0]["combat"] is False
    finished = Mock()
    finished.poll.return_value = 0
    alive = Mock()
    alive.poll.return_value = None
    alive.wait.return_value = -15
    cleanup_owned([finished, alive], time.monotonic() + 1)
    finished.terminate.assert_not_called()
    alive.terminate.assert_called_once()
    folder, request = Mock(), Mock()
    folder.__truediv__ = Mock(return_value=request)
    request.exists.return_value = request.is_symlink.return_value = False
    check_stop_request(folder)
    request.exists.return_value = request.is_file.return_value = True
    with patch.dict(check_stop_request.__globals__, {"no_reparse": Mock()}):
        try:
            check_stop_request(folder)
        except Failed as exc:
            assert str(exc).startswith("CANCELLED:")
        else:
            raise AssertionError("Local stop request did not fail the run")
    print("RUNNER_SELFTEST_PASS environment/exit/mutation/scorer/pause/campaign/owned-cleanup fixtures")
    return 0


def main(argv=None):
    args = arguments(argv)
    if args.selftest:
        return selftest()
    result_dir = HERE / ("result-" + args.name)
    try:
        no_reparse(result_dir, new=True)
        result_dir.mkdir()
        summary = run_suite(args, result_dir)
        with (result_dir / "summary.json").open("x", encoding="utf-8") as stream:
            json.dump(summary, stream, indent=2)
            stream.write("\n")
    except (Failed, OSError, ValueError, KeyError, TypeError) as exc:
        summary = {"schema": "coop-fof-pair-run-v1", "name": args.name,
                   "result": "FAIL", "errors": [str(exc)]}
    print(json.dumps(summary, indent=2))
    return int(summary["result"] != "PASS")


if __name__ == "__main__":
    raise SystemExit(main())
