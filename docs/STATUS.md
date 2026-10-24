# Status and evidence

Snapshot: **21 October 2026**. The project is an active research preview, with no public release binaries.

## Historical verified checkpoints

The two game gates below used **scenario version 5**. They do not certify current scenario-10 source or every later checksum change.

| Check | Observed result | Practical limit |
| --- | --- | --- |
| Scenario-5 mixed English/Russian paired gate | 7,200 consecutive ticks; three fixture-driven area swaps; keyboard/controller fixtures; injected jitter; mock Steam stream; selected state matched | Same-machine processes, bounded scenario and selected fields |
| Completion of that gate | Every owned game, launcher and mock relay exited 0; each relay sent/received 921,808 bytes; discarded bytes 0 | Normal completion of this test, not campaign completion |
| Scenario-5 quiet unlimited real-input path | 361 ticks with `HUMAN=1`, `HUMAN_FIXTURE=0`; no diagnostic game captures; host-only window close propagated peer STOP; normal exits | Startup/idle/stop lifecycle; no physical input exercise |
| Native relay fixtures | Pinned own-code ABI DLL; host/join callbacks and message release; full-size 16,400-byte messages; bidirectional loopback stream and close/ack tests | Checks our API contract and local stream behavior; no second Steam client |
| Launcher fixtures | Content checks, quoting/environment isolation, delayed peer-ready startup, retained process identity, normal and cancelled cleanup, synthetic connection-test scoring | Own-code children and synthetic evidence, not a remote gameplay session |
| Steam lobby smoke | One friends-only lobby was created and left successfully | One account, no invitation or game traffic |

These summarize completed maintainer checks. Raw evidence stays private because it can contain game captures, local paths and account identifiers. [Testing](TESTING.md) documents the source fixtures and how to produce new local evidence. A pass is always attached to its scope.

## Current source and recent failures

Current source is **scenario 11 / wire protocol 3**. The selected-state sample includes the full area path, campaign flags, actual mode, previous mode only while pause/NIS restoration uses it, and pause owner only while paused. Mode ranges are guarded. This broadens detection; it is not a complete world-state oracle.

| Check | Observed result | Interpretation |
| --- | --- | --- |
| Run 029, scenario 7 | 178 frozen-world frames and native resume at tick 421; axis check failed at tick 422 | The fixture overrode input after native processing, invalidating the axis observation. The fixture moved upstream to the physical provider; the run remains failed. |
| Run 030, scenario 8 | Tick-0 mismatch from inactive previous-mode history: 5 versus 1 while actual mode was 4; world hash matched | Static control-flow analysis shows mode 4 completes without reading previous mode. Scenario 9 canonicalizes that inactive storage and retains previous mode only in active pause/NIS modes 1/5. The run remains failed. |
| Pure mode fixture | Range/active-history checks and two mutation twins passed | Own-code validation of the mode sample, not gameplay QA |
| Pure progress fixture | 16 shared-field mutations detected; 12 intentionally omitted bytes ignored | Includes campaign-flag boundaries; incomplete progress oracle |
| Profile-storage foundation | 39/39 synthetic tests passed | Not integrated into the app; no shared save/resume claim or progress credit |
| QA lease and runners | 13 lease checks, three caught mutation twins and runner selftests passed | Synthetic process/lease safety checks; no game or real slot used by those fixtures |
| Run 031, scenario 9 | Failed at tick 1,287: the completed pause fixture kept asserting against a later legitimate native tutorial/system pause; both clients' selected states still matched | Scenario 10 limits only fixture assertions to ticks through 430. Continuous mode/world/progress checks remain enabled every tick. The run remains failed. |
| Run 032, scenario 10 | All 3,600 consecutive selected-state frames matched; keyboard/controller fixtures, jitter, mock relay, movement, damage and hostile defeat; native pause at 243, 178 frozen frames, resume at 421 and fixture completion at 430 | Bounded same-machine English/Russian pause/combat gate, not a full campaign or remote-session pass |
| Completion of run 032 | Games, launchers and relays all exited 0; each relay sent/received 461,008 bytes, discarded 0; no forced cleanup or timeout | Normal completion of that exact scenario-10 gate |

The source-publication helper has produced an independently scanned, allowlisted source snapshot. It prepares files and an inventory; it does not upload anything or certify arbitrary future contributions.

Run **033**, using scenario 10 with the intended 7,200-tick / three-area options, failed on both clients during startup with native access violations, before network readiness or gameplay. No coverage is credited to that run. Static analysis found an unbounded native keyboard-name lookup; layout-dependent internal names are a possible trigger, but the failed logs do not establish the requested name, descriptor count or layout.

Scenario 11 bounds that lookup, reads native strings safely and rejects missing/malformed bindings with nonzero exit 86 instead of substituting a key. A guarded initializer call selects and validates an **already-loaded standard US layout** for internal names only. It neither loads nor activates layouts, and preserves separate native display labels. Missing supported layout is an explicit failure. The pure key-name fixture passes, including two caught mutation twins; complete configured-binding or hardware coverage is not claimed.

| New check | Observed result | Practical limit |
| --- | --- | --- |
| Run 034, scenario 11 | 7,200 consecutive selected-state ticks matched; three fixture-driven area transitions; English/Russian, keyboard/controller fixtures, jitter and mock relay | Same-machine controlled scenario; ordinary campaign travel remains unverified |
| Completion of run 034 | All games, launchers and relays exited 0; each relay sent/received 921,808 bytes, discarded 0; no timeout or forced cleanup | Normal completion of that bounded gate |
| Run 035, scenario 11 | 3,600 consecutive selected-state ticks matched; movement, damage, hostile defeat; pause at 243, frozen 178 frames, resume at 421, fixture complete at 430 | Same-machine combat/pause fixture, not campaign QA |
| Completion of run 035 | All games, launchers and relays exited 0; each relay sent/received 461,008 bytes, discarded 0; no timeout or forced cleanup | Normal completion of that bounded gate |

Both new runs reported actual and canonical US layout, with 102 keyboard descriptors during binding lookup. They support the guarded path without reproducing the original layout/target from run 033. Its precise trigger and broader startup/layout compatibility remain unconfirmed.

Run **036**, also scenario 11, passed the quiet fixture-disabled gameplay lifecycle: at least 361 common ticks, four sparse observations, no diagnostic game captures, host-only owned-window close, ordered peer stop and normal exits. The pre-game movie/bootstrap route was slow: readiness was still pending at 212 seconds and the gate completed at about 221 seconds within its 240-second bound. This is startup/idle/shutdown evidence, not a physical-input or retail-campaign pass. It uses the controlled research bootstrap.

## Retail campaign bootstrap (scenario 12, private feature branch)

| Check | Observed result | Practical limit |
| --- | --- | --- |
| Runs 037-039 | Stopped at native save notices, then ran out of the 300 s budget on the first-level loading screen | Retained as FAIL |
| Runs 044-050 | Both seats reached the native Helicarrier hub. Hub walking needed (1) held stick input re-applied after the campaign input clears, (2) a host-only bot that walks P1 into the `LevelSelect01` trigger, (3) peer following host P1 position in the hub only, and (4) excluding position from the Accept confirmation | Retained as FAIL; each fixed one blocker |
| Run 051 | **PASS.** English host and Russian peer: native New Game, save prompts, hub, walk to the mission terminal, mutual Accept, synchronized Tokyo load, then 600 consecutive equal sampled-state ticks. All games, launchers and relays exited 0; 156,624 bytes each way, 0 discarded | One PC, mock Steam, bot input, first mission only |
| Run 052 | **PASS**, identical repeat of 051 with the same build and options | Same limits |
| Runs 053-058 | Retail tutorial hints pause gameplay (native mode 1) until OK is pressed; the research travel fixture is unreliable on the campaign route; a network bug dropped a checkpoint message that arrived together with the peer's "finished" signal (fixed, with a regression self-test) | Retained as FAIL |
| Runs 059, 060 | **PASS.** After tutorials are accepted through lockstep input, a test fixture calls the game's own mission-end routine on both seats on the same tick. Both show the results screen, leave it on the same tick, reload the hub through the shared checkpoint and agree on the progress bytes, then finish and exit normally | The fixture skips the real end-of-level script, so mission completion is **not** shown to be recorded (progress changed at the first tutorial hint, not at mission end) |
| Run 061 | **PASS.** Two cycles: mission, end, hub, walk to the terminal again, second mission start, 2,400 equal sampled ticks | Same limits |
| Run 064 | **PASS.** The launcher's play path (`--play`, unlimited) on the campaign route, bot driving only host menus/hub: 361 matching frames, host-only close, ordered peer stop, normal exits | Took about 9 minutes to reach gameplay with unskipped movies |

The GUI's Play mode now uses this campaign route with the host's controls driving menus and the hub. The automated connection check keeps the bounded research scenario. Only New Game and the first slot are accepted.

Hub movement is not deterministic (it uses wall-clock time), so the peer's Spider-Man is snapped to the host's position there. That is acceptable in the hub, which has no enemies or progress, and is not used inside missions.

## Implemented, with incomplete validation

The code includes two-peer input exchange, native player routing, fixed gameplay timing, selected state checks, complete-pack language verification, host/join UI, an x64 native relay and ordered user-stop handling. The real-input path accepts keyboard/controller providers, but physical device usability remains untested. The packaging helper creates an explicit asset-free file set, but no release package is offered here.

## Not established

* Two different PCs exchanging game traffic through Steam or across a WAN.
* Full campaign progression, ordinary menu-driven transitions, bosses and cutscenes.
* Persistent shared saving, resume, reconnect, late join or host migration.
* A complete enemy, mission, combat, animation and progress state oracle.
* Long sessions, broad hardware compatibility, latency tolerance or physical input focus/usability.
* Support for a different executable, storefront build, translation or engine family.

The three area swaps are controlled fixtures. They are useful transition coverage and do not certify ordinary campaign travel. Matching sampled hashes cannot establish equality of every unobserved game field.

## Progress estimate

The current estimate is **63 / 100 toward a first playable release** (2026-10-24, v0.1.6-alpha-player; was 40):

| Workstream | Earned | Available |
| --- | ---: | ---: |
| Exact-build foundation and language compatibility | 10 | 10 |
| Core input lockstep (incl. pause, character swap) | 17 | 20 |
| Transport (Steam relay; no real two-PC session yet) | 8 | 15 |
| Campaign and menus (New Game/Continue, hub, first mission and return, join/rejoin, start together, sidekick choice) | 11 | 20 |
| Save/resume (host saves and Continue; shared saves not done) | 3 | 10 |
| Launcher and packaging (player releases, 11 languages, kept settings) | 9 | 10 |
| End-to-end QA (bots on one PC only) | 5 | 15 |
| **Total** | **63** | **100** |

Weights and partial credit are engineering judgement, not empirical test coverage, elapsed effort or a time forecast. “10/10” in the foundation row applies only to the pinned target and known language packs. It does not imply compatibility with all editions. Scores should change when acceptance evidence changes, including regression findings.

The remaining 60 points include unresolved design and validation work. There is no supported release date. See [Roadmap](ROADMAP.md).
