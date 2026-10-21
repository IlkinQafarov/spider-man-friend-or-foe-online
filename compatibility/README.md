# Content compatibility preflight

Private, standard-library-only tooling. It reads game files without running them. The baseline contains relative names, sizes and SHA-256 hashes, never game assets or absolute machine paths.

From this directory, with Python 3.10 or newer:

```text
python preflight.py build-baseline
python -m unittest -v test_preflight.py
python preflight.py verify --game-dir ../test/english --output english-pass-001.json
python preflight.py verify --game-dir ../test/russian --output russian-pass-001.json
```

The supplied directory must be a child of this project's `local/spider-man-friend-or-foe/test/`. All its ancestors and contents must have no symlinks or Windows reparse points. Output must be a new file beneath `compatibility/`; failure creates no result JSON. Existing outputs are never overwritten.

The pinned common digest covers 1,257 critical files: all runtime assets/configuration other than 34 exact language files, plus `Game.exe`, `binkw32.dll`, `Beenox/bxdata.dat`, and the two material files. Every INI file is strict, including `ini/user.ini`. QA modifications must be restored by the separate owner workflow before validation; this tool never restores anything.

All 34 language files must collectively match the known English or Russian pack. This permits different complete packs on the two peers, while rejecting mixed fonts/text/audio and unknown translations. Case-only movie names are normalized to lowercase slash paths. Unknown files are rejected except the 14 known documentation/launcher/icon/uninstall files and generated `GameLog/` contents. Unknown DLLs are rejected even inside ignored locations.

Critical digest: `c46b90afe36571cf3f4fbe3d4e7918da833297f78119d40504e96b4f531b36ea`.

Digest bytes are UTF-8 without BOM: header `coop-fof-critical-v1` + NUL + decimal file count + LF; then ordinal-sorted lowercase slash path + NUL + decimal byte size + NUL + lowercase SHA-256 + LF for each file. Locale digests use `coop-fof-locale-v1`. Neither JSON formatting nor file timestamps enter the digest.

A pass checks on-disk content at preflight time. It does not establish deterministic simulation, mixed-language timing correctness, networking compatibility, or prevent later file changes. The eventual handshake must separately compare the mod/protocol version and shared session settings. New output files are reusable records, not proof of current content after a directory changes.
