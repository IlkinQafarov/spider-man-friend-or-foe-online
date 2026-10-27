"""Private smoke check for the player edition probe on the real game.

The game is closed only by WM_CLOSE to a retained, verified child process handle.
The player DLL and QA lease are preserved if graceful cleanup cannot be confirmed.
Usage: python -I qa/player_smoke.py --name player-smoke-001
"""
import argparse
import importlib.util
import json
import os
import re
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
GAME_EXE = ROOT / "test" / "english" / "Game.exe"


def read_text(path):
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def wait_for(process, game, api, seconds):
    """Observe the owned launcher/game; close an active game via its verified window only."""
    end = time.monotonic() + seconds
    while process.poll() is None and time.monotonic() < end:
        time.sleep(0.25)
    game_exit = api.exit(game) if game is not None else None
    if game is not None and game_exit is None:
        sent = api.close_windows([game])
        if sent.get(game["pid"], 0) < 1:
            return {"game_exit": None, "launcher_exit": process.poll(),
                    "cleanup_error": "No WM_CLOSE could be posted to the verified owned game window"}
        close_end = time.monotonic() + 30
        while time.monotonic() < close_end:
            game_exit = api.exit(game)
            if game_exit is not None:
                break
            time.sleep(0.1)
        if game_exit is None:
            return {"game_exit": None, "launcher_exit": process.poll(),
                    "cleanup_error": "Verified owned game did not exit after WM_CLOSE"}
    try:
        process.wait(timeout=30)
    except subprocess.TimeoutExpired:
        return {"game_exit": game_exit, "launcher_exit": process.poll(),
                "cleanup_error": "Owned launcher did not exit after normal game close"}
    return {"game_exit": game_exit, "launcher_exit": process.returncode, "cleanup_error": None}


def run(lane, sandbox, extra, seconds, api, lifecycle, state):
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith("COOP_PROBE_")}
    env.update({"COOP_PROBE_" + k: v for k, v in {**PLAY, **extra}.items()})
    launcher_log_path = sandbox.parent / (sandbox.name + "-launcher.log")
    probe_text = ""
    game = None
    with launcher_log_path.open("w", encoding="utf-8") as log:
        process = subprocess.Popen([str(ROOT / "probe" / "native-launcher.exe"), "--play",
                                    str(GAME_EXE), str(sandbox)],
                                   stdout=log, stderr=subprocess.STDOUT, env=env)
        state["launchers"].append(process)
        lane.register_launcher(process)
        end = time.monotonic() + seconds
        while process.poll() is None and time.monotonic() < end:
            launcher_text = read_text(launcher_log_path)
            matches = lifecycle.HOOK.findall(launcher_text)
            if len(matches) > 1:
                raise RuntimeError("Duplicate owned HOOK_READY in launcher log")
            if matches and game is None:
                game = api.capture(int(matches[0][0]), process, GAME_EXE)
                state["games"].append(game)
            time.sleep(0.25)
        exited_before_deadline = process.poll() is not None
        cleanup = wait_for(process, game, api, 0)
        # If the process window ended unexpectedly while its game remained, request normal close now.
        if cleanup["cleanup_error"] is None and game is not None and cleanup["game_exit"] is None:
            cleanup = wait_for(process, game, api, 0)
        probe_text = read_text(sandbox / "probe.log")
        launcher_text = read_text(launcher_log_path)
    if game is not None and cleanup["game_exit"] is not None:
        api.kernel.CloseHandle(game["handle"])
        game["handle_closed"] = True
    return {"early_exit": process.returncode if exited_before_deadline else None,
            "game_exit": cleanup["game_exit"], "launcher_exit": cleanup["launcher_exit"],
            "cleanup_error": cleanup["cleanup_error"], "probe_text": probe_text,
            "launcher_text": launcher_text, "game_captured": game is not None}


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
    lifecycle_spec = importlib.util.spec_from_file_location("coop_play_lifecycle", ROOT / "launcher" / "play_lifecycle.py")
    lifecycle = importlib.util.module_from_spec(lifecycle_spec)
    lifecycle_spec.loader.exec_module(lifecycle)
    api = lifecycle.Win32()
    dll, player = ROOT / "probe" / "native-probe.dll", ROOT / "probe" / "player" / "native-probe.dll"
    backup = result / "native-probe.dll.research"
    shutil.copy2(dll, backup)
    summary = {}
    state = {"launchers": [], "games": []}
    lane = lane_module.GameLane(args.seconds * 2 + 180).acquire()
    restore_safe = False
    try:
        shutil.copy2(player, dll)
        played = run(lane, result / "play", {}, args.seconds, api, lifecycle, state)
        play_text = played["probe_text"]
        summary["play"] = {"early_exit": played["early_exit"], "game_exit": played["game_exit"],
                           "launcher_exit": played["launcher_exit"],
                           "player_edition": "FOF_PLAYER_EDITION" in play_text,
                           "campaign": "CAMPAIGN CONFIG enabled=1" in play_text,
                           "deferred_wait": "NET DEFERRED_WAIT" in play_text,
                           "live_join_enabled": "LIVE_JOIN PLAYER enabled=1" in play_text,
                           "exceptions": len(re.findall(r"EXCEPTION", play_text + played["launcher_text"], re.I)),
                           "cleanup_error": played["cleanup_error"],
                           "probe_lines": play_text.count("\n")}
        if played["cleanup_error"] is None:
            rejected = run(lane, result / "bot", {"INPUTBOT": "1"}, 30, api, lifecycle, state)
            bot_text = rejected["probe_text"]
            summary["bot_request"] = {"exited": rejected["launcher_exit"],
                                      "probe_attached": "probe attach" in bot_text,
                                      "player_edition_logged": "FOF_PLAYER_EDITION" in bot_text,
                                      "cleanup_error": rejected["cleanup_error"],
                                      "exceptions": len(re.findall(r"EXCEPTION", bot_text + rejected["launcher_text"], re.I)),
                                      "game_captured": rejected["game_captured"]}
        else:
            summary["bot_request"] = {"skipped": "play cleanup did not complete safely"}
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        summary["error"] = str(exc)
    finally:
        active_games = []
        for game in state["games"]:
            if game.get("handle_closed"):
                continue
            try:
                if api.exit(game) is None:
                    active_games.append(game)
                elif not game.get("handle_closed"):
                    api.kernel.CloseHandle(game["handle"])
                    game["handle_closed"] = True
            except Exception as exc:
                active_games.append(game)
                summary["cleanup_error"] = str(exc)
        if active_games:
            try:
                sent = api.close_windows(active_games)
                if any(sent.get(game["pid"], 0) < 1 for game in active_games):
                    summary["cleanup_error"] = "WM_CLOSE was not posted to every verified active game"
                close_end = time.monotonic() + 30
                while active_games and time.monotonic() < close_end:
                    active_games = [game for game in active_games if api.exit(game) is None]
                    if active_games:
                        time.sleep(0.1)
            except Exception as exc:
                summary["cleanup_error"] = "Graceful game cleanup failed: " + str(exc)
        for game in state["games"]:
            if game not in active_games and not game.get("handle_closed"):
                api.kernel.CloseHandle(game["handle"])
                game["handle_closed"] = True
        active_launchers = [p for p in state["launchers"] if p.poll() is None]
        for process in active_launchers:
            try:
                process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                pass
        active_launchers = [p for p in state["launchers"] if p.poll() is None]
        restore_safe = not active_games and not active_launchers
        if restore_safe:
            shutil.copy2(backup, dll)
            lane.release(children_stopped=True)
        else:
            summary["cleanup_error"] = ("Owned game/launcher remains active or could not be verified; "
                                         "DLL left untouched and QA lease retained")
            summary["active_games"] = [g["pid"] for g in active_games]
            summary["active_launchers"] = [p.pid for p in active_launchers]
    play, bot = summary.get("play", {}), summary.get("bot_request", {})
    ok = (restore_safe and play.get("early_exit") is None and play.get("game_exit") == 0
          and play.get("launcher_exit") == 0 and play.get("player_edition") and play.get("campaign")
          and play.get("deferred_wait") and play.get("live_join_enabled") and play.get("exceptions") == 0
          and play.get("cleanup_error") is None and bot.get("exited") is not None
          and not bot.get("probe_attached") and not bot.get("game_captured")
          and bot.get("cleanup_error") is None and bot.get("exceptions") == 0)
    summary["result"] = "PASS" if ok else "FAIL"
    (result / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary))


if __name__ == "__main__":
    main()
