# Status and evidence

Snapshot: **26 October 2026**. The latest player pre-release is [v1.0.1-beta](https://github.com/IlkinQafarov/spider-man-friend-or-foe-online/releases/tag/v1.0.1-beta). This document distinguishes earlier beta evidence, the current release changes, automated fixture evidence and user-reported play.

## Published beta and later campaign evidence

The beta provides the player launcher and native campaign route. According to the owner's two-PC session reports, real players completed the first Tokyo mission together over real Steam. This is **user-reported gameplay evidence**, distinct from the automated local gates below; it does not establish later missions, all hardware or arbitrary network conditions. The earlier lobby-only smoke remains historical evidence, not the limit of all subsequent Steam testing.

| Check | Recorded result | Practical limit |
| --- | --- | --- |
| Full-campaign-488, before the beta release | **PASS:** 60,001 common frames, all 20 mission endings from Tokyo 1 to Venom, seven boss lifecycle paths and 20 matching hub-return progress checkpoints | Bots on one PC with a mock Steam relay. The research level runner shortens missions to their endings and invokes boss-defeat script paths; this is not 20 complete ordinary missions played through combat. |
| Regional bad-network cases | Tokyo, Egypt, Island, Nepal 1–3 and Transylvania 1–2 completed their shortened mission paths with simulated delay, jitter and periodic stalls | Separate local fixtures, not one complete bad-network campaign or a WAN certification. |
| Exact-build game repairs | Guarded repairs for the damaged Mysterio laser code and empty Transylvania 3 draw-list item were exercised before release | Applies only to the pinned executable and expected damaged instructions; no support for unrelated builds is implied. |
| Owner's real Steam sessions | **User reported:** first Tokyo mission played together on two PCs; pressure plates and synchronization worked during play | Later missions remain bot-tested. The evening beta session also exposed the tutorial and save-slot defects below. |

## v1.0.1-beta changes

Current campaign source uses **scenario 12 / wire protocol 5**. Protocol 5 carries the host pointer through the synchronized input stream and rejects incompatible peers. These changes are included in v1.0.1-beta and **not included in v1.0.0-beta**; both peers need matching builds.

| Change or check | Current evidence | Remaining limit |
| --- | --- | --- |
| Mission tutorial and host menu ownership | Native tutorial popups use mission lockstep input instead of the hub-only shared-menu path. In-mission popup/pause controls belong to the session host, including after character swaps; local movie-skip handling is retained. | The focused tutorial gates below cover one first-area popup, not every menu, pause option or physical mouse/controller combination. |
| Tutorial-host-493 | **PASS:** native first Tokyo area; 1,801 matching common frames. Popup opened at tick 744; three peer Accept attempts were ignored at 770; transported host pointer click accepted at 870; popup closed at 883. Both owned launchers exited 0, with no errors or forced cleanup. | Same-machine English/Russian bots and mock Steam. The fixture withholds host acceptance before exercising it; no shortened level-runner route is used. |
| Tutorial-bad-net-494 | **PASS:** 1,801 matching common frames; corresponding markers at 749 / 780 / 880 / 893. Configured 120 ms one-way delay, 40 ms jitter and 600 ms stalls every 10 seconds; elapsed 239.093 seconds. Both exits 0, no errors or forced cleanup. | Simulated bad network on one PC. This repeats the focused tutorial test, not a full mission or remote-PC regression. |
| In-mission rejoin, rejoin-mission-492 and rejoin-bad-net-506 | **PASS:** peer left Tokyo 1 after 601 common frames; host continued alone, returned to the Helicarrier, accepted a replacement peer and both resumed Tokyo/1_Level/1_Area. Each run reached 1,801 matching new-session frames and clean exits. 506 used 120 ms one-way delay, 40 ms jitter and 600 ms stalls every 10 seconds; replacement watched its opening while host waited; elapsed 656.875 s. | Area restart through the hub, not restoration of interrupted combat. Later areas, boss arenas and a new join arriving during the host's cutscene remain unverified. This work postdates the published beta. |
| Host save-slot selection and overwrite | **PASS:** later empty slot495 (host row2 / peer row0, 601 matching frames); native Yes502 (601 matching frames); No503 and full-peer504. Native Yes/No prompt and each local profile route were exercised. No/full required unchanged seed bytes, size and timestamps, no profile initialization or mission load, and clean exits. | Generated sandbox saves on one PC. This does not attest all native write failures, corrupted saves or shared Continue recovery. |
| Phase-stop shutdown, phase-stop-overwrite-505 | **PASS:** host closed during the native overwrite prompt; peer recorded one deferred owned-window close and unwound normally. Both exits 0, no exception or forced cleanup, no later shared save/load operations, all seeded files unchanged. | Focused menu-phase STOP regression for the reported shutdown failure family; not a claim that every possible shutdown crash is fixed. |
| Own-code regression checks | Progress, mode, key, campaign-phase, mission-menu policy/mutation and lifecycle fixtures passed. Wire tests include pointer round-trip and malformed/old-protocol rejection. | Synthetic evidence only; it cannot certify native save writes, every UI path or real hardware. |

The beta's reported tutorial dismissal and inability to choose an occupied New Game slot are therefore separate from the unreleased fixes. The tutorial, native save-choice and focused shutdown fixes have the bounded passes above. The first overwrite test stalled because the bot used vertical controls on a horizontal Yes/No dialog; corrected native Left/Right fixture controls produced the successful gate. Failed runs remain failures. These changes are included in v1.0.1-beta.

## Historical verified checkpoints

The two early game gates below used **scenario version 5**. They do not certify the current working tree or every later checksum change.

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

This section preserves the **historical scenario 7–11 / wire-protocol-3** investigation. Scenario 11 is not the current source version. Its selected-state sample included the full area path, campaign flags, actual mode, previous mode only while pause/NIS restoration used it, and pause owner only while paused. Those checks broadened detection without providing a complete world-state oracle. Failed runs below remain failed after later fixes.

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

## Historical retail campaign bootstrap (scenario 12)

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

At this early checkpoint, the GUI adopted the native campaign route, while the automated connection check retained the bounded research scenario. The then-current restriction to New Game and the first slot is historical; later published builds added Continue and joining/rejoining at the hub. The current overwrite change is still unverified as described above.

The campaign implementation synchronizes hub position to the host because native hub movement uses local timing. This correction is scoped to the hub and is not evidence of deterministic mission movement or complete hub-state coverage.

## Implemented, with incomplete validation

The project includes two-peer input exchange, native player routing, fixed gameplay timing, selected state checks, complete-pack language verification, the host/join UI, an x64 native relay and ordered user-stop handling. Player packages have been published. Keyboard play has user-reported two-PC evidence; physical gamepads and broad layout/focus/device compatibility remain unverified. Working-tree changes require fresh gates and a separately approved release.

## Not established

* All 20 ordinary missions played start to finish on two PCs. The later automated campaign uses shortened mission-ending fixtures.
* One full campaign over the simulated bad network, or broad real-WAN coverage. Rare Rhino-intro and Transylvania-start mismatches remain watch items.
* Persistent shared saving and Continue from both seats, including error handling beyond the bounded native overwrite cases above.
* Broader in-mission rejoin beyond the first-area bad-network gate: later areas, bosses and host cutscenes; exact interrupted-world restoration or host migration.
* A complete enemy, mission, combat, animation and progress state oracle.
* Broad hardware/gamepad compatibility, long-session reliability and all physical input focus arrangements.
* Support for a different executable, storefront build, translation or engine family.

The three area swaps are controlled fixtures. They are useful transition coverage and do not certify ordinary campaign travel. Matching sampled hashes cannot establish equality of every unobserved game field.

## Progress estimate

The current estimate remains **about 85 / 100 toward the intended playable co-op experience** (26 October 2026, v1.0.1-beta). This is engineering judgement about milestones, not measured test coverage, a claim of 85% campaign completion, or a delivery forecast. The fixes above do not automatically increase it.

For history, the earlier v0.1.6-alpha-player estimate was **63 / 100** on 24 October (previously 40), using this breakdown:

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

The historical table is not a current scorecard. Shared saves, remaining native UI/rejoin validation, complete ordinary missions, controller coverage and regression findings still require work. See [Roadmap](ROADMAP.md); this snapshot makes no release-date commitment.
