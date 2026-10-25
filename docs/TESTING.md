# Testing and interpretation

Game QA uses bots and bounded cases. Do not substitute a request for someone to play manually for an automated test plan. Test only new copies and session directories; original game installs and saves must remain untouched.

Commands below are source entrypoints, checked against the repository. They are not a claim that all commands were rerun for this documentation change. The [status snapshot](STATUS.md) records completed checkpoints and their limits.

The current player pre-release is **v1.0.1-beta**. Commands below target the **scenario-12 / protocol-5 source**; historical results remain tied to their original build. Both test peers must use matching source/builds; older protocols are rejected. Historical scenario-5/10/11 passes remain attached to their original builds.

## Own-code checks, without the game or an account

From the repository root, after [building](BUILDING.md):

```powershell
python -I -m unittest discover -s compatibility -p "test_*.py" -v
python -I qa/test_game_lane.py
python -I qa/run_pair.py --selftest
python -I launcher/play_lifecycle.py --selftest
cmd /c probe\build-menu-policy-selftest.cmd
python -I launcher/test_profile_store.py
python -I qa/prepare_publication.py --selftest
python -I analysis/native/score_pair.py --selftest
python -I launcher/build_package.py --selftest
python -I -m unittest discover -s steam -p "test_*.py" -v
.\probe\native-progress-selftest.exe
.\probe\native-mode-selftest.exe
.\probe\native-wire-selftest.exe
.\probe\wire-stop-selftest.exe
.\steam\native\native-relay.exe --abi-selftest
.\steam\native\native-relay.exe --protocol-selftest
```

These use synthetic files, own-code processes or loopback sockets. The Steam ABI fixture needs the project-built `steam/api_abi_mock.dll` and its reviewed pin; it never selects the real Steam backend. A changed mock binary deliberately requires a pin review. Loopback tests need local socket permission and available ports; a sandbox denial is a failed prerequisite, not a transport pass.

The WinForms own-code modes are `--selftest`, `--connection-selftest`, `--offline-session-selftest` and `--offline-connection-selftest`. The GUI binary is built as `winexe`; explicitly redirect output and wait for it. For example:

```powershell
$exe = (Resolve-Path .\launcher\fof-private-launcher.exe).Path
$log = Join-Path (Resolve-Path .\qa).Path ("ui-fixture-" + [guid]::NewGuid().ToString("N"))
$child = Start-Process -FilePath $exe -ArgumentList "--selftest" -WindowStyle Hidden -PassThru -Wait -RedirectStandardOutput "$log.stdout.txt" -RedirectStandardError "$log.stderr.txt"
Get-Content "$log.stdout.txt"
if ($child.ExitCode -ne 0) { throw "UI fixture failed; inspect the new logs." }
```

Repeat with the other modes using new log names. Synthetic fixture evidence must remain labelled as synthetic. It cannot establish actual game, physical hardware or remote account behavior.

## Before a real-game gate

Use two verified new copies at `test/english/` and `test/russian/`. The preflight is read-only and refuses existing output names:

```powershell
python -I compatibility/preflight.py verify --game-dir test/english --output compatibility/en-preflight-001.json
python -I compatibility/preflight.py verify --game-dir test/russian --output compatibility/ru-preflight-001.json
```

Do not regenerate the baseline to make unknown or changed content pass. A manifest pass checks bytes at preflight time, not future file immutability or synchronization.

Game gates require the exclusive QA lease implemented by `qa/game_lane.py` and the `coop-anything` hub's `tools/gameslot/` backend. Set `COOP_ANYTHING_ROOT` to that checkout when it cannot be discovered as an ancestor. The paired/lifecycle runners perform content preflight first, then acquire the lease before the first launch and retain it through owned-process cleanup. Scoring happens after release. A missing helper or an occupied lane fails closed. A worker must never overlap a second game gate or kill someone else's processes to free the lane. `qa/test_game_lane.py` tests the helper with synthetic state and never touches a real slot or game.

## Bounded paired-game example

After the lease helper and copies are ready, use a new name every time:

```powershell
python -I qa/run_pair.py --name mixed-001 --frames 7200 --seconds 300 --keyboard-host --steam-mock --jitter --transition --transition-count 3
```

This is a same-machine English/Russian bot case with keyboard/controller fixtures, synthetic relay and controlled area swaps. It does not initialize Steam or exercise physical controls. Historical 7,200-tick passes exist for scenarios 5 and 11; a run on the current working tree needs its own result. The runner creates new run/result folders, records actual owned game and helper exits, and scores only after cleanup. The nominal 7,200 ticks equal 120 seconds of 60 Hz simulation; startup/loading and wall-clock delays are additional.

The pause gate is a separate bounded case:

```powershell
python -I qa/run_pair.py --name pause-001 --frames 3600 --seconds 300 --pause --keyboard-host --jitter --steam-mock
```

`--pause` requires more than 430 frames and excludes mutation/disconnect fixtures. Both seats must record native pause, frozen-world and resume assertions while the usual state and axis checks remain enabled. The historical scenario-10 correction limited fixture-only pause assertions to ticks through 430; continuous mode/world/progress checks remained enabled every tick. Runs 032 and 035 passed their exact pause gates, while 029/030/031 remain failed. Those results do not confer a pass on a rebuilt version; see [Status](STATUS.md#current-source-and-recent-failures).

Use the runner's documented disconnect and deliberate-mutation cases separately. A deliberate divergence detection pass is not a successful combat/session-completion pass.

## Current campaign and tutorial regression commands

Use a fresh lowercase run name for every command; replace the numeric suffixes below when they already exist. These use the research build and mock Steam, not a real account. The ordinary campaign route and the shortened level runner are different test scopes.

```powershell
python -I launcher/play_lifecycle.py --name campaign-regress-001 --campaign --join-at-hub --skip-cutscenes --seconds 1500 --frames 3600 --quiet
python -I launcher/play_lifecycle.py --name tutorial-host-001 --campaign --together --tutorial-ui --skip-cutscenes --seconds 900 --frames 1800 --quiet
python -I launcher/play_lifecycle.py --name tutorial-bad-net-001 --campaign --together --tutorial-ui --skip-cutscenes --seconds 900 --frames 1800 --quiet --net-delay-ms 120 --net-jitter-ms 40 --net-stall-every 10 --net-stall-ms 600
```

`--tutorial-ui` requires fresh `--campaign --together`, at least 600 frames and no alternate start, seeded save, rejoin or inherited level-ending/alternate-route research settings. It starts the native first Tokyo area and exercises its real hint popup instead of jumping to a mission ending. The strict scorer requires matching ordered `OPEN`, `PEER_IGNORED`, `HOST_ACCEPT` and `CLOSED` markers on both seats, three ignored peer attempts, at least 120 ticks of withheld host acceptance, matching state and normal exits. The host acceptance must use its transported pointer input; a local synthetic key bypass is insufficient.

The unreleased tutorial-host-493 gate passed with 1,801 matching common frames and marker ticks 744 / 770 / 870 / 883. Tutorial-bad-net-494 also passed, with markers 749 / 780 / 880 / 893, normal exits and no forced cleanup. Its proxy was configured for 120 ms one-way delay, 40 ms jitter and 600 ms stalls every 10 seconds. These are focused local regression passes, not a published fix or a full-mission/WAN pass.

For the unreleased in-mission rejoin route:

```powershell
python -I launcher/play_lifecycle.py --name rejoin-mission-001 --campaign --join-at-hub --rejoin-in-mission --skip-cutscenes --seconds 1500 --frames 1800 --quiet
```

Rejoin-mission-492 passed a first Tokyo-area restart through the hub. It does not restore the interrupted fight. Bad network, later areas, boss arenas and joining during a cutscene still need separate gates.

## Native save-slot regression candidates: unverified

The working-tree slot-selection/overwrite code has not yet earned a native game pass. Its own-code scorer fixtures do not prove that an occupied save can be safely overwritten. Use only a save generated by an isolated test run, never an owner's profile; the runner validates the seed and stages private copies.

`--slot-fixture` takes a zero-based host row 0, 1 or 2 and requires `--campaign --together`. `--seed-host-slots` and `--seed-peer-slots` accept comma-separated zero-based rows or `none`. A supplied seed defaults to host row 0 and no peer rows. `--overwrite-no` requires a seeded occupied host row and an available peer row. The following are verification candidates, not completed results:

```powershell
# Set this to a native save produced by a previous isolated test.
$generatedSave = 'path-to-generated-test-save.SFOFSaveGame'
python -I launcher/play_lifecycle.py --name slot-yes-001 --campaign --together --slot-fixture 0 --seed-save "$generatedSave" --seed-host-slots 0 --seed-peer-slots 0 --skip-cutscenes --seconds 900 --frames 1800 --quiet
python -I launcher/play_lifecycle.py --name slot-no-001 --campaign --together --slot-fixture 0 --seed-save "$generatedSave" --seed-host-slots 0 --seed-peer-slots 0 --overwrite-no --skip-cutscenes --seconds 900 --frames 1800 --quiet
python -I launcher/play_lifecycle.py --name slot-empty-001 --campaign --together --slot-fixture 2 --skip-cutscenes --seconds 900 --frames 1800 --quiet
python -I launcher/play_lifecycle.py --name slot-full-peer-001 --campaign --together --slot-fixture 0 --seed-save "$generatedSave" --seed-host-slots 0 --seed-peer-slots 0,1,2 --skip-cutscenes --seconds 900 --frames 1800 --quiet
```

Yes and later-empty cases require the expected native profile rows, generated save filename/payload slot index, preservation of unrelated seeded files and the normal matching-gameplay gate. No and full-peer cases require the exact native cancellation/refusal evidence, no profile/gameplay initialization, unchanged seed inventories and normal owned exits. A full peer profile must not silently fall back to overwriting row 0. A negative safety outcome can pass its own gate without being called a gameplay completion pass.

## Pass criteria

* Content preflights and both expected build/scenario identities pass.
* Required consecutive samples, correct player slots and scenario observations exist.
* Selected states agree; requested fixture transitions are actually observed.
* Games, launchers and relays exit normally, with no forced cleanup or timeout.
* Relay completion exists once, carries the expected bidirectional bytes and discards none for a normal-completion gate.
* Unexpected exceptions, blocked writes, missing records, disconnects and nonzero exits fail the relevant gate.

For an ordinary user stop, require the separate ordered-stop lifecycle evidence; do not mislabel cancellation as finite scenario completion. The historical scenario-5 quiet real-input-path check reached 361 ticks without input fixtures, then stopped normally. It did not test a person using actual keyboard/controller hardware.

Historical scenario-11 gates are recorded in [Status](STATUS.md): runs 034/035 cover three fixture-driven area transitions and native pause/resume. Run 036 repeats the quiet fixture-disabled gameplay lifecycle with normal ordered stop; its controlled pre-game route took about 212 seconds to become ready. These early checks did not establish physical input usability or ordinary campaign flow. Later campaign and user-reported Steam evidence is listed separately there.

After building, `probe/native-input-key-selftest.exe` exercises bounded key lookup, guarded native-string parsing and selection of an already-loaded exact US layout, including two mutation twins. It uses synthetic memory/layout lists, not game data or OS layout changes. The runtime additionally validates 17 physical translations and checks each actual requested keyboard binding; it does not install or activate layouts.

## Preparing source for review

```powershell
python -I qa/prepare_publication.py --selftest
python -I qa/prepare_publication.py --snapshot source-preview-001
```

Use a new snapshot name. The helper applies independent source membership, file-type and privacy checks, copies only accepted files and writes a hash inventory. It does not upload, publish Git history or license third-party material. Review the inventory before any authorized publication; it is not a general-purpose secret scanner. Raw game evidence and binaries stay excluded.

## Live Steam is a separate test boundary

Opening the UI and offline tests do not use an account. Host/Join and explicit `--steam-live` do. The initial account smoke created and left one lobby. Later, the owner reported completing the first Tokyo mission with a friend on two PCs over real Steam; this is user-reported gameplay evidence, not a local automated gate or proof of all later missions. The beta session also exposed the tutorial and save-slot defects now being addressed in the unreleased working tree.

Further account use, invitations or remote-PC tests need their own authorization and bounded evidence. Existing reports do not authorize automated account actions. Do not infer broad WAN readiness from mock transport, ABI fixtures, lobby smoke or one successful mission.

Keep local logs, captures, save data, account IDs and absolute personal paths out of commits. Publish sanitized commands and summaries with the exact build, test scope and failure criteria.
