"""Build one self-extracting Windows player executable from a built package."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
HERE = Path(__file__).resolve().parent
CSC = Path(r"C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe")
CORE = (
    "launcher/fof-private-launcher.exe", "probe/native-launcher.exe",
    "probe/native-probe.dll", "probe/d3d9.dll", "steam/native/native-relay.exe",
    "probe/version.dll", "probe/safedisc-source.zip", "probe/SAFE-DISC-LICENSE.txt",
    "steam/native/steam_appid.txt", "compatibility/baseline.json",
)
ROOFTOP = tuple("launcher/assets/rooftop/{}.png".format(x) for x in ("distant-city", "swing-sign", "front", "live-plates"))
MAGIC = b"FOFBNDL1"


class Rejected(Exception):
    pass


def digest(data):
    return hashlib.sha256(data).hexdigest()


def put_string(out, value):
    raw = value.encode("utf-8")
    n = len(raw)
    while n >= 0x80:
        out.write(bytes(((n & 0x7f) | 0x80,)))
        n >>= 7
    out.write(bytes((n,)))
    out.write(raw)


def safe_file(root, rel):
    path = root.joinpath(*rel.split("/"))
    cur = path
    while True:
        if cur.exists() or cur.is_symlink():
            info = cur.lstat()
            # On Windows, junctions/mount points are reparse points even on Python 3.11,
            # where pathlib.Path.is_junction() is not available.
            if cur.is_symlink() or (getattr(info, "st_file_attributes", 0) & 0x400):
                raise Rejected("Reparse or symbolic link in package: " + rel)
        if cur.parent == cur:
            break
        cur = cur.parent
    if not path.is_file():
        raise Rejected("Missing regular package file: " + rel)
    return path


def collect(package):
    root = Path(os.path.abspath(package))
    if not root.is_dir():
        raise Rejected("Package directory does not exist")
    manifest_path = safe_file(root, "SHA256SUMS.json")
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except Exception as exc:
        raise Rejected("Invalid package manifest") from exc
    if (manifest.get("schema") != "fof-private-package-v1" or manifest.get("edition") != "player"
            or manifest.get("status") != "beta-player-build" or not isinstance(manifest.get("files"), dict)):
        raise Rejected("Input must be a player package built by launcher/build_package.py --player")
    files = manifest["files"]
    names = set(files)
    allowed = set(CORE) | set(ROOFTOP) | {"README.md", "Start.cmd"}
    if not (set(CORE) | set(ROOFTOP)).issubset(names) or not names.issubset(allowed):
        raise Rejected("Package manifest file set is outside the fixed own-code allowlist")
    actual = set()
    for current, dirs, filenames in os.walk(str(root), topdown=True, followlinks=False):
        current_path = Path(current)
        for name in list(dirs) + filenames:
            entry_path = current_path / name
            info = entry_path.lstat()
            if entry_path.is_symlink() or (getattr(info, "st_file_attributes", 0) & 0x400):
                raise Rejected("Reparse or symbolic link in package tree")
            if entry_path.is_file():
                actual.add(entry_path.relative_to(root).as_posix())
        dirs[:] = [d for d in dirs if (current_path / d).is_dir()]
    if actual != names | {"SHA256SUMS.json"}:
        raise Rejected("Package has files missing from or outside the manifest (extra: {}; missing: {})".format(
            sorted(actual - names - {"SHA256SUMS.json"}), sorted((names | {"SHA256SUMS.json"}) - actual)))
    result = {}
    for rel in sorted(names):
        entry = files[rel]
        if not isinstance(entry, dict) or set(entry) != {"size", "sha256"}:
            raise Rejected("Malformed manifest entry: " + rel)
        data = safe_file(root, rel).read_bytes()
        if type(entry["size"]) is not int or entry["size"] != len(data) or entry["sha256"] != digest(data):
            raise Rejected("Manifest hash or size mismatch: " + rel)
        if rel in CORE or rel in ROOFTOP:
            result[rel] = data
    if result["steam/native/steam_appid.txt"].strip() != b"480":
        raise Rejected("Unexpected Steam app metadata")
    return result


def archive(files, path):
    with path.open("wb") as out:
        out.write(MAGIC)
        out.write(struct.pack("<i", len(files)))
        for rel, data in sorted(files.items()):
            put_string(out, rel)
            out.write(struct.pack("<q", len(data)))
            put_string(out, digest(data))
            out.write(data)


def build(package, output):
    if not CSC.is_file():
        raise Rejected("Existing .NET Framework 4 compiler not found: " + str(CSC))
    files = collect(package)
    output = Path(os.path.abspath(output))
    if output.exists():
        raise Rejected("Output already exists; no file was replaced")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="fof-bundle-", dir=str(output.parent)) as temp:
        archive_path = Path(temp) / "payload.bin"
        compiled = Path(temp) / "FriendOrFoe.exe"
        archive(files, archive_path)
        cmd = [str(CSC), "/nologo", "/codepage:65001", "/warn:4", "/warnaserror", "/target:winexe",
               "/win32icon:" + str(HERE / "assets" / "icons" / "launcher.ico"),
               "/platform:x64", "/optimize+", "/out:" + str(compiled), "/resource:" + str(archive_path) + ",fof.bundle",
               "/reference:System.dll", "/reference:System.Windows.Forms.dll", str(HERE / "Bundle.cs")]
        proc = subprocess.run(cmd, cwd=str(ROOT), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if proc.returncode:
            raise Rejected("C# build failed:\n" + proc.stdout)
        try:
            os.rename(str(compiled), str(output))
        except FileExistsError:
            raise Rejected("Output appeared during build; no existing file was replaced")
    return {"output": str(output), "files": len(files), "embedded_bytes": sum(len(x) for x in files.values()),
            "exe_bytes": output.stat().st_size, "sha256": digest(output.read_bytes())}


def selftest():
    # No game, launcher, account or package files: only synthetic manifest and bytes.
    with tempfile.TemporaryDirectory(prefix="fof-bundle-builder-") as raw:
        root = Path(raw)
        files = {p: (b"synthetic own-code fixture " + p.encode("ascii")) for p in CORE}
        files["steam/native/steam_appid.txt"] = b"480\r\n"
        for p in ROOFTOP:
            files[p] = b"synthetic rooftop fixture " + p.encode("ascii")
        files["README.md"] = b"synthetic README"
        files["Start.cmd"] = b"synthetic launcher stub"
        manifest = {"schema": "fof-private-package-v1", "status": "beta-player-build", "edition": "player",
                    "files": {p: {"size": len(v), "sha256": digest(v)} for p, v in files.items()}}
        for p, data in files.items():
            f = root.joinpath(*p.split("/")); f.parent.mkdir(parents=True, exist_ok=True); f.write_bytes(data)
        (root / "SHA256SUMS.json").write_text(json.dumps(manifest), encoding="utf-8")
        assert set(collect(root)) == set(CORE) | set(ROOFTOP)
        good = collect(root)
        manifest["files"]["probe/native-probe.dll"]["sha256"] = "0" * 64
        (root / "SHA256SUMS.json").write_text(json.dumps(manifest), encoding="utf-8")
        try:
            collect(root)
        except Rejected:
            pass
        else:
            raise AssertionError("corrupt manifest accepted")
        archive(good, root / "fixture.bin")
        assert (root / "fixture.bin").read_bytes().startswith(MAGIC)
    print("BUNDLE_BUILDER_SELFTEST_PASS synthetic-manifest/allowlist/rooftop/hash/archive")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", help="already-built player package directory containing SHA256SUMS.json")
    parser.add_argument("--output", help="new output .exe path; default <package-parent>/FriendOrFoe.exe")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        selftest()
    elif args.package:
        package = Path(args.package).resolve()
        output = args.output or str(package.parent / "FriendOrFoe.exe")
        print(json.dumps(build(package, output), indent=2))
    else:
        parser.error("provide --selftest or --package")


if __name__ == "__main__":
    try:
        main()
    except Rejected as exc:
        raise SystemExit("REJECTED: " + str(exc))
