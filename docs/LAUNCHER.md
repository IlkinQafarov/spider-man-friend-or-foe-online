# Windows launcher and single-EXE bundle

The visible launcher uses WPF with the established C# session/import/profile backend. It has two large Host/Join panels, a selectable lobby code, Settings and Help cards, and eleven UI languages. Game language is independently English or Russian. Protocol remains 6.

## Current source revision (1 November 2026)

The illustrated layout uses a fixed 900x600 logical-unit window. It can be moved, minimized and closed; resizing and maximizing are disabled. A smaller monitor work area can reduce the window to keep it visible. A reversible standing-person accessibility toggle switches to readable Segoe UI fonts, horizontal labels and grey boards. Keyboard focus stays inside open cards, Escape closes them, and focus returns to the invoking control.

Hosting shows a large copyable lobby code and Stop action. Joining shows progress without inventing a lobby code. Long status messages wrap and scroll. The host receives a seven-second, translated, nonmodal notification when the relay validates the peer's session connection. It does not take focus or input, and does not claim that game catch-up has completed. Duplicate events are suppressed; reconnecting produces a new notice.

The joiner's in-game New Game and Load/Continue rows are hidden while waiting for the host. Native row indices and automatic host-driven startup are preserved; the host's menu is unchanged. The guest's Options and Quit rows retain the native renderer and indices.

Steam component discovery follows registered libraries across drives and checks conventional Steam/SteamLibrary locations on every ready local fixed/removable drive. It searches installed game folders, not every file on the disk. Every candidate still needs the required x64 exports and a valid Valve signature. Discovery reads files without loading Steam or using an account.

These changes are committed source updates; the existing v1.1.0-beta release asset predates this launcher. Building or publishing source does not replace a release download.

## Editable artwork

Three independent background layers are kept in `launcher/assets/backgrounds/`:

| File | Scene |
| --- | --- |
| `header.png` | New York skyline and flying carrier |
| `host.png` | Red sunset flight deck |
| `join.png` | Blue moonlit Tokyo rooftops |

Separate transparent foregrounds live in `launcher/assets/foreground/`: `title.png`, `host-icon.png` and `join-icon.png`. They are generated UI artwork, not extracted game assets. Text, buttons, panel frames and live status are WPF elements. Pinned third-party accessibility SVGs and their complete license/provenance live in `launcher/assets/icons/`.

Replace the source PNGs and rebuild to change the artwork permanently. Backgrounds use uniform-to-fill cropping. The bootstrapper restores edited runtime assets from its embedded copy, so changing extracted PNGs alone does not survive the next launch. Review the art hashes in `qa/prepare_publication.py` when intentionally changing source assets.

## Build

Use Windows .NET Framework/WPF and the existing native toolchain described in [BUILDING.md](BUILDING.md). No browser or downloaded UI runtime is required.

```powershell
cmd /c launcher\build.cmd
cmd /c launcher\build-player.cmd
python -I launcher/build_package.py --player --build <new-build-name>
python -I launcher/build_bundle.py --package dist/<new-build-name> --output dist/<new-exe-name>.exe
```

The single download extracts a sibling `FriendOrFoe` folder containing runtime files and player data. Keep the EXE in a writable folder. It bundles neither the game nor Valve's Steam DLL. Extraction/opening the launcher does not start a game or use a Steam account; Host/Join does.

The bootstrapper checks embedded paths and SHA-256 digests, rejects reparse paths, updates only allowlisted runtime/art files atomically, and holds a directory mutex while its launcher runs. Settings, profiles, imported game copies and sessions survive upgrades. Earlier ZIP-package data is not migrated automatically; back up add-on data and transfer it only while both launchers are closed. Do not copy or modify the original game's saves as part of this setup.

## Verification and limits

On 1 November 2026, research/player builds passed warnings-as-errors, all eleven locales and five UI states passed the WPF suite, and native window tests confirmed fixed sizing and maximize refusal. Host notification tests covered translation fit, deduplication, reconnect, cancellation, host-only signaling and actual nonactivation/click-through styles. Synthetic library fixtures covered configured and fallback libraries across simulated volumes, a missing volume and deduplication; a real read-only scan found compatible components on a non-system volume. No Steam account was used.

The final bundle passed framing, digest, allowlist, traversal, extraction/upgrade and settings/profile/import/session preservation checks; its packaged player passed the UI suite. Actual fixed-size English home and Russian hosting renders were inspected. Actual Windows DPI changes and mixed-monitor movement were not newly tested.

The native menu regression drove two English/Russian game instances for 601 matching frames after automatic host-selected New Game, with clean game/launcher exits. Guest rows 0/1 were filtered and the host had no hide markers. Options/Quit retain unchanged code paths; direct clicks on them after filtering were not separately exercised. Earlier failed attempts remain recorded in [STATUS.md](STATUS.md). This is not new two-PC Steam, physical-controller or full-campaign certification.
