![Two independent clients connected through a shared input stream](docs/banner.svg)

# Spider-Man: Friend or Foe — co-op research

Two friends, two Windows game instances, and an independent English or Russian language choice on each PC. This project connects the game's native two-player controls through a synchronized input stream.

**Research preview · active development · source only.** This is not a playable public release. No release binaries, game files or Steam libraries are distributed here. The launcher's play mode uses the native campaign route; the bounded Tokyo scenario remains for automated connection checks.

## Current state (2026-10-23)

The campaign route now runs end to end in automated tests: New Game (or Continue) on the host, the Helicarrier hub, the mission computer, the first Tokyo mission and the return to the hub, twice in a row.

- **Join at the hub:** the host plays alone and clicks Host whenever they like; the friend clicks Join, their game catches up with the host's progress by itself and appears in the Helicarrier.
- **Player 2 in the hub:** the friend plays the campaign's chosen sidekick (Prowler at the start) next to Spider-Man; the friend can pick another unlocked sidekick at the sidekick computer.
- **Cutscenes:** each player may skip or watch; the one ahead pauses with a "Waiting for your partner..." box in the launcher's language until the other catches up.
- **Launcher:** red/blue themed UI in 11 languages, a big Play Game button, plain Steam error messages, and import that links the game files instead of copying them (no second 3 GB copy on the same drive).

All of this was checked with bots on one PC with a mock Steam relay. Two PCs over real Steam, real controllers, later missions and save/continue across sessions remain untested; closed testing with real players is starting.

## What has been demonstrated

Current **scenario 11 / wire protocol 3** passed two English/Russian gates: **7,200 ticks with three fixture-driven area transitions**, and **3,600 ticks with native pause/resume**. Both used keyboard/controller fixtures, jitter and a mock Steam relay. All consecutive selected-state samples matched; games, launchers and relays exited 0 without timeout or forced cleanup. Each relay sent/received 921,808 bytes in the transition gate and 461,008 in the pause gate, with zero discarded bytes. These remain bounded same-machine tests.

On **scenario version 5**, two separate English/Russian game processes completed a bounded **7,200-tick** automated gate with keyboard/controller input fixtures, injected timing jitter, a mock Steam relay and **three fixture-driven area swaps**. Selected state checks matched, all owned game/launcher/relay processes exited normally, and each relay sent and received 921,808 bytes with zero discarded bytes. This is historical evidence for that scenario, not a pass for the current source.

A separate **scenario-5** check reached **361 ticks** on the unlimited real-input code path, with input fixtures disabled and quiet diagnostics enabled. Closing only the owned host window delivered an ordered stop to the peer; both sides exited normally. This checks startup and shutdown wiring. It does not establish physical keyboard/controller usability.

Actual two-PC Steam delivery, ordinary campaign transitions and menus, save/resume, reconnect and complete world-state coverage remain unverified. A Steam lobby create/leave smoke check passed, but carried no game traffic. See the [evidence and limits](docs/STATUS.md) before interpreting any pass.

Current source has broader area, progress and mode checks, bounded native keyboard-name lookup and canonical internal key names. Own-code key-name/mode/progress/profile fixtures pass. Recent failed gates, corrections and their limits are recorded in [Status](docs/STATUS.md#current-source-and-recent-failures).

Earlier **run 033** failed on both clients during native startup, before gameplay. The guarded scenario-11 runs passed; the original trigger remains unconfirmed. Current internal naming requires an already-loaded standard US keyboard layout, preserves native display labels and changes no Windows settings. Other layouts and physical input usability need further validation. See [Status](docs/STATUS.md#current-source-and-recent-failures).

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
