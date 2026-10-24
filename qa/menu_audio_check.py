"""Private real-game check: main menu Options opens the full Options screen with Audio (research probe build).

Runs the Play Game path (campaign, deferred partner, human controls) in a window with the research menu walk
(COOP_PROBE_MENU_FIXTURE=1): main menu -> Options (cursor must be on Audio) -> Audio -> back -> Save Game entry
(must be ignored) -> back to the main menu, then the game closes itself.
Usage: python -I qa/menu_audio_check.py --name menu-audio-001
"""
import argparse
import importlib.util
import json
import os
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).absolute().parents[1]
CRITICAL = "c46b90afe36571cf3f4fbe3d4e7918da833297f78119d40504e96b4f531b36ea"
PLAY = {"INPUTBOT": "0", "PLAYBOT": "0", "ACTIONBOT": "0", "HUMAN": "1", "HUMAN_FIXTURE": "0", "PATHSYNC": "1",
        "AMBIENT_SPLIT": "1", "CLEAN_EXIT": "1", "NET_FRAMES": "0", "NET_ADDRESS": "127.0.0.1", "NET_PORT": "49375",
        "CONTENT_SHA256": CRITICAL, "CAMPAIGN": "1", "CAMPAIGN_FIXTURE": "0", "NET_MODE": "deferred",
        "DISPLAY_MODE": "window", "RESOLUTION": "1280x720", "MENU_FIXTURE": "1", "DIAGNOSTICS": "1"}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", required=True)
    parser.add_argument("--seconds", type=int, default=240)
    parser.add_argument("--nav", action="store_true", help="Log which applied action moves the main menu cursor")
    parser.add_argument("--continue-save", metavar="FILE", help="Seed this save in slot 1 and check that main-menu volumes survive Continue")
    args = parser.parse_args()
    result = ROOT / "analysis" / args.name
    result.mkdir(parents=True)
    spec = importlib.util.spec_from_file_location("coop_game_lane", ROOT / "qa" / "game_lane.py")
    lane_module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(lane_module)
    lane = lane_module.GameLane(args.seconds + 120).acquire()
    sandbox = result / "play"
    try:
        env = {k: v for k, v in os.environ.items() if not k.upper().startswith("COOP_PROBE_")}
        env.update({"COOP_PROBE_" + k: v for k, v in PLAY.items()})
        if args.nav:
            env["COOP_PROBE_MENU_FIXTURE"] = "3"
        if args.continue_save:
            env["COOP_PROBE_MENU_FIXTURE"] = "2"
            env["COOP_PROBE_PROFILE"] = str(result / "profile")
            saves = result / "profile" / "Documents" / "Activision" / "Spider-Man FoF" / "Saved Games"
            saves.mkdir(parents=True)
            (saves / "SavedGame1.SFOFSaveGame").write_bytes(Path(args.continue_save).read_bytes())
        log = (result / "launcher.log").open("w", encoding="utf-8")
        process = subprocess.Popen([str(ROOT / "probe" / "native-launcher.exe"), "--play",
                                    str(ROOT / "test" / "english" / "Game.exe"), str(sandbox)],
                                   stdout=log, stderr=subprocess.STDOUT, env=env)
        lane.register_launcher(process)
        end = time.monotonic() + args.seconds
        while process.poll() is None and time.monotonic() < end:
            time.sleep(0.5)
        exited = process.poll()
        if exited is None:
            subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"], capture_output=True)
            process.wait(30)
        log.close()
    finally:
        lane.release()
    text = (sandbox / "probe.log").read_text(encoding="utf-8", errors="replace") if (sandbox / "probe.log").exists() else ""
    lines = [l for l in text.splitlines() if "MENU_FIXTURE" in l or "OPTIONS" in l or "KEYBOARD_BINDINGS" in l or "EXCEPTION" in l]
    summary = {"exited": exited, "pass": "MENU_FIXTURE PASS" in text and "FAIL" not in "\n".join(lines),
               "exceptions": text.count("EXCEPTION count="), "lines": lines[-60:]}
    if args.continue_save:
        summary["pass"] = summary["pass"] and "CONTINUE_PASS" in text
    summary["result"] = "PASS" if summary["pass"] and exited is not None and not summary["exceptions"] else "FAIL"
    (result / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps({k: summary[k] for k in ("exited", "exceptions", "result")}))


if __name__ == "__main__":
    main()
