"""Private smoke check for the player edition probe (probe/player/native-probe.dll) on the real game.

Runs the Play Game path (campaign, deferred partner, human controls) in a window for a few seconds and checks
that the player edition starts; then asks for a test bot and checks that the add-on refuses to start.
It swaps probe/native-probe.dll for the player build and always restores the original afterwards.
Usage: python -I qa/player_smoke.py --name player-smoke-001
"""
import argparse
import importlib.util
import json
import os
import shutil
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).absolute().parents[1]
CRITICAL = "c46b90afe36571cf3f4fbe3d4e7918da833297f78119d40504e96b4f531b36ea"
PLAY = {"INPUTBOT": "0", "PLAYBOT": "0", "ACTIONBOT": "0", "HUMAN": "1", "HUMAN_FIXTURE": "0", "PATHSYNC": "1",
        "AMBIENT_SPLIT": "1", "CLEAN_EXIT": "1", "NET_FRAMES": "0", "NET_ADDRESS": "127.0.0.1", "NET_PORT": "49375",
        "CONTENT_SHA256": CRITICAL, "CAMPAIGN": "1", "CAMPAIGN_FIXTURE": "0", "NET_MODE": "deferred",
        "DISPLAY_MODE": "window", "RESOLUTION": "1280x720"}


def run(lane, sandbox, extra, seconds):
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith("COOP_PROBE_")}
    env.update({"COOP_PROBE_" + k: v for k, v in {**PLAY, **extra}.items()})
    log = (sandbox.parent / (sandbox.name + "-launcher.log")).open("w", encoding="utf-8")
    process = subprocess.Popen([str(ROOT / "probe" / "native-launcher.exe"), "--play",
                                str(ROOT / "test" / "english" / "Game.exe"), str(sandbox)],
                               stdout=log, stderr=subprocess.STDOUT, env=env)
    lane.register_launcher(process)
    end = time.monotonic() + seconds
    while process.poll() is None and time.monotonic() < end:
        time.sleep(0.5)
    exited = process.poll()
    if exited is None:
        subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"], capture_output=True)
        process.wait(30)
    log.close()
    probe = sandbox / "probe.log"
    text = probe.read_text(encoding="utf-8", errors="replace") if probe.exists() else ""
    return exited, text


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", required=True)
    parser.add_argument("--seconds", type=int, default=45)
    args = parser.parse_args()
    result = ROOT / "analysis" / args.name
    result.mkdir(parents=True)
    spec = importlib.util.spec_from_file_location("coop_game_lane", ROOT / "qa" / "game_lane.py")
    lane_module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(lane_module)
    dll, player = ROOT / "probe" / "native-probe.dll", ROOT / "probe" / "player" / "native-probe.dll"
    backup = result / "native-probe.dll.research"
    shutil.copy2(dll, backup)
    summary = {}
    lane = lane_module.GameLane(args.seconds * 2 + 120).acquire()
    try:
        shutil.copy2(player, dll)
        exited, text = run(lane, result / "play", {}, args.seconds)
        summary["play"] = {"exited_early": exited, "player_edition": "FOF_PLAYER_EDITION" in text,
                           "campaign": "CAMPAIGN CONFIG enabled=1" in text,
                           "deferred_wait": "NET DEFERRED_WAIT" in text, "probe_lines": text.count("\n")}
        exited, text = run(lane, result / "bot", {"INPUTBOT": "1"}, 30)
        summary["bot_request"] = {"exited": exited, "probe_attached": "probe attach" in text,
                                  "player_edition_logged": "FOF_PLAYER_EDITION" in text}
    finally:
        shutil.copy2(backup, dll)
        lane.release()
    play, bot = summary["play"], summary["bot_request"]
    ok = (play["exited_early"] is None and play["player_edition"] and play["campaign"] and play["deferred_wait"]
          and bot["exited"] is not None and not bot["probe_attached"])
    summary["result"] = "PASS" if ok else "FAIL"
    (result / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary))


if __name__ == "__main__":
    main()
