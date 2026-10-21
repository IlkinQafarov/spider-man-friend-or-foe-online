![Two independent clients connected through a shared input stream](docs/banner.svg)

# Spider-Man: Friend or Foe — co-op research

Two friends, two Windows game instances, and an independent English or Russian language choice on each PC. This project connects the game's native two-player controls through a synchronized input stream.

**Research preview · active development · source only.** This is not a playable public release. No release binaries, game files or Steam libraries are distributed here. The current harness starts a controlled Tokyo scenario; a complete campaign session is still a goal.

## What has been demonstrated

Current **scenario 10 / wire protocol 3** passed a **3,600-tick** English/Russian gate with keyboard/controller fixtures, jitter, mock Steam relay and native pause/resume. All consecutive selected-state samples matched; combat checks observed movement, damage and hostile defeat. Games, launchers and relays exited 0 without timeout or forced cleanup; each relay sent and received 461,008 bytes with zero discarded bytes. This remains a bounded same-machine test.

On **scenario version 5**, two separate English/Russian game processes completed a bounded **7,200-tick** automated gate with keyboard/controller input fixtures, injected timing jitter, a mock Steam relay and **three fixture-driven area swaps**. Selected state checks matched, all owned game/launcher/relay processes exited normally, and each relay sent and received 921,808 bytes with zero discarded bytes. This is historical evidence for that scenario, not a pass for the current source.

A separate **scenario-5** check reached **361 ticks** on the unlimited real-input code path, with input fixtures disabled and quiet diagnostics enabled. Closing only the owned host window delivered an ordered stop to the peer; both sides exited normally. This checks startup and shutdown wiring. It does not establish physical keyboard/controller usability.

Actual two-PC Steam delivery, ordinary campaign transitions and menus, save/resume, reconnect and complete world-state coverage remain unverified. A Steam lobby create/leave smoke check passed, but carried no game traffic. See the [evidence and limits](docs/STATUS.md) before interpreting any pass.

Current source has broader area, progress and mode checks. Own-code mode/progress/profile fixtures pass. Recent failed gates, corrections and the scenario-10 pause result are recorded in [Status](docs/STATUS.md#current-source-and-recent-failures).

The next multi-area regression (**run 033**) failed on both clients during native startup, before gameplay. Startup reliability is an open blocker; the earlier scenario-10 pause gate remains a scoped pass. See [Status](docs/STATUS.md#current-source-and-recent-failures).

## How far along?

**40 / 100 toward a first playable release**, based on a weighted engineering assessment. This is a judgement about milestones, not a measured coverage percentage or a delivery-date forecast. The missing campaign, save and remote-session work is substantial. The [score breakdown](docs/STATUS.md#progress-estimate) and [roadmap](docs/ROADMAP.md) explain what remains.

## Explore the source

| Component | Purpose |
| --- | --- |
| [`probe/`](probe/README.md) | Exact-build x86 hooks, guarded launch, local player routing and input lockstep |
| [`steam/native/`](steam/native/README.md) | Standalone x64 relay; explicit Steam API loading and a loopback game stream |
| [`steam/`](steam/README.md) | Python reference relay and own-code API/transport fixtures |
| [`launcher/`](launcher/README.md) | Windows host/join UI, verified content import and owned-process cleanup |
| [`compatibility/`](compatibility/README.md) | File-hash preflight for the known English and Russian packs |
| [`qa/`](qa/README.md) | Bounded paired-game gates and strict result scoring |

Start with [Building](docs/BUILDING.md), [Testing](docs/TESTING.md) and the [Architecture](docs/ARCHITECTURE.md). Build prerequisites are developer tools; the intended packaged launcher/relay do not require Python. Packaging is not a release-readiness claim.

## Compatibility

The only studied target is the Windows x86 `Game.exe`, version **1.0.0.111**, SHA-256:

```text
ebd30047d7e58c95bf7dc4bd1fb4a5780e16aa472b61c25a8c421632662083db
```

Other executables and unknown translations are unsupported. The engine's exact identity is not verified; this project does not claim generic engine compatibility. Use your own game copy. Import and testing operate on new copies and isolated session directories, preserving original installs and saves.

## Project context and license

Work began through [coop-anything claim #71](https://github.com/cranebeaker/coop-anything/issues/71). Development is continuing. Useful contributions include reproducible test cases, bounded sync oracles, campaign state mapping and portability fixes. Keep game assets, saves, raw runtime logs, account data and personal paths out of contributions.

Original project code and documentation use the [MIT license](LICENSE). That license does not cover the game, its assets, trademarks or third-party libraries. This is an independent fan research project, unaffiliated with the game's rights holders or Valve.
