# Steam relay prototype

Private own code, no game assets, no downloaded runtime. A user-authorized real Steam lobby smoke check passed on 2026-10-20. It created one friends-only two-member Spacewar lobby, left it and shut down, with no invitations or game traffic. Actual two-account message delivery is still unverified. The game remains an x86 process; this separate adapter uses existing x64 Python and an existing x64 Steam API library.

`relay.py` wraps one loopback TCP stream in reliable ordered Steam messages. Host creates a two-member friends-only lobby. A joiner supplies the lobby number and verifies this prototype's metadata before accepting game traffic. Only that lobby's other member is accepted. Streams have ordered byte offsets, bounded buffers, close/ack messages and keep-alives while the game loads. The local native-game input protocol still performs build/scenario and sampled-state checks.

Sources checked 2026-10-20:

- [Valve networking overview](https://partner.steamgames.com/doc/features/multiplayer/networking)
- [Valve ISteamNetworkingMessages](https://partner.steamgames.com/doc/api/ISteamNetworkingMessages)
- [Valve Spacewar example](https://partner.steamgames.com/doc/sdk/api/example)
- Project playbook `playbook/networking-steam-spacewar.md`.

Own mock tests use `mock_service.py`, bound only to127.0.0.1. They never load Steam or use an account. The mock models lobby metadata/membership, duplicate creation results, session-request acceptance, reliable ordered delivery and leave events. Eight tests passed, including32,768 bytes in each direction with TCP half-close propagation, malformed/out-of-order rejection, wrong-lobby rejection, nonmember rejection, reset-on-game-exit handling and ignoring unrelated asynchronous results. This broker is a contract stand-in, not an actual Valve relay test.

Separately, `api_abi_mock.cpp` builds an own-code x64 DLL exposing just the flat API functions this adapter uses. `test_api_abi.py` loads that exact project DLL with `test_mock=True`; this path leaves Steam environment variables/current directory unchanged and never loads Valve's DLL. The binary ABI fixture passed host/join callbacks (including an unrelated completion), lobby membership/metadata, 16,400-byte message payload/identity decoding, release of every received message, leave and shutdown. It checks the chosen ctypes ABI against this fixture, not against a second live Steam client. Build with existing compiler: `build-abi-mock.cmd`.

```text
python -m unittest -v test_relay.py
python -m unittest -v test_api_abi.py
python ../qa/run_pair.py --name UNIQUE-NAME --frames 3600 --seconds 240 --keyboard-host --steam-mock
```

The latter runs fresh content checks, two native game clients and two own-code relays. Run016 passed all3,600 paired samples with normal game/launcher/relay exits; both relay streams sent and received461,008 bytes with zero discarded bytes. Run017 also passed an ordinary native area swap with keyboard-host/controller-peer fixtures. Results are written to the private QA result folder; completion is not assumed before scoring.

The live path requires explicit `--steam-live <existing steam_api64.dll>` and its verified `--steam-api-sha256`. It sets AppID480 only inside the helper process and creates a project-local `steam_appid.txt`. It does not install software, download anything, send invitations, edit Steam settings or request achievements. Initialization and lobby creation do use the signed-in Steam account and may show Spacewar as playing. Obtain the owner's concrete permission underAGENTS.md before invoking that path.

Smoke check: `relay.py host --steam-live <existing DLL> --steam-api-sha256 <verified SHA> --smoke`. It initializes Steam, creates one friends-only two-member lobby, records its ID, leaves and shuts down. Deadline15 seconds; it launches no game and sends no invite. The first authorized attempt failed because the adapter tried consuming unrelated async completions. Tracking only its owned lobby call fixed the observed failure; the second attempt passed and exited0. A successful smoke check does not establish two-account data delivery or working co-op through Steam. Account authorization was for this bounded smoke check, not invitations or gameplay sessions.

Human host/join packaging, friend discovery/invites, actual two-account transport, campaign progression and complete shared-world synchronization remain unfinished. Do not present this research helper as a playable release.
