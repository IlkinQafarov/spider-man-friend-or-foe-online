# Testing and interpretation

Game QA uses bots and bounded cases. Do not substitute a request for someone to play manually for an automated test plan. Test only new copies and session directories; original game installs and saves must remain untouched.

Commands below are source entrypoints, checked against the repository. They are not a claim that all commands were rerun for this documentation change. The [status snapshot](STATUS.md) records completed checkpoints and their limits.

## Own-code checks, without the game or an account

From the repository root, after [building](BUILDING.md):

```powershell
python -I -m unittest discover -s compatibility -p "test_*.py" -v
python -I qa/test_game_lane.py
python -I qa/run_pair.py --selftest
python -I launcher/play_lifecycle.py --selftest
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

This is a same-machine English/Russian bot case with keyboard/controller fixtures, synthetic relay and controlled area swaps. It does not initialize Steam or exercise physical controls. The historical 7,200-tick pass was scenario 5; running this command on current source is a new scenario-10 test with no inherited PASS. The runner creates new run/result folders, records actual owned game and helper exits, and scores only after cleanup. The nominal 7,200 ticks equal 120 seconds of 60 Hz simulation; startup/loading and wall-clock delays are additional.

The pause gate is a separate bounded case:

```powershell
python -I qa/run_pair.py --name pause-001 --frames 3600 --seconds 300 --pause --keyboard-host --jitter --steam-mock
```

`--pause` requires more than 430 frames and excludes mutation/disconnect fixtures. Both seats must record native pause, frozen-world and resume assertions while the usual state and axis checks remain enabled. Scenario 10 limits fixture-only pause assertions to ticks through 430; continuous mode/world/progress checks remain enabled every tick. The exact 3,600-tick pause/keyboard/jitter/mock-relay case passed with normal exits and zero discarded bytes. This does not confer a current-version 7,200-tick/three-area pass. Runs 029/030/031 failed; corrections and scoped results are documented in [Status](STATUS.md#current-source-and-recent-failures).

Use the runner's documented disconnect and deliberate-mutation cases separately. A deliberate divergence detection pass is not a successful combat/session-completion pass.

## Pass criteria

* Content preflights and both expected build/scenario identities pass.
* Required consecutive samples, correct player slots and scenario observations exist.
* Selected states agree; requested fixture transitions are actually observed.
* Games, launchers and relays exit normally, with no forced cleanup or timeout.
* Relay completion exists once, carries the expected bidirectional bytes and discards none for a normal-completion gate.
* Unexpected exceptions, blocked writes, missing records, disconnects and nonzero exits fail the relevant gate.

For an ordinary user stop, require the separate ordered-stop lifecycle evidence; do not mislabel cancellation as finite scenario completion. The historical scenario-5 quiet real-input-path check reached 361 ticks without input fixtures, then stopped normally. It did not test a person using actual keyboard/controller hardware.

## Preparing source for review

```powershell
python -I qa/prepare_publication.py --selftest
python -I qa/prepare_publication.py --snapshot source-preview-001
```

Use a new snapshot name. The helper applies independent source membership, file-type and privacy checks, copies only accepted files and writes a hash inventory. It does not upload, publish Git history or license third-party material. Review the inventory before any authorized publication; it is not a general-purpose secret scanner. Raw game evidence and binaries stay excluded.

## Live Steam is a separate test boundary

Opening the UI and offline tests do not use an account. Host/Join and explicit `--steam-live` do. The completed account test only created and left one lobby; no game traffic or invitations were sent. Actual two-PC gameplay needs an explicitly authorized, bounded test and new evidence. Do not infer WAN readiness from mock transport, ABI fixtures or a lobby smoke pass.

Keep local logs, captures, save data, account IDs and absolute personal paths out of commits. Publish sanitized commands and summaries with the exact build, test scope and failure criteria.
