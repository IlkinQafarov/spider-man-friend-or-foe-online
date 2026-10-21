# Private Windows launcher prototype

This is a private research build, not a release. It launches only the scripted Tokyo scenario. Campaign progression, menus, persistent saving, real-user input focus, and two-account remote gameplay have not been verified. Saves created by the native probe remain inside each new session sandbox; this launcher does not offer resume.

The x64 executable uses existing Windows .NET Framework and does not require Python or any installation. Build using `build.cmd`; it invokes the existing Framework64 C# compiler with C# 5-compatible source, warnings as errors, and `/target:winexe` so opening the app does not create a console window. CLI fixture modes retain explicitly redirected standard input/output/error handles; wait for process exit and inspect redirected output when running them. Keep the folder under the research root as `launcher/`. It expects the existing `compatibility/baseline.json`, `probe/native-launcher.exe`, `probe/native-probe.dll`, and `steam/native/native-relay.exe`. This directory does not contain game assets or a Steam DLL.

## File verification and import

Choose English or Russian independently on each computer. If that language's `test/english` or `test/russian` copy already exists, use Verify copy. Existing destinations are never replaced. If absent, Import installed folder reads a chosen source, verifies its complete known runtime manifest, copies only the expected 1291 runtime files into a new private staging folder, verifies the copied manifest again, and moves the result atomically into the new test destination. The source is never written. A failed import leaves its new staging folder for inspection.

The pinned manifest SHA-256 is `5c85b8c90c60b6d615a1aa50ae95d7b0acd32bbbe24bc68c282c136e5e7af42b`. It describes 1257 shared runtime files and 34 exact language-specific files. The shared digest is `c46b90afe36571cf3f4fbe3d4e7918da833297f78119d40504e96b4f531b36ea`. Coherent English or Russian packs are accepted; mixtures are rejected. Junctions/symlinks and reparse ancestors, unexpected DLLs, unexpected package files, changed configuration and runtime files, and duplicate case-insensitive paths are rejected. The same exact metadata allowlist and GameLog rule as `compatibility/preflight.py` are used. Metadata, uninstallers, personal saves, and GameLog are never copied.

## Host and join flow

Choose a locally installed `steam_api64.dll`. This prototype accepts only the already inspected x64 build with SHA-256 `670d654aa3255c5061cf0236a1cdbb2f5076cbd5ae44f613b6f4921558e83e2d`. Do not redistribute that file. Steam must be running and signed in. Clicking Host or Join is the app action that starts the Spacewar account transport. There is no automatic account activity when opening the launcher, verifying, importing, or running selftests.

Host creates a lobby and displays its numeric code. Copy it and exchange it manually with your friend. Join accepts that decimal code. No invitations or messages are sent by this launcher. It waits for the relay's validated `PEER_READY` event before starting the game, preventing the game's 30-second input handshake from expiring while waiting in a lobby. The relay connects to a loopback game stream on port 49375. Each player's local input can be a keyboard or controller; usability with real hardware has not yet been established.

Every session uses a unique new `sessions/<UTC-time>-<random>/` directory with preflight JSON, relay/launcher logs, lobby JSON and a sandbox. No account IDs are displayed in the UI; raw relay output remains private in its session log. Inherited `COOP_PROBE_*` variables are removed before transport and gameplay. The real-input environment explicitly selects frame limit zero, the known Tokyo area, fixed deterministic settings and the verified shared content digest.

Stop session posts WM_CLOSE only to windows belonging to the captured game process. Its process handle is retained and its parent PID and creation time checked against this session's native launcher. Cleanup falls back to terminating only this session's owned native launcher, whose existing job owns its game. The relay receives `STOP` on redirected stdin, allowing it to close the peer session, leave the lobby and shut down Steam. A bounded wait falls back to that owned relay handle. An outer kill-on-close Windows job contains both children for abnormal UI termination. Nothing enumerates or kills unrelated game or Steam processes.

## Opt-in automated two-PC connection check

Ordinary play remains the default. Enable **Automated connection test** on both PCs before clicking Host or Join. Both sides must choose the same mode; their scenario handshake rejects a mismatch. This mode uses native `HUMAN=1`, `HUMAN_FIXTURE=1`, `NET_FRAMES=3600`, `CLEAN_EXIT=1`, and `DIAGNOSTICS=1` with the same Tokyo area and critical content digest. It starts the bounded native launcher with 300 seconds instead of `--play`. The test supplies scripted controls and closes itself; there is no manual gameplay step. The same Host/Join account notice applies. Opening the UI, selecting the checkbox, and own-code selftests do not start Steam or use an account.

The UI reports PASS only after the retained owned game handle, native launcher, and relay all exit0 normally; logs show one matching `PROCESS_EXIT`, one expected native `NET READY role`, 3600 consecutive finite `NET state` rows numbered 0–3599 with slots0,1 plus progress/area hashes and both players moving, one `NET SCRIPT_COMPLETE frames=3600; peer completion confirmed; no co-op QA awarded`, one native clean WM_CLOSE request, and one normal nonempty relay `STREAM_COMPLETE` with zero discarded bytes. Native live state checks provide peer comparison; this UI does not add a complete game-state oracle. Exceptions, NET FAIL, native timeout/forced completion, cancellation, nonzero process exits, missing/repeated records, or forced/incomplete owned cleanup reject the test. The launcher permits 15 seconds for the already completed game stream to drain before cleanup; a relay requiring cancellation cannot pass. All conclusions and generic error reasons are saved in `sessions/<new-session>/connection-test.json` after cleanup. Raw logs remain private, and account identifiers are not shown in the result.

A pass is limited to a finite scripted Tokyo connection check. Own offline fixture passes do not prove remote delivery; actual two-PC Steam delivery remains unverified until this mode really runs between two PCs. Physical keyboard/controller usability, the full campaign, persistent saving, and reconnect remain outside its claim.

## Checks actually run

`fof-private-launcher.exe --selftest` passed own-code fixtures for path normalization/rejection, metadata and DLL allowlists, canonical digest format, Windows command argument quoting, inherited-environment removal, pinned baseline loading, coherent English and Russian fixtures, mixed languages, changed core files, real-tree unexpected DLLs, test-root confinement, refusal to replace existing output, and kill-on-close cleanup of a pure own-code child process. Fixtures remain in new `launcher/own-fixtures-*` directories. They contain only synthetic text written by this program.

`fof-private-launcher.exe --verify <research-root>/test/english` passed all 1291 runtime hashes, reporting English and the pinned critical digest. The corresponding Russian command passed all 1291 hashes, reporting Russian and its pinned locale digest. These modes read the supplied test folders without opening UI, game, Steam DLL, or account transport.

`fof-private-launcher.exe --offline-session-selftest` passed the same session methods used by Host: an own-code relay process announces a synthetic lobby, delays PEER_READY, and only then does the launcher start an own-code launcher child. The fixture launcher creates an invisible no-activate own WinForms child. The parent retains its handle, verifies its parent PID and creation time, posts WM_CLOSE, observes fixture game exit0, requests relay STOP, and closes all owned process handles and the job. The synthetic relay exits130 as the explicit cancellation contract specifies. Fixture logs stay under new `launcher/offline-sessions/` directories; their preflight file explicitly says `own-offline-fixture` and `status=fixture`. No actual Steam DLL, native game, or network transport is selected in this mode. This exercises process wiring and cleanup, not physical input or gameplay.

`fof-private-launcher.exe --connection-selftest` passed one synthetic positive evidence case, 28 negative cases, and three environment/argument checks. The failures cover role/FIN/close records, consecutive/finite state rows and player slots, progress hashes, exceptions, forced exits, matching process identity, relay completion/bytes/discard/failure, cancellation and cleanup. `--offline-connection-selftest` passed the same opt-in StartSession branch using only own-code child processes and synthetic logs: delayed PEER_READY, verified finite bot environment, retained owned child exit0, normal relay exit0, full evidence evaluation, saved PASS summary, and no remaining owned process/job handles. Both the ordinary own-code selftest and offline cancellation/session helper were rerun successfully after this change. No game or Steam library was loaded and no network/account was used by these fixtures.

No actual import, interactive usability session, real game launch, or real Steam account initialization was performed for launcher testing. The root worker must review and integrate it with the native relay, then exercise the complete flow using finite real-game QA before any readiness claim. Keep the native relay's own `steam_appid.txt` (480) beside its executable when packaging.

## Private asset-free packaging

`python launcher/build_package.py --selftest` checks synthetic packaging fixtures without launching any runtime, game, or account. Final packaging is deferred until the real-input startup check has been reviewed. Afterwards, the explicit command `python launcher/build_package.py --build <new-package-name>` creates a NEW `dist/<new-package-name>/` below the research root. Existing outputs are refused. Python is only needed by the builder, not the packaged application.

The builder copies exactly six allowlisted inputs: `launcher/fof-private-launcher.exe`, `probe/native-launcher.exe`, `probe/native-probe.dll`, `steam/native/native-relay.exe`, `steam/native/steam_appid.txt`, and `compatibility/baseline.json`. It generates portable `Start.cmd`, sanitized unfinished-prototype instructions, and `SHA256SUMS.json` containing every other shipped file's size and SHA-256. This hash inventory is not a signature. It validates the pinned baseline, Steam application ID 480, PE architecture/DLL identity, regular input files, existing path ancestors for reparse points, and embedded Windows user-profile build paths before copying. It verifies exact output membership and reads every output back.

No recursive source copy is used. Game assets/executables, Steam libraries, logs, saves, account IDs, analysis, mocks, selftests, and original install files are excluded. The relative layout is required: both Windows launchers derive the research/package root from their own parent directory. Import creates new `test/<language>/` copies inside that package root; sessions create new `sessions/` entries there. The supported Steam library remains an external user-selected file pinned to the hash above, so providing it on another PC is still an unresolved setup constraint. Creating a private package does not certify gameplay or authorize publishing it.


## Actual native play lifecycle

The separate play_lifecycle.py harness starts two isolated real games with
--play, HUMAN=1, HUMAN_FIXTURE=0 and unlimited network frames. It verifies
owned process identity/handles, matching selected state and ordinary WM_CLOSE
shutdown without Steam. Run023 matched306 consecutive idle samples and both
game/launcher exit codes0 with no forced cleanup. Run022 failed at frontend
startup; startup-only native menu actions now advance the initial boot menu.
An attempted intro-skip variant exposed a starting checksum mismatch in024;
this was later reproduced by a single uninitialized padding byte0xBF in the
progress sample. The oracle now excludes it. Intro skipping remains removed. Let the normal intro complete. Real hardware and two-account
transport remain unverified. --quiet checks120-tick diagnostic cadence and no
game-image captures; the live divergence check still runs each tick.
