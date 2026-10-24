"""Build a NEW private research package from a fixed own-code allowlist.

This script never starts binaries, reads game folders, loads Steam, or publishes.
Run --selftest for synthetic packaging/path fixtures. Actual packaging is explicit.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import tempfile
from types import SimpleNamespace
from unittest import mock


ROOT = Path(__file__).resolve().parent.parent
BASELINE_SHA = "5c85b8c90c60b6d615a1aa50ae95d7b0acd32bbbe24bc68c282c136e5e7af42b"
STEAM_SHA = "670d654aa3255c5061cf0236a1cdbb2f5076cbd5ae44f613b6f4921558e83e2d"
# No recursive copy. Adding a shipped file requires editing this allowlist.
PAYLOAD = {
    "launcher/fof-private-launcher.exe": (0x8664, False),
    "probe/native-launcher.exe": (0x14C, False),
    "probe/native-probe.dll": (0x14C, True),
    "probe/d3d9.dll": (0x14C, True),
    "steam/native/native-relay.exe": (0x8664, False),
    "steam/native/steam_appid.txt": None,
    "compatibility/baseline.json": None,
}
# Player edition: the same package paths, built with no automated test or bots (launcher/build-player.cmd,
# probe/build-player.cmd). Each player binary must carry the edition marker; the other builds must not.
PLAYER_SOURCES = {
    "launcher/fof-private-launcher.exe": "launcher/player/fof-private-launcher.exe",
    "probe/native-probe.dll": "probe/player/native-probe.dll",
}
PLAYER_MARKERS = {
    "launcher/fof-private-launcher.exe": "FOF_PLAYER_EDITION".encode("utf-16-le"),
    "probe/native-probe.dll": b"FOF_PLAYER_EDITION",
}
START = ('@echo off\r\nsetlocal\r\nrem Opened straight from inside the .zip, Windows copies only this file to a temporary folder:\r\nrem say so plainly instead of Windows\' "cannot find fof-private-launcher.exe".\r\nif exist "%~dp0launcher\\fof-private-launcher.exe" goto run\r\nchcp 65001 >nul\r\necho.\r\necho   The package is still inside the .zip file.\r\necho   Right-click the .zip, choose "Extract All...", then open Start.cmd in the extracted folder.\r\necho.\r\necho   Файлы всё ещё внутри .zip-архива.\r\necho   Нажмите на .zip правой кнопкой мыши, выберите «Извлечь все...»,\r\necho   затем откройте Start.cmd в распакованной папке.\r\necho.\r\npause\r\nexit /b 1\r\n:run\r\nstart "" "%~dp0launcher\\fof-private-launcher.exe"\r\n').encode('utf-8')
README = """# Spider-Man: Friend or Foe — private research prototype

This package is unfinished and is not a release.

How to play: the host clicks the big red HOST button. The game opens and a lobby
code appears in the launcher; the host sends it to the friend and plays (New Game
or Continue; saves are kept in the package's profile folder). The friend pastes
the code ("Paste code") and clicks the big blue JOIN button, without opening the
game first: their game opens, starts by itself, takes over
the host's campaign progress and appears in the Helicarrier hub. From there the
host walks to the mission computer and both play the missions together, each
with their own keyboard or controller. In co-op only the host can use the mouse, and
only in menus (point and click); in missions the mouse is not used.

In the Helicarrier hub the friend also has a character: the campaign's chosen
sidekick (Prowler at the start) appears next to Spider-Man and walks with the
friend's controls; the camera keeps both characters in view. To change the
sidekick, the host walks to the sidekick computer and presses Select; the friend
then picks the character on that screen (only unlocked ones), and player 2 changes
right away. The host uses the mission computer as before.

Swapping characters: in a mission, either player holds the game's character-switch
control (TAB on the keyboard; the same control as in single player) for half a second, and
the two players trade characters: the friend takes Spider-Man and the host the
sidekick, or back again. The swap stays for the next missions until someone
swaps back. In the Helicarrier hub the host always walks Spider-Man.

Audio settings: the main menu's Options now has Audio Settings too (the same
screen as in the hub). Its first entry, saving the game, only works once a game
is loaded. The game runs at 60 frames per second, alone and in co-op.

First launch: music starts at 50%, speech and effects at 80%.

Controls and settings: keyboard players (no controller plugged in) start with the
game's Laptop controls layout. You can change it in Options; the game's own
settings (controls layout, volumes, brightness) are kept in the package's profile
folder and come back the next time you play.

If one player skips a movie or cutscene and the other watches it, the game that
is ahead pauses at the next menu or the hub until the other one catches up.
While it waits, a small box says "Waiting for your partner..." in the launcher's
language; it closes by itself when the game continues.

Watching the opening together happens by itself: if the friend clicks JOIN while
the host is still in the main menu (before starting a game), both games stay in
their own main menus (each player can change Options, Audio and so on). When the
host picks New Game, the host's game waits a moment until the friend's game is
back in its main menu, then you both watch the opening and play on together (each
PC uses its own first empty save slot). If the host picks Continue instead, the
friend joins in the Helicarrier as usual. If the host is already playing when the
friend joins, the friend joins in the Helicarrier.

In the Helicarrier during co-op, Escape does nothing (its pause menu only offers
dropping player 2 or quitting, which would break the session). To stop, click
"Stop session" in the launcher or close the game. Pause in missions works for both.

Tested only with bots on one PC: the host playing alone and a friend joining in
the hub, then a mission together; New Game, hub, the first Tokyo mission and the
return to the hub. Not established: two PCs over Steam, a host with real
campaign progress (only fresh progress was transferred in tests), later
missions, bosses, mission completion being recorded, real controllers and
reconnecting.

## Setup on each PC

1. Extract the complete package into a new writable local folder. Keep its
   launcher/, probe/, steam/native/, and compatibility/ subfolders together.
2. Open Start.cmd. Pick the window's language at the top ("Launcher language":
   English, Russian, Turkish, Ukrainian, German, French, Spanish, Portuguese,
   Italian, Polish or Chinese); it starts in your Windows language and is
   remembered. The game's own language (English or Russian files) is chosen
   separately under "Game language". Windows x64 and the existing .NET Framework are required;
   Python is not needed to run this package.
3. The launcher looks for your installed game on all drives when it starts and
   offers the copy it finds (only read, never changed). Otherwise click
   "Locate game install" and select the folder that contains Game.exe. The game
   can be on any drive: on the launcher's drive its files are linked (no second
   copy); on another drive the large art folder is linked as a folder and about
   30 MB is copied. Your installed game is never changed: the add-on refuses any
   write to it. Choose English or Russian independently. Use Verify copy thereafter.
4. The window finds the Steam component (steam_api64.dll) by itself in your
   installed Steam games and shows "Steam component found (from <game>)".
   It only reads files; nothing is copied or changed. It accepts copies signed
   by Valve that contain everything Spacewar networking needs (recent games
   such as Counter-Strike 2, Dota 2 or many Unity games have one). If none is
   found, install any recent free Steam game, or use "Choose manually...".
   This package does not contain that DLL, Steam, game executables, game
   assets, or personal saves, and does not download them.
5. Display: choose "Borderless window (Alt+Tab friendly)" or "Window" and a
   resolution; your monitor's own size is preselected and the choice is
   remembered. Each player picks their own; widescreen and ultrawide shapes
   are shown correctly (more view to the sides, nothing stretched). In
   borderless mode Alt+Tab never pauses the game, so your partner keeps playing.
   "Game's own settings (original fullscreen)" uses the resolution saved in
   the game's own options on this PC and its original fullscreen; Alt+Tab then
   behaves like the original game (it may flicker or pause your partner).
   HOST opens the game and the lobby at once (Steam must be open); the host's
   progress is kept in the profile folder.
   The launcher also remembers the game folder you imported from.
6. For an explicitly authorized account session, run Steam signed in, then use
   Host or Join. Host shows a numeric lobby code to exchange manually. Join
   enters that code. Clicking these controls starts Spacewar transport on that
   account. Opening the UI, verification, and import do not use the account.
   No invitations are sent. The game starts after a validated peer is ready.

## Automated connection check

Ordinary play is the default. To run an automated connection check, enable
Automated connection test on BOTH PCs before clicking Host or Join. Both sides
must use the same mode. The test supplies its own scripted controls: no manual
gameplay is needed. It requests 3600 frames and ends automatically, with a
five-minute game limit. Startup and slow connections may lengthen the run.

After the owned processes have exited and cleanup finishes, the UI reports
PASS only for a complete consecutive frame log, native peer FIN completion,
normal game/launcher/relay exit, and a nonempty relay stream with zero discarded
bytes. Cancellation, timeout, missing evidence, or a desync fails the check.
A private connection-test.json and raw logs stay in that session directory.
No additional Steam action occurs until Host or Join is clicked.

Own-code fixture checks exercise these branches without Steam or the game.
A local fixture pass does not prove remote delivery. This check must actually
complete between two PCs before claiming remote transport works. Even a real
connection-test pass covers this finite scripted Tokyo scenario; real physical
controls, the full campaign, persistent saves and reconnect remain unverified.

The only supported executable is Windows version 1.0.0.111 with SHA-256
ebd30047d7e58c95bf7dc4bd1fb4a5780e16aa472b61c25a8c421632662083db.
Complete known English or Russian runtime packs must pass verification. Mixed
language files and modified runtime/configuration files are rejected. Players
are intended to choose their own keyboard/controller; real hardware behavior
has not yet been verified. Do not use this package as a completed playtest plan.

## Sessions and stopping

Every launch creates a new sessions/ folder entry with private logs and an
isolated sandbox. Saves are not imported from the original game and no resume
option exists. Stop session closes this launcher's game and transport. Session
logs may contain account identifiers: keep them private. The probe redirects
known game file and registry operations; it is not a complete OS sandbox.

steam/native/steam_appid.txt must stay beside native-relay.exe and contain 480.
The intended Steam relay avoids manual port forwarding, but actual two-PC/WAN
delivery has not yet been tested. A successful create/leave lobby smoke check
does not prove game delivery.

SHA256SUMS.json lists every other shipped file, with its byte size and SHA-256.
It detects accidental changes and is not a signature or an independent trust
source. This package includes only this project's own runtime components and
file-hash metadata. It contains no SDK implementation, game data, test fixtures,
saved games, raw account output, or original install files.
""".replace("STEAM_SHA_VALUE", STEAM_SHA)


class Rejected(ValueError):
    pass


def safe_path(path: Path, must_exist: bool = True) -> Path:
    """Reject symlinks/junctions at every existing path component, before resolve."""
    path = Path(os.path.abspath(path))
    for part in (path, *path.parents):
        try:
            info = part.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise Rejected("Reparse or symbolic-link path rejected")
    if must_exist and not path.exists():
        raise Rejected("Required package input is missing: " + path.name)
    return path


def validate_name(name: str) -> str:
    reserved = {"CON", "PRN", "AUX", "NUL"} | {
        prefix + str(i) for prefix in ("COM", "LPT") for i in range(1, 10)
    }
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}", name) or name.upper() in reserved:
        raise Rejected("Package name must be a plain nonreserved folder name (1–64 characters)")
    return name


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def validate_pe(data: bytes, expected: tuple[int, bool]) -> None:
    if len(data) < 64 or data[:2] != b"MZ":
        raise Rejected("Runtime input is not a PE executable")
    offset = struct.unpack_from("<I", data, 0x3C)[0]
    if offset > len(data) - 24 or data[offset:offset + 4] != b"PE\0\0":
        raise Rejected("Invalid PE header")
    machine = struct.unpack_from("<H", data, offset + 4)[0]
    flags = struct.unpack_from("<H", data, offset + 22)[0]
    if (machine, bool(flags & 0x2000)) != expected:
        raise Rejected("Wrong executable architecture or DLL identity")


def player_readme() -> str:
    head, rest = README.split("## Automated connection check", 1)
    tail = rest[rest.index("The only supported executable"):]
    text = head + tail
    text = text.replace("# Spider-Man: Friend or Foe — private research prototype",
                        "# Spider-Man: Friend or Foe — online co-op (player edition)")
    return text.replace("Tested only with bots on one PC", "Tested only with automated tests on one PC") + (
        "\nPlayer edition: this package has no automated connection test, test bots or\n"
        "scripted controls; the add-on refuses to start if one is requested.\n")


def collect(root: Path, baseline_sha: str = BASELINE_SHA, player: bool = False) -> dict[str, bytes]:
    root = safe_path(root)
    result = {}
    for relative, pe in PAYLOAD.items():
        source = safe_path(root / (PLAYER_SOURCES.get(relative, relative) if player else relative))
        if not source.is_file():
            raise Rejected("Package input is not a regular file")
        data = source.read_bytes()
        # Also catches an accidentally embedded build/PDB path in runtime files.
        flattened = data.replace(b"\0", b"")
        if re.search(rb"[A-Za-z]:[\\/]Users[\\/]", flattened, re.IGNORECASE):
            raise Rejected("Embedded private user path in package input: " + relative)
        if relative in PLAYER_MARKERS and (PLAYER_MARKERS[relative] in data) != player:
            raise Rejected("Wrong edition binary for this package: " + relative)
        if pe is not None:
            validate_pe(data, pe)
        elif relative.endswith("steam_appid.txt"):
            if data.strip() != b"480":
                raise Rejected("Steam application metadata must contain 480")
        elif digest(data) != baseline_sha:
            raise Rejected("Compatibility baseline pin mismatch")
        result[relative] = data
    result["README.md"] = (player_readme() if player else README).encode("utf-8")
    result["Start.cmd"] = START
    return result


def build(root: Path, name: str, baseline_sha: str = BASELINE_SHA, player: bool = False) -> dict:
    validate_name(name)
    root = safe_path(root)
    parent = safe_path(root / "dist", must_exist=False)
    destination = safe_path(parent / name, must_exist=False)
    if destination.exists():
        raise Rejected("Package destination already exists; nothing was replaced")
    # Finish all input and privacy checks before creating the output directory.
    payload = collect(root, baseline_sha, player)
    parent.mkdir(exist_ok=True)
    safe_path(parent)
    destination.mkdir()  # Atomic refusal to reuse a concurrent/existing output.
    manifest = {
        "schema": "fof-private-package-v1",
        "status": "unfinished-private-research",
        "edition": "player" if player else "research",
        "files": {p: {"size": len(data), "sha256": digest(data)}
                  for p, data in sorted(payload.items())},
    }
    payload["SHA256SUMS.json"] = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode()
    for relative, data in sorted(payload.items()):
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        safe_path(target.parent)
        with target.open("xb") as stream:
            stream.write(data)
    actual = {p.relative_to(destination).as_posix() for p in destination.rglob("*") if p.is_file()}
    if actual != set(payload):
        raise Rejected("Unexpected package output; inspect the new directory")
    for relative, data in payload.items():
        if (destination / relative).read_bytes() != data:
            raise Rejected("Package readback mismatch")
    return {"status": "private-research-package-created", "files": len(payload),
            "bytes": sum(map(len, payload.values())), "package": name}


def selftest() -> None:
    count = 0

    def reject(call):
        nonlocal count
        try:
            call()
        except Rejected:
            count += 1
        else:
            raise AssertionError("Expected rejection")

    for name in ("..", "a/b", "a\\b", "CON", "lpt1", "a.", "x" * 65):
        reject(lambda name=name: validate_name(name))
    with tempfile.TemporaryDirectory(prefix="fof-package-own-fixture-", dir=ROOT / "launcher") as raw:
        root = Path(raw)
        assert root.resolve().is_relative_to((ROOT / "launcher").resolve())
        baseline = b'{"fixture":"own synthetic metadata only"}\n'
        for relative, pe in PAYLOAD.items():
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            if pe:
                data = bytearray(128)
                data[:2] = b"MZ"
                struct.pack_into("<I", data, 0x3C, 64)
                data[64:68] = b"PE\0\0"
                struct.pack_into("<H", data, 68, pe[0])
                struct.pack_into("<H", data, 86, 0x2000 if pe[1] else 0)
                path.write_bytes(data)
            else:
                path.write_bytes(b"480\r\n" if relative.endswith(".txt") else baseline)
        (root / "never-copy-game-or-account-log.txt").write_text("private fixture")
        result = build(root, "fixture-package", digest(baseline))
        assert result["files"] == len(PAYLOAD) + 3
        output = root / "dist" / "fixture-package"
        manifest = json.loads((output / "SHA256SUMS.json").read_text())
        assert set(manifest["files"]) == set(PAYLOAD) | {"README.md", "Start.cmd"}
        for relative, entry in manifest["files"].items():
            data = (output / relative).read_bytes()
            assert len(data) == entry["size"] and digest(data) == entry["sha256"]
        count += 1
        reject(lambda: build(root, "fixture-package", digest(baseline)))
        reject(lambda: collect(root, "0" * 64))
        app = root / "steam/native/steam_appid.txt"
        app.write_bytes(b"481")
        reject(lambda: collect(root, digest(baseline)))
        app.write_bytes(b"480")
        binary = root / "probe/native-launcher.exe"
        original = binary.read_bytes()
        binary.write_bytes(original + b"C:\\Users\\private-name\\build.pdb")
        reject(lambda: collect(root, digest(baseline)))
        binary.write_bytes(b"not a PE")
        reject(lambda: collect(root, digest(baseline)))
        binary.write_bytes(original)
        reject(lambda: validate_pe(original, (0x8664, False)))
        reject(lambda: validate_pe(original, (0x14C, True)))
        real_lstat = Path.lstat
        def fake_reparse(path, *args, **kwargs):
            if path == root.parent:
                return SimpleNamespace(st_mode=stat.S_IFDIR, st_file_attributes=0x400)
            return real_lstat(path, *args, **kwargs)
        with mock.patch.object(Path, "lstat", fake_reparse):
            reject(lambda: safe_path(binary))
        (root / "probe/native-probe.dll").unlink()
        reject(lambda: collect(root, digest(baseline)))
    print(json.dumps({"status": "own-package-selftest-pass", "checks": count,
                      "actual_runtime_packaged": False, "game_or_account_used": False}))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--selftest", action="store_true")
    mode.add_argument("--build", metavar="NEW_PACKAGE_NAME")
    parser.add_argument("--player", action="store_true", help="player edition: no automated test or bots")
    args = parser.parse_args()
    try:
        if args.selftest:
            selftest()
        else:
            print(json.dumps(build(ROOT, args.build, player=args.player), sort_keys=True))
    except (Rejected, OSError) as error:
        parser.exit(1, "Package rejected: " + str(error) + "\n")


if __name__ == "__main__":
    main()
