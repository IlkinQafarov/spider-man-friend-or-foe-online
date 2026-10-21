"""Read-only content preflight for the two supplied Friend or Foe language packs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import sys

HERE = Path(__file__).absolute().parent
LOCAL = HERE.parent
TEST = LOCAL / "test"
CRITICAL = "c46b90afe36571cf3f4fbe3d4e7918da833297f78119d40504e96b4f531b36ea"
LOCALES = {
    "English": "4d7d3496ff2ecd7d4f112a7a0576b7fa82b4b4233e355e1ecae82f63a3c7fea8",
    "Russian": "04053ecb5b7b995dc9c3368970d17197b7495d16d10ccac1165f070e6fc221cc",
}
IGNORE = frozenset((
    "docs/cs/customer_support.htm", "docs/license/license.txt", "docs/manual/manual.pdf",
    "docs/msr/msr.txt", "docs/readme/readme.doc", "gameicon.ico", "icon_mediacenter.png",
    "mc.exe", "mediacenter.mcl", "sm.ico", "spider-man friend or foe.exe", "unins000.dat",
    "unins000.exe", "uninstall.ico",
))


class Rejected(ValueError):
    pass


def normalized(path):
    path = str(path).replace("\\", "/")
    if not path or path.startswith("/") or any(x in path for x in ("\0", "\n", "\r", ":")):
        raise Rejected("Invalid relative path")
    if any(part in ("", ".", "..") for part in path.split("/")):
        raise Rejected("Invalid relative path")
    return path.lower()


def index(entries):
    result = {}
    for path, value in entries:
        key = normalized(path)
        if key in result:
            raise Rejected("Duplicate case-insensitive path: " + key)
        result[key] = value
    return result


def digest(records, kind):
    h = hashlib.sha256()
    h.update(f"coop-fof-{kind}-v1\0{len(records)}\n".encode("utf-8"))
    for path in sorted(records):
        size, sha = records[path]
        h.update(f"{path}\0{size}\0{sha}\n".encode("utf-8"))
    return h.hexdigest()


def runtime(path):
    return path.startswith(("art/", "ini/", "beenox/", "materials/")) or path in (
        "game.exe", "binkw32.dll")


def no_reparse(path):
    info = path.lstat()
    if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
        raise Rejected("Reparse/symlink path rejected: " + path.name)
    return info


def confined(path, parent, require_exists=True):
    path, parent = Path(os.path.abspath(path)), Path(os.path.abspath(parent))
    try:
        relative = path.relative_to(parent)
    except ValueError:
        raise Rejected("Path must stay inside " + parent.name) from None
    if not relative.parts:
        raise Rejected("Select a child path inside " + parent.name)
    # Check lexical ancestors before any resolve() can conceal a junction.
    for ancestor in reversed((path.parent, *path.parent.parents)):
        no_reparse(ancestor)
    if require_exists:
        no_reparse(path)
    elif path.exists() or path.is_symlink():
        raise Rejected("Output must be a NEW file")
    return path


def scan(root):
    found = []
    directories = [root]
    while directories:
        directory = directories.pop()
        no_reparse(directory)
        with os.scandir(directory) as entries:
            for entry in entries:
                path = Path(entry.path)
                info = no_reparse(path)
                if stat.S_ISDIR(info.st_mode):
                    directories.append(path)
                elif stat.S_ISREG(info.st_mode):
                    found.append((path.relative_to(root).as_posix(), path))
                else:
                    raise Rejected("Non-regular file rejected: " + path.name)
    return index(found)


def hash_file(path):
    before = no_reparse(path)
    h = hashlib.sha256()
    with path.open("rb") as stream:
        opened = os.fstat(stream.fileno())
        if (before.st_dev, before.st_ino) != (opened.st_dev, opened.st_ino):
            raise Rejected("File changed before hashing: " + path.name)
        for chunk in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            h.update(chunk)
        after = os.fstat(stream.fileno())
    current = no_reparse(path)
    signature = lambda s: (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns)
    if signature(before) != signature(after) or signature(after) != signature(current):
        raise Rejected("File changed while hashing: " + path.name)
    return [before.st_size, h.hexdigest()]


def verify_records(baseline, found, hasher=hash_file):
    core, packs = baseline["critical"], baseline["locales"]
    expected = set(core) | set(next(iter(packs.values()))["files"])
    missing = expected - found.keys()
    if missing:
        raise Rejected("Missing runtime file: " + sorted(missing)[0])
    for path in sorted(found):
        if path.endswith(".dll") and path != "binkw32.dll":
            raise Rejected("Unknown DLL: " + path)
        if path not in expected and path not in IGNORE and not path.startswith("gamelog/"):
            raise Rejected("Unexpected runtime/package file: " + path)
    # Small configuration files fail before reading the large asset set.
    order = sorted(expected, key=lambda p: (not p.startswith("ini/"), p))
    actual = {}
    candidates = set(packs)
    for path in order:
        actual[path] = hasher(found[path])
        if path in core:
            if actual[path] != core[path]:
                raise Rejected("Critical content mismatch: " + path)
        else:
            candidates = {name for name in candidates if actual[path] == packs[name]["files"][path]}
            if not candidates:
                raise Rejected("Unknown or mixed language pack: " + path)
    if len(candidates) != 1:
        raise Rejected("Language pack is ambiguous")
    language = candidates.pop()
    actual_core = {p: actual[p] for p in core}
    actual_locale = {p: actual[p] for p in packs[language]["files"]}
    return {
        "schema": "coop-fof-preflight-v1", "status": "pass", "language": language,
        "critical_digest": digest(actual_core, "critical"),
        "locale_digest": digest(actual_locale, "locale"),
        "critical_files": len(core), "locale_files": len(actual_locale),
    }


def check_baseline(baseline):
    if baseline.get("schema") != "coop-fof-baseline-v1":
        raise Rejected("Unsupported baseline schema")
    core = baseline["critical"]
    packs = baseline["locales"]
    if len(core) != 1257 or digest(core, "critical") != CRITICAL or set(packs) != set(LOCALES):
        raise Rejected("Baseline critical manifest is not pinned")
    locale_paths = None
    for language, pinned in LOCALES.items():
        records = packs[language]["files"]
        if len(records) != 34 or digest(records, "locale") != pinned:
            raise Rejected("Baseline language manifest is not pinned: " + language)
        if locale_paths is not None and set(records) != locale_paths:
            raise Rejected("Language path sets differ")
        locale_paths = set(records)
    for path in set(core) | locale_paths:
        if normalized(path) != path or not runtime(path):
            raise Rejected("Invalid baseline runtime path")
    if set(core) & locale_paths:
        raise Rejected("Baseline categories overlap")
    return baseline


def build_baseline():
    inventories = {}
    for language in LOCALES:
        source = LOCAL / "analysis" / "inventory" / (language.lower() + ".json")
        source_records = json.loads(source.read_text(encoding="utf-8"))
        inventories[language] = index((p, [v["size"], v["sha256"]]) for p, v in source_records.items())
    en, ru = inventories.values()
    if en.keys() != ru.keys():
        raise Rejected("Intake path sets differ")
    core, locale = {}, {language: {} for language in LOCALES}
    for path in sorted(en):
        if runtime(path):
            if en[path] == ru[path]:
                core[path] = en[path]
            else:
                for language in LOCALES:
                    locale[language][path] = inventories[language][path]
    baseline = {"schema": "coop-fof-baseline-v1", "critical": core,
                "locales": {name: {"files": records} for name, records in locale.items()}}
    return check_baseline(baseline)


def write_new(output, value):
    output = confined(output, HERE, require_exists=False)
    with output.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, separators=(",", ":"), sort_keys=True)
        stream.write("\n")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    build = sub.add_parser("build-baseline", help="Derive hash-only baseline from existing intake inventories")
    build.add_argument("--output", type=Path, default=HERE / "baseline.json")
    verify = sub.add_parser("verify", help="Hash an isolated copy; write a NEW result only after passing")
    verify.add_argument("--game-dir", type=Path, required=True)
    verify.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        confined(args.output, HERE, require_exists=False)
        if args.command == "build-baseline":
            result = build_baseline()
        else:
            game = confined(args.game_dir, TEST)
            if not game.is_dir():
                raise Rejected("Game directory is not a directory")
            baseline_path = HERE / "baseline.json"
            no_reparse(baseline_path)
            baseline = check_baseline(json.loads(baseline_path.read_text(encoding="utf-8")))
            result = verify_records(baseline, scan(game))
        write_new(args.output, result)
        if args.command == "verify":
            print(f"PASS language={result['language']} critical={result['critical_digest']}")
        else:
            print("BASELINE READY critical=" + CRITICAL)
        return 0
    except (Rejected, OSError, KeyError, TypeError, json.JSONDecodeError) as exc:
        print("REJECTED: " + str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
