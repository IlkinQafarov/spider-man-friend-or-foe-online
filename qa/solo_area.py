"""Single-player check: one test copy loads one area through the game's own developer test loader, no co-op network.

Used to tell a game-file crash from a co-op crash: if an area crashes alone too, the add-on's networking is not the
cause. A virtual pad only presses through the start-up movies; the run ends after --seconds and reports any EXCEPTION lines from probe.log.

    python -I qa/solo_area.py --name solo-nepal4-001 --area Nepal/4_Level/3_Area --seconds 240
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
CRITICAL = "c46b90afe36571cf3f4fbe3d4e7918da833297f78119d40504e96b4f531b36ea"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", required=True)
    parser.add_argument("--area", required=True)
    parser.add_argument("--language", default="english", choices=("english", "russian"))
    parser.add_argument("--seconds", type=int, default=240)
    args = parser.parse_args()
    if not re.fullmatch(r"[a-z0-9-]{1,48}", args.name) or not re.fullmatch(r"[A-Za-z0-9_/]{1,100}", args.area) or ".." in args.area:
        parser.error("bad --name or --area")
    box = ROOT / ("run-solo-area-" + args.name)
    if box.exists():
        parser.error("run name already used: " + box.name)
    spec = importlib.util.spec_from_file_location("coop_game_lane", ROOT / "qa" / "game_lane.py")
    lane_module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(lane_module)
    env = {key: value for key, value in os.environ.items() if not key.upper().startswith("COOP_PROBE_")}
    env.update({"COOP_PROBE_" + key: value for key, value in {
        "AUTOTEST": "ExecutionPath", "INPUTBOT": "1", "PLAYBOT": "0", "ACTIONBOT": "0", "HUMAN": "0",
        "CLEAN_EXIT": "1", "AREA": args.area, "CONTENT_SHA256": CRITICAL, "DIAGNOSTICS": "1"}.items()})
    lane = lane_module.GameLane(args.seconds + 60).acquire()
    process = None
    try:
        log = (ROOT / ("solo-area-" + args.name + "-launcher.log")).open("w", encoding="utf-8")
        # Research form (no --play): the launcher closes the game itself after the given seconds.
        process = subprocess.Popen([str(ROOT / "probe" / "native-launcher.exe"),
                                    str(ROOT / "test" / args.language / "Game.exe"), str(box), str(args.seconds)],
                                   cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
        lane.register_launcher(process)
        deadline = time.time() + args.seconds + 60
        while time.time() < deadline and process.poll() is None:
            time.sleep(5)
        exited_early = process.poll()
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=30)
        lane.release()
    probe_log = box / "probe.log"
    text = probe_log.read_text(encoding="utf-8", errors="replace") if probe_log.exists() else ""
    exceptions = [line for line in text.splitlines() if line.startswith("EXCEPTION count=")]
    print(json.dumps({"name": args.name, "area": args.area, "exited_early": exited_early,
                      "probe_lines": text.count("\n"), "exceptions": exceptions[:3]}))
    return 1 if exceptions else 0


if __name__ == "__main__":
    sys.exit(main())
