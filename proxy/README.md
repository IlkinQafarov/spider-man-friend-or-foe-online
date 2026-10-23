# d3d9 proxy: how the probe gets into the game

The game imports `d3d9.dll`. Windows looks in the game's own folder first, so a `d3d9.dll`
placed next to `Game.exe` is loaded by the Windows loader itself, before any game code runs.
This proxy is that file. It does three things and nothing else:

1. Forwards every real Direct3D export to `d3d9_chain.dll`, a copy of this PC's own system
   `d3d9.dll` (the launcher makes the copy at start; it is never shipped).
2. Puts a 5-byte jump at the game's entry point, in its own process only. When the game
   reaches it, the proxy restores the original bytes and loads `native-probe.dll`.
3. If the launcher started the game and the probe cannot load, it stops the game, so the
   game never runs without the save and registry redirects.

Started any other way (no `COOP_PROBE_ROOT`), a failed probe load is ignored and the game
runs unmodified.

This replaces the older launcher that started the game suspended, wrote into its memory from
outside and ran a remote thread. That pattern is what antivirus tools flag as process injection.
Nothing here hides, packs or obfuscates anything.

Build: `probe\build.cmd` (runs `proxy\build.cmd`, copies `d3d9.dll` into `probe\`).
`make_def.py` writes `d3d9_forwards.h` from the export list of this PC's 32-bit `d3d9.dll`.
Self-test without the game: `proxy\build_selftest.cmd <new dir>` prints `PROXY_SELFTEST_PASS`.
