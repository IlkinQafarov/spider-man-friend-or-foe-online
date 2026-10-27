# Testing and interpretation

Game QA uses bots and bounded cases. Do not substitute a request for someone to play manually for an automated test plan. Test only new copies and session directories; original game installs and saves must remain untouched.

Commands below are source entrypoints, checked against the repository. They are not a claim that all commands were rerun for this documentation change. The [status snapshot](STATUS.md) records completed checkpoints and their limits.

The player pre-release **v1.1.0-beta** uses scenario 13 / protocol 6. Production live joining is enabled by default for retail campaigns; the fixture flag is for research builds only. Both test peers need matching builds. The earlier v1.0.1-beta uses scenario 12 / protocol 5.

## Live mission admission

The release reconstructs a joining client's mission from bounded recorded inputs, checks every recorded state sample, and resumes at the host's current tick. The host waits in place; it does not return to the hub or restart an area. Native AI/player transitions preserve existing actors, positions, health and area identity. The completed checks below retain their bounded local scope.

```powershell
cmd /c probe\build-live-join-selftest.cmd
python -I qa/live_join_replay.py --selftest
python -I launcher/play_lifecycle.py --selftest
python -I launcher/play_lifecycle.py --name live-rejoin-check --campaign --join-at-hub --rejoin-in-mission --live-join --skip-cutscenes both --seconds 900 --frames 1800 --quiet
python -I launcher/play_lifecycle.py --name live-first-check --campaign --join-at-hub --live-first-join --skip-cutscenes both --seconds 900 --frames 1800 --quiet
```

Use fresh names. The reconnect fixture closes its initial peer during the mission and launches a fresh replacement. The first-join fixture lets the host complete at least 600 solo frames before connecting the friend. Both require matching completion ticks, a continuing host timeline, no host hub/restart path, matching states after admission and clean exits. Only pre-admission history may contain AI controllers; compared post-admission frames require two human controllers.

Recorded reconnect check **522 passed** in English/Russian over loopback: admission at tick 655, 1,866 matching post-admission frames, both game and launcher exits 0. This and first-arrival check 524 are historical first-area research proofs, not candidate release verification; they do not establish later areas, bosses, real controllers or a two-PC Steam admission run. See [status](STATUS.md) for current candidate results.

First-arrival check **524 passed** over English/Russian loopback: the host played solo with native AI before the friend linked, admission completed at tick 857, then both games matched for 1,904 frames. Both game and launcher exits were 0. This uses the same first-area coverage limit.

### Candidate runs and failure accounting

| Check | Recorded result |
| --- | --- |
| 525 | **FAIL:** launcher restriction stopped the attempt before game start; no gameplay evidence. |
| 526–528 | **FAIL / no pass credit:** first-chance Windows UI Automation exceptions were misclassified as faults. |
| 529 | **FAIL / no frame credit:** negative quiet-sample harness bug. |
| 530 | **PASS:** guest closed after CATCHUP; host canceled at tick 857 and continued 463 ticks. Both games and launchers exited 0; no forced cleanup. |
| 531 | **PASS:** 300-frame history cap disabled at tick 301; guest refused at tick 632 and self-closed via STOP. Host continued 448 ticks; both games and launchers exited 0; no forced cleanup. |
| 532 | **PASS:** first join under 120 ms delay, 40 ms jitter and 600 ms stalls every 10 seconds. All 857 replay ticks matched; admission at tick 857; 1,904 matching post-admission frames; negotiated 14-frame delay. Both games and launchers exited 0, no forced cleanup; elapsed 294.547 s. |
| 533 / 534 | **FAIL / not run:** the swap fixture fired during a cutscene, so 533 earns no swap pass; the sequence stopped before 534. |
| 535 | **PASS:** host swapped at tick 235 before disconnect; all 743 replay ticks matched, admission completed at 743, and 1,898 post-admission frames matched. Both games and launchers exited 0; no forced cleanup. |
| 536–542 | **FAIL:** stale NIS stream state blocked idle Doc Ock admission; then cross-area replay failed at ticks 617–618 with matching RNG but local controls enabled early. Experiments that paced around or exited the area were removed after the native loading-VM readiness gate was identified. |
| 543–546 | **FAIL:** stale readiness at tick 601, an unrecorded NIS start at 667 after matching through 666, guest local-VM readiness arriving late at 617, and a wrong replay frame index. The NIS gate now uses agreed readiness plus local-loaded state; controls release uses recorded readiness and falling/rising edges with the correct tick-adjusted replay index. |
| 547 | **PASS:** offline Russian replay of the retained 545 host journal matched all 2,074 frames, including the native Tokyo 2/1-to-3 exit and full Doc Ock intro. Game and launcher exited 0. This is replay validation, not a live cross-area admission pass. |
| 548 | **PASS:** live admission inside Doc Ock’s arena after the native Tokyo 2/1-to-3 exit and intro; all 2,074 history frames and 1,887 post-admission frames matched. Both games and launchers exited 0; no forced cleanup; elapsed 318.157 s. |
| 549 | **PASS:** player-edition campaign/live-join startup and normal owned close; game/launcher exit 0, bot request refused before attachment, no native fault or forced cleanup. |

The current handler logs standard MSVC C++ first-chance notifications as `CPP_FIRST_CHANCE` and returns `CONTINUE_SEARCH`; a caught notification alone is not a test failure. Unhandled exceptions, nonzero exits, native faults, `NET FAIL` markers and state-oracle mismatches still fail the gate. Checks 530, 531, 532 and 535 remain bounded passes; 548 passed the live boss-area case and 549 passed the player-edition smoke; their scope remains local.

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

## Native save-slot regression fixtures

The working-tree slot-selection/overwrite code has not yet earned a native game pass. Its own-code scorer fixtures do not prove that an occupied save can be safely overwritten. Use only a save generated by an isolated test run, never an owner's profile; the runner validates the seed and stages private copies.

`--slot-fixture` takes a zero-based host row 0, 1 or 2 and requires `--campaign --together`. `--seed-host-slots` and `--seed-peer-slots` accept comma-separated zero-based rows or `none`. A supplied seed defaults to host row 0 and no peer rows. `--overwrite-no` requires a seeded occupied host row and an available peer row. The following reproduce the focused save-choice regressions; see Status for the completed native Yes/No/full-slot results and their limits:

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

Opening the UI and offline tests do not use an account. Host/Join and explicit `--steam-live` do. The initial account smoke created and left one lobby. Later, the owner reported completing the first Tokyo mission with a friend on two PCs over real Steam; this is user-reported gameplay evidence, not a local automated gate or proof of all later missions. The beta session also exposed the tutorial and save-slot defects fixed in v1.0.1-beta.

Further account use, invitations or remote-PC tests need their own authorization and bounded evidence. Existing reports do not authorize automated account actions. Do not infer broad WAN readiness from mock transport, ABI fixtures, lobby smoke or one successful mission.

Keep local logs, captures, save data, account IDs and absolute personal paths out of commits. Publish sanitized commands and summaries with the exact build, test scope and failure criteria.
