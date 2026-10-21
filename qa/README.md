# Paired-client research QA

`run_pair.py` starts bounded English/Russian game pairs through the content preflight, guarded native launcher and strict pair scorer. It installs nothing, changes no game configuration, restores no files and uses no account. Both isolated copies must already have their original `ini/user.ini`. See [Testing](../docs/TESTING.md) and the [versioned evidence](../docs/STATUS.md).

## Own-code checks

From the source root:

```powershell
python -I qa/test_game_lane.py
python -I qa/run_pair.py --selftest
python -I launcher/play_lifecycle.py --selftest
python -I qa/prepare_publication.py --selftest
```

These modes use synthetic state; they do not start games, load Steam or touch a real QA slot. `game_lane.py` tests cover retained process identity, exclusive lease behavior and caught mutation twins. Runner fixtures keep timeout, forced completion, malformed evidence and deliberate divergence distinct from successful gameplay completion.

## Exclusive game lane

Set `COOP_ANYTHING_ROOT` to a checkout of the [coop-anything hub](https://github.com/cranebeaker/coop-anything) containing `tools/gameslot/gameslot.py`, unless an ancestor checkout can be discovered. The backend is reused from that checkout, not vendored. An inherited `GAMESLOT_DIR` must remain inside the selected hub's private `local/` directory.

The runners perform content preflights **before acquiring the lease**. They then hold it from the first owned helper/game launch through owned-process cleanup, release it and score the saved evidence. Missing helper/backend, unavailable lane or invalid process identity fails closed. Foreign leases are not reaped, and no unrelated process is stopped to free a lane.

## Bounded game gates

Use a new name for every run:

```powershell
python -I qa/run_pair.py --name mixed-001 --frames 7200 --seconds 300 --keyboard-host --steam-mock --jitter --transition --transition-count 3
python -I qa/run_pair.py --name pause-001 --frames 3600 --seconds 300 --pause --keyboard-host --jitter --steam-mock
```

The first command is a new test, not a current-source PASS claim. Historical 7,200-tick/three-area and 361-tick quiet lifecycle passes used scenario 5. Current scenario 10 / wire protocol 3 passed the second command's bounded 3,600-tick pause/keyboard/jitter/mock-relay case: selected state matched, games/launchers/relays exited 0, and each relay sent/received 461,008 bytes with zero discarded bytes. No current-version three-area pass is claimed.

`--frames` accepts 60..12000 (default 3600), `--seconds` accepts 60..300 (default 240), and `--area` defaults to `Tokyo/1_Level/1_Area`. Names are at most 64 ASCII letters/digits with single hyphens between groups. `--pause` requires more than 430 frames and excludes mutation/disconnect fixtures. Both peers must report native pause, frozen-world and resume assertions. Fixture-only assertions stop at tick 430; ordinary axis and continuous mode/world/progress checks stay enabled. Shorter cases are not exempted from missing combat or requested fixture observations.

`--disconnect-host`, `--mutate-host`, `--jitter`, `--transition`, `--transition-count`, `--keyboard-host` and `--steam-mock` select separate bounded fixtures. A mutation pass means the intended divergence was detected; it is not successful scenario completion. `--steam-mock` uses only own loopback helpers and no account transport.

## Copies, output and scoring

The source root is the script's parent folder. Host uses `test/english/Game.exe`; peer uses `test/russian/Game.exe`. Before launch, both manifests must pass into new `compatibility/qa-NAME-host.json` and peer files, with equal critical digests. Inherited `COOP_PROBE_*` values are removed. Native-input bot flags, shared scenario and finite frame count are set explicitly.

New game sandboxes are `run-NAME-host` and `run-NAME-peer`; private evidence is saved under new `qa/result-NAME/`. Actual game `PROCESS_EXIT` codes and launcher/helper return codes are checked separately. Missing/conflicting markers, unexpected exceptions, timeouts, forced completion, incomplete stream drain and nonzero exits reject a normal-completion gate. A zero launcher code cannot hide a failed game.

Children launch hidden and cleanup uses only retained owned process/job handles. Nothing searches for process names or kills a game or Steam process it did not start. The runner prints its summary and exits 0 only when its strict gate passes. These gates remain bounded same-machine evidence, not WAN, full-campaign, persistent-save or physical-input certification.

## Preparing a public source snapshot

```powershell
python -I qa/prepare_publication.py --snapshot source-preview-001
```

Choose a new name. This helper applies an independent source allowlist, regular-file checks and privacy checks, writes a source-only copy and hash inventory, and reads it back. It uploads nothing and copies no Git history. Review the inventory before authorized publication. Binaries, game assets, raw logs, captures, saves and account data remain excluded; the helper is not a universal secret scanner or license audit.
