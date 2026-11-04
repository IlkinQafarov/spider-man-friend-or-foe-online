"""Prepare a NEW, audited source snapshot; never uploads or copies Git history.

Only tracked/explicitly unignored source files enter the candidate list. A second
independent allowlist, regular-file checks and privacy checks then fail closed.
Review the resulting inventory before uploading. This is not a secret scanner
or a license audit for arbitrary third-party contributions.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import stat
import subprocess

ROOT = Path(__file__).absolute().parents[1]
BASELINE = "5c85b8c90c60b6d615a1aa50ae95d7b0acd32bbbe24bc68c282c136e5e7af42b"
GENERATED_ART = {'launcher/assets/backgrounds/header.png': 'a5cc36eb6260981ca9d81751ae1b4ac0152594118e45c2ac55bc81d28fc86bd3', 'launcher/assets/backgrounds/host.png': '0c66e3ce8419b1d1a00c9592605d48734f6a59f1bec544d736ae8c1e011e6f0b', 'launcher/assets/backgrounds/join.png': '8ad67becaf639116352cfd45f3775c78a78ef82c0ff0f578f86c613b613dee67'}
GENERATED_ART["launcher/assets/foreground/title.png"] = "7e60cce03410bbc8d302d8c8e8fc895feb2bfe5665f18111bf839d7d5af9aa12"
GENERATED_ART.update({'launcher/assets/foreground/host-icon.png': '008e22aad351e387f9a6d749dd7f9de559bba2867615fde4577f5e351e9ee3e5', 'launcher/assets/foreground/join-icon.png': '553931a85b918a069cd79395068c9ced4af2e91e87c00c0b81412301e6cc41c1'})
GENERATED_ART.update({'launcher/assets/rooftop/distant-city.png': 'f1dd15fb776377b2a80fad80dd31571c340d26592284a3461580115d491a96c8', 'launcher/assets/rooftop/swing-sign.png': '25efde7aa9a1e043d5a6f0f00e31bbec4855fead64e28bedc80f66a61e28c063', 'launcher/assets/rooftop/front.png': '3875e7467f12b4a9401c9c5e58efd259ca2f708775e3ea24882667746f5c4ba2', 'launcher/assets/rooftop/live-plates.png': 'c6ec4e71e016ff45d32cf632ff8b3fdef5b7e5a7a85a1826c844a19573c714ba', 'launcher/assets/rooftop/hover-ink.png': '5303ea42c54618aa44a9275e8bc97132702f0ee75e54bf25bfceb6f35c4d6651', 'launcher/assets/rooftop/hover-signs.png': 'bd2cf342e4fa6a012a10939bdcd89301eabdd093b05857baab4f608dc955bbad', 'launcher/assets/comic-paper/lobby-frame.png': '554fddf47ce96a3bfcdebbe8ae353488adf3e91347cded37353607ee49641bb1', 'launcher/assets/comic-paper/dialog-frame.png': '893077adec70c71e3485d26ab5c352ee96145c4e97b278afebd7db74c3084872'})
ORIGINAL_UI_BINARY = {'launcher/assets/sounds/arrival.wav': '94965867050410795afccf2e8cbf07e1c9220258321b389f4a40d57144e70f89', 'launcher/assets/sounds/click.wav': '4702e4639701f94a583911927bc99618fd46845f89d88f28fafc128a3832c388', 'launcher/assets/sounds/close.wav': 'a15a1be020b97e039cb58702626fa537054a4e4dc690e3664df29178696b2093', 'launcher/assets/icons/launcher.ico': 'c30b6c2c093ea1ffad61296acbcb1ba53236bf7c4ba511997550a646454cf630'}
THIRD_PARTY_ICONS = {'launcher/assets/icons/accessibility.svg': '114a9b6983ee20c43d76556bbb5633085a0e082b19464dfa36f1e27768c8f3ea', 'launcher/assets/icons/globe.svg': '82c6b8fa0b8e8d775ec9becd86f74a4e50bff7bee4c81f7362de45348504f14d', 'launcher/assets/icons/LICENSE-lucide.txt': 'b495047bd93a9b06913511076f504daba17d5bbeb3e0650f3bb53a4220329c57', 'launcher/assets/icons/person-standing.svg': '02960f63be14df8543ec5dabf5250a47509c35f177eb90ef9c864aa7acc1161a', 'launcher/assets/icons/settings.svg': '0ae27fd0f81999229e3127ac96c5b32edfea448e291d509e76212b917551d66b', 'launcher/assets/icons/SOURCES.json': '8ca4dacc3cb758081b00ad20f1bf566b611edba73bfb1ee34b3b3e37266e4e2e'}
TOP = {".gitignore", ".gitattributes", "README.md", "PROTOTYPE.md", "LICENSE"}
DIRECTORIES = {
    "probe": {".cpp", ".hpp", ".inc", ".cmd", ".md"},
    "launcher": {".cs", ".py", ".cmd", ".md"},
    "compatibility": {".py", ".md"},
    "qa": {".py"},
    "steam": {".py", ".cpp", ".cmd", ".md"},
    "steam/native": {".cpp", ".hpp", ".cmd", ".md"},
    "proxy": {".cpp", ".cmd", ".md", ".py"},
    "docs": {".md", ".svg"},
}
SPECIAL = {"compatibility/baseline.json", "steam/native/steam_appid.txt",
           "analysis/native/score_pair.py", "qa/README.md", "launcher/LauncherWindow.xaml"}
RULES = {
    "user profile path": re.compile(r"(?i)\b[A-Z]:[\\/]+Users[\\/]+[^\\/\s\"'<>%]+"),
    "credential": re.compile(r"\b(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,}|sk-[A-Za-z0-9_-]{30,})\b"),
    "private key": re.compile(r"-----BEGIN(?: [A-Z]+)? PRIVATE KEY-----"),
    "Steam account identifier": re.compile(r"\b7656119[0-9]{10}\b"),
    "email address": re.compile(r"\b[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}\b"),
}


class Rejected(RuntimeError):
    pass


def allowed(name):
    path = Path(name)
    if "\\" in name or path.is_absolute() or any(p in ("", ".", "..") for p in name.split("/")):
        return False
    return name in ORIGINAL_UI_BINARY or name in THIRD_PARTY_ICONS or name in GENERATED_ART or name in TOP or name in SPECIAL or (
        path.parent.as_posix() in DIRECTORIES and path.suffix in DIRECTORIES[path.parent.as_posix()])


def plain(path):
    for part in reversed((path, *path.parents)):
        info = part.lstat()
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise Rejected("Reparse path rejected")
    info = path.stat()
    if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or not 0 < info.st_size <= (5242880 if path.relative_to(ROOT).as_posix() in GENERATED_ART else 524288):
        raise Rejected("Nonregular, linked, empty or oversized source file")


def inspect_text(name, raw):
    if name in ORIGINAL_UI_BINARY:
        if hashlib.sha256(raw).hexdigest() != ORIGINAL_UI_BINARY[name]:
            raise Rejected("Original UI asset pin mismatch: " + name)
        return
    if name in THIRD_PARTY_ICONS:
        if hashlib.sha256(raw).hexdigest()!=THIRD_PARTY_ICONS[name]:
            raise Rejected("Pinned Lucide source changed")
        return
    if name in GENERATED_ART:
        if not raw.startswith(b"\x89PNG\r\n\x1a\n") or hashlib.sha256(raw).hexdigest() != GENERATED_ART[name]:
            raise Rejected("Generated artwork pin mismatch: " + name)
        return
    if b"\0" in raw:
        raise Rejected(name + ": binary content")
    text = raw.decode("utf-8")
    # This one literal is an existing rejection fixture, not an owner path.
    if name == "launcher/build_package.py":
        fixture = "C:" + chr(92) * 2 + "Users" + chr(92) * 2 + "private-name" + chr(92) * 2 + "build.pdb"
        text = text.replace(fixture, "<synthetic rejection fixture>")
    for label, pattern in RULES.items():
        if pattern.search(text):
            raise Rejected(name + ": " + label)
    if name == "compatibility/baseline.json" and hashlib.sha256(raw).hexdigest() != BASELINE:
        raise Rejected("Pinned compatibility manifest changed")
    if name == "steam/native/steam_appid.txt" and raw.strip() != b"480":
        raise Rejected("Unexpected Steam application ID")
    if name.endswith(".svg") and re.search(r"(?i)<(?:script|image|foreignObject)\b|(?:href|src)\s*=", text):
        raise Rejected("SVG must contain only original static drawing/text")


def snapshot(name):
    if not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", name) or len(name) > 72:
        raise Rejected("Use a simple new snapshot name")
    # Committed files only: uncommitted work in progress (another agent's, or unfinished) never goes public.
    names = subprocess.check_output(["git", "ls-files", "--cached", "-z"], cwd=ROOT).decode("utf-8").split("\0")
    names = sorted(set(n for n in names if n))
    candidates, seen = [], set()
    for source in names:
        if not allowed(source) or source.casefold() in seen:
            raise Rejected("Source membership/case collision rejected: " + source)
        seen.add(source.casefold())
        path = ROOT / source
        plain(path)
        raw = path.read_bytes()
        inspect_text(source, raw)
        candidates.append((source, raw))
    if not {n.casefold() for n in ("README.md", "LICENSE", "probe/probe.cpp", "qa/run_pair.py")}.issubset(seen):
        raise Rejected("Snapshot is missing required public source/documentation")
    parent = ROOT / "publication"
    if parent.exists():
        if not parent.is_dir() or parent.is_symlink() or getattr(parent.stat(), "st_file_attributes", 0) & 0x400:
            raise Rejected("Snapshot parent rejected")
    else:
        parent.mkdir()
    dest = parent / name
    dest.mkdir()  # Must be NEW. No overwrite or deletion path exists.
    inventory = []
    for source, raw in candidates:
        target = dest / "source" / source
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open("xb") as stream:
            stream.write(raw)
        if target.read_bytes() != raw:
            raise Rejected("Snapshot readback differs")
        inventory.append({"path": source, "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()})
    report = {"schema": "fof-source-publication-v1", "created_utc": datetime.now(timezone.utc).isoformat(),
        "files": inventory, "count": len(inventory), "bytes": sum(f["bytes"] for f in inventory),
        "history_included": False, "binaries_included": False, "game_assets_included": False,
        "original_ui_binary_assets": sorted(n for n in names if n in ORIGINAL_UI_BINARY),
              "generated_launcher_art_included": sorted(n for n in names if n in GENERATED_ART)}
    with (dest / "inventory.json").open("x", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
    print(json.dumps({"snapshot": name, "count": report["count"], "bytes": report["bytes"],
                      "history_included": False, "audit": "PASS"}))
    return report


def selftest():
    for name in ("Game.exe", "test/Game.exe", "probe/probe.dll", "sessions/log.py", "../probe/a.cpp",
                 "/probe/a.cpp", "probe/../a.cpp", "qa/sub/a.py", "docs/game.png"):
        assert not allowed(name), name
    for name in ("probe/probe.cpp", "docs/STATUS.md", "qa/prepare_publication.py", "compatibility/baseline.json"):
        assert allowed(name), name
    private_path = "C:" + chr(92) + "Users" + chr(92) + "ExampleUser" + chr(92) + "save.dat"
    bad = [private_path, "gh" + "p_" + "a" * 36,
           "7656119" + "1" * 10, "person" + "@" + "example.test", "binary\0text"]
    for value in bad:
        try:
            inspect_text("qa/fixture.py", value.encode())
        except Rejected:
            pass
        else:
            raise AssertionError("Privacy/binary mutation was accepted")
    inspect_text("qa/fixture.py", b"connect('127.0.0.1'); version='1.0.0.111'")
    print("PUBLICATION_SELFTEST_PASS membership=13 sensitive_or_binary_mutations=5")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--selftest", action="store_true")
    group.add_argument("--snapshot")
    args = parser.parse_args()
    selftest() if args.selftest else snapshot(args.snapshot)
