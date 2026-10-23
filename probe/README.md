# Native research probe (own code only)

This own-code component provides guarded game launch, observed file/registry redirection, normalized two-player input exchange and fixed-step research scenarios. Historical scenario-5 English/Russian game processes completed bounded local gates. Current scenario 10 / wire protocol 3 passed a 3,600-tick pause/combat gate with normal exits; broader gameplay remains unverified. It is not a campaign-ready online release or an OS security sandbox. See [current status](../docs/STATUS.md) and [testing](../docs/TESTING.md) for the evidence and its limits.

## Build

Use the existing installed Visual Studio 18 Community x86 compiler:

```powershell
cmd /c .\build.cmd
```

The build uses `/W4 /O2 /MT /EHsc`, produces a 32-bit launcher, probe DLL and own selftest executable, and installs nothing. The game and intake files are not linked into these outputs.

## Review first, then test the own-code fixture

```powershell
.\native-launcher.exe --selftest <new-absolute-scratch-directory> 15
```

The fixture checks redirected Documents, a sandbox file, virtual registry create/set/query/close, and a deliberately denied canary path under `analysis/native/`. The launcher creates a separate new control file in the same directory first to prove that directory is writable without the hooks. Required fixture IAT hooks must be present before the fixture is resumed. If the denial hook regresses it can only create this named scratch canary. Successful expected output: `OWN_SELFTEST_PASS` and `PROCESS_EXIT ... code=0`, with corresponding `<sandbox>/probe.log` events. Any timeout is not a selftest pass.

## Guarded game invocation after review

```powershell
.\native-launcher.exe <isolated-test-copy>\Game.exe <new-absolute-sandbox-directory> 15
```

The launcher only accepts game SHA-256 `ebd30047d7e58c95bf7dc4bd1fb4a5780e16aa472b61c25a8c421632662083db`, x86 PE image base `0x400000`, entrypoint `0x52FD2C`, no EXE TLS directory. It starts suspended, replaces the first two entry bytes with a temporary `EB FE` gate, resumes only until the main thread is at that gate, then suspends the main thread. It resolves `LoadLibraryW` in the remote process by module plus export offset, injects this local DLL, and resumes original entry bytes only after successful DLL attach. DLL attach requires every observed game import hook to be found; failures leave the game stopped and cause termination.

Game targets must be under the local workspace `test/` directory. A new sandbox must be inside this local game workspace, outside `incoming/`. The launcher rejects reparse points in both paths.

The original game entrypoint has not run when hooks install. Imported DLL initialization has already occurred under Windows loader control; that earlier dependency behavior is outside this IAT probe's coverage.

## Implemented containment and observation

- A new per-run directory is mandatory; it is never reused or erased.
- The game SHGetFolderPathW import redirects Documents, Roaming AppData, Local AppData and Common AppData into this directory. System folder lookup is allowed; unknown folder IDs fail closed.
- All six observed game registry imports are replaced. Registry handles and values live only in this process. No original game registry API is invoked. Missing values use the game's own defaults; the confirmed ResolutionX/ResolutionY settings return 640/480.
- Imported game CreateFileA/W, WriteFile, fopen/_wfopen, _wmkdir and _wremove reject writes outside the sandbox or isolated test copy. Reparse-point traversal is rejected by path-component checks. This is a path allowlist for observed entrypoints, not complete kernel-level mediation.
- ShellExecuteW is blocked. Cursor confinement/hiding are suppressed. CreateWindowExA and ShowWindow use a small window; fixtures request no activation and actual human mode uses normal activation. Standard Direct3D9 CreateDevice/Reset parameters force 640x480 windowed mode. These are API interventions, not guessed game binary offsets or unverified registry flags.
- Direct3D Present, timeGetTime and dynamically resolved XInputGetState log first and periodic calls. Observation-only mode passes results through. Network research mode fixes task timing, routes local normalized input, supplies a reserved remote input device, and checks selected world state. A 4-device XInput poll is not evidence of support for four simultaneous game characters.
- The launcher assigns its process to a kill-on-close job before the original entrypoint runs. The job permits one process; ShellExecute is also blocked. The launcher uses retained process/job handles and a bounded 1–300 second game timeout (selftest maximum 60, default 15). The explicit --play path permits unlimited lifetime only with real-human input flags and frame limit zero. It never finds or kills unrelated processes by name.

## Limits and required interpretation

Containment is implemented for the game executable's statically observed IAT, plus blocking dynamically requested registry APIs. It does not globally hook every imported dependency, kernel call, COM component, dynamically obtained file API, or native system call. An unchanged real-save/config snapshot remains a required observation after a run. An unexpected blocked operation, new write path, crash, no READY log, or timeout in the fixture is a failed prerequisite to investigate.

The game may auto-save into its isolated sandbox. No save/registry data is imported from the owner profile. Settings do not persist between probe runs. Language remains the native default for the chosen test folder unless the game changes its virtual setting. The probe now implements two-peer input lockstep, native player position access and selected state checks for bounded research scenarios; separate relay and launcher components provide the account-transport path. Same-machine English/Russian gates have passed, but complete world determinism, campaign behavior, save/resume and actual two-PC Steam game traffic remain unverified. See [current public status](../docs/STATUS.md).


## Network research contract

InputWire protocol 3 carries sequenced 60 Hz action frames with a three-frame delay.
Finite gates exchange FIN; ordinary user exit sends a distinct ordered STOP
from the guarded owned main-window procedure. Unexpected disconnects retain
errors. The dedicated build_wire_selftest.cmd test covers ordered/local/remote
stop, immediate close and simultaneous close stress, bad markers and FIN.

Scenario version 10 includes the full area path, selected progress/campaign flags
and canonical mode state. Previous mode is active only in pause/NIS modes 1/5;
pause owner is active only while paused. It omits inactive history and internal
padding, including byte 0xBF after the upgrade array, local settings and date/time.
Progress and mode fixtures test these boundaries, including mutation twins.
Fixture-only pause assertions end at tick 430; continuous selected mode, world
and progress checks remain enabled every tick.
This remains an incomplete world/mission oracle. Actual unlimited human mode
defaults to quiet diagnostics: no game-image capture and one state log per 120
ticks; live divergence checks still run every tick. COOP_PROBE_DIAGNOSTICS=1
enables full private diagnostics for finite QA.
