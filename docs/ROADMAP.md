# Roadmap to a first playable release

Development is continuing. The current [40/100 estimate](STATUS.md#progress-estimate) describes milestones, not a schedule. Items below are acceptance goals, not claims that the feature already works.

## 1. Make the current proof reproducible

* Build the source on a clean developer setup, with documented compiler-path adjustments.
* Run the own-code fixtures, content preflights and bounded mixed-language game gates under the QA lease.
* Keep normal stop, malformed protocol, content mismatch, timeout and deliberate-divergence tests strict.
* Expand sampled state coverage and preserve a small, reproducible case for each desync.

Current scenario-11 source includes bounded keyboard-name lookup and canonical internal names as well as broader mode/area/progress checks. Fresh 7,200-tick/three-area and 3,600-tick pause/combat gates passed. Their same-machine fixture scope does not establish ordinary campaign travel, hardware compatibility or remote delivery. The earlier startup failure remains recorded; its exact trigger is unconfirmed.

Exit evidence: another developer can reproduce the bounded result using their own supported game copy, without exposing private game data or touching original saves.

## 2. Validate one real remote session

* Exercise explicit host/join between two authorized Steam accounts on separate PCs.
* Verify the stream carries game input, not just lobby events.
* Test controlled delay, jitter, loss/stalls and ordinary cancellation; record latency and failures honestly.
* Test physical keyboards and controllers, focus, binding changes and independent language choices.

Exit evidence: an automated finite two-PC scenario completes with matching selected state, complete stream delivery and normal process exits. This would still be a bounded connection milestone.

## 3. Replace scenario control with campaign behavior

* Map and synchronize frontend choices, party selection, mission state and normal area exits.
* Cover cutscenes, enemies, boss phases, upgrades, deaths, retries and return-to-menu flows.
* Detect or synchronize events outside the current fixed gameplay boundary.
* Add oracles for mission/enemy/combat state before widening completion claims.

Exit evidence: representative campaign routes progress through the game's own menus and exits, with automated checks for both peers and no forced fixture travel.

## 4. Design shared persistence and recovery

* Specify which peer owns progress and how conflicts are handled.
* Implement isolated shared save/resume and verify it across clean restarts.
* Decide whether reconnect is supported in the first release; reject unsupported recovery clearly.
* Keep original local saves and unrelated sessions outside the mod's writes.

Exit evidence: saved progress resumes into the same verified session state on both peers, and failure/recovery behavior is tested.

The current profile-storage foundation has 39 passing synthetic tests. It is not integrated into the application and earns no save/resume milestone credit yet.

## 5. Release gate

* Finish import/setup feedback and remove machine-specific build/setup assumptions.
* Validate packaged files on clean Windows PCs without developer Python tooling.
* Run a documented hardware/language/latency matrix and longer campaign sessions.
* Publish an own-code package, checksums, supported-build list, known issues and reproducible QA summary.

A source upload or a private package does not satisfy this gate. Release readiness requires the missing remote, campaign, persistence and usability evidence.

## Contributing useful evidence

Start with a narrow workstream and [Testing](TESTING.md). Report the exact supported executable hash, protocol/scenario version, test command, expected result and observed result. Share sanitized summaries or minimal own-code fixtures; do not attach game assets, personal saves, account identifiers or raw private captures. Public tracker coordination began at [coop-anything #71](https://github.com/cranebeaker/coop-anything/issues/71).
