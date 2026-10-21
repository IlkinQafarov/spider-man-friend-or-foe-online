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

Current source is **scenario 10 / wire protocol 3**. The selected-state sample now includes the full area path, campaign flags, actual mode, previous mode only while pause/NIS restoration uses it, and pause owner only while paused. Mode ranges are guarded. This broadens detection; it is not a complete world-state oracle.

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

The current estimate is **40 / 100 toward a first playable release**:

| Workstream | Earned | Available |
| --- | ---: | ---: |
| Exact-build foundation and language compatibility | 10 | 10 |
| Core input lockstep | 16 | 20 |
| Transport | 6 | 15 |
| Campaign and menus | 0 | 20 |
| Save/resume | 0 | 10 |
| Launcher and packaging | 5 | 10 |
| End-to-end QA | 3 | 15 |
| **Total** | **40** | **100** |

Weights and partial credit are engineering judgement, not empirical test coverage, elapsed effort or a time forecast. “10/10” in the foundation row applies only to the pinned target and known language packs. It does not imply compatibility with all editions. Scores should change when acceptance evidence changes, including regression findings.

The remaining 60 points include unresolved design and validation work. There is no supported release date. See [Roadmap](ROADMAP.md).
