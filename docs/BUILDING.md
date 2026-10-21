# Building the research source

This is a developer build for one pinned Windows executable, not a supported install guide for a playable release. No compiler, Steam library or game is downloaded by the build scripts.

## Prerequisites

* 64-bit Windows.
* Installed MSVC C++17 tools and a Windows SDK, including x86 and x64 targets.
* Installed .NET Framework C# compiler and Windows Forms assemblies for the x64 launcher. The source uses C# 5-compatible syntax.
* Python 3.10 or newer for developer preflight, fixtures and packaging; standard library only.

The checked-in C++ scripts currently name the Visual Studio 18 Community `VC/Auxiliary/Build/vcvars32.bat` or `vcvars64.bat` installation. The UI script names the Framework64 `v4.0.30319/csc.exe` location. Inspect the scripts and adjust those toolchain locations to your installed tools when necessary. Automatic tool installation and portable compiler discovery are not implemented.

## Build from the repository root

In PowerShell:

```powershell
.\probe\build.cmd
.\probe\build-wire.cmd
.\probe\build_wire_selftest.cmd
.\steam\build-abi-mock.cmd
.\steam\native\build.cmd
.\launcher\build.cmd
```

Check every command's exit code before continuing. The probe build produces the x86 `native-launcher.exe`, `native-probe.dll`, own launch fixture, progress-sample fixture and mode-sample fixture. The separate wire scripts build transport and ordered-stop fixtures. The relay and API mock are x64. The GUI launcher is x64 .NET Framework. C++ outputs use a statically linked runtime; these scripts do not link or embed game assets.

Keep the relative source layout: the launchers derive the repository/package root from their own parent directory. Keep `steam/native/steam_appid.txt` beside the native relay. It contains Spacewar's application ID, 480. Building and running own-code fixtures do not initialize Steam.

The native ABI selftest pins the exact own-code mock DLL hash. A fresh mock build can change its bytes through compiler settings or PE timestamps. Review `steam/api_abi_mock.cpp` and your local build output before updating `MockSha256` in `steam/native/relay.hpp`, then rebuild the native relay. Never replace the own-code fixture with a real Steam DLL or update a pin merely to silence an unexplained mismatch.

## Game content is separate

The supported x86 `Game.exe` is version 1.0.0.111 with SHA-256:

```text
ebd30047d7e58c95bf7dc4bd1fb4a5780e16aa472b61c25a8c421632662083db
```

The engine's exact identity is unverified. Addresses and hooks are specific to this executable, and another build must be investigated independently.

Use your own game files. The launcher can import a verified complete English or Russian pack into a **new** `test/english/` or `test/russian/` destination; it refuses to replace an existing destination or write the source folder. The content manifest contains hashes and relative names, not game bytes. Reparse points, unknown runtime files and incoherent language mixtures are rejected. Verify the copy before any game gate; see [Testing](TESTING.md).

Opening the GUI or verifying files does not start account transport. Clicking Host/Join or explicitly invoking `--steam-live` does. The native relay requires an externally supplied, supported x64 Steam API DLL and its exact hash; this repository does not supply or redistribute that library. Live gameplay delivery remains unverified.

## Packaging remains a developer operation

```powershell
python -I launcher/build_package.py --selftest
```

The builder has an explicit `--build <new-package-name>` mode. It copies only allowlisted own runtime outputs, the content manifest and instructions; existing output names are refused. Do not turn a successful package build into a playable-release claim. No release binaries are published with this research preview.

Continue with [Testing](TESTING.md) before attempting a game run.
