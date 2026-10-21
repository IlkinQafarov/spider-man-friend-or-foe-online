# Native Windows relay (private prototype)

Own C++17 code. The x64 EXE uses the existing Windows APIs and a statically linked C++ runtime; friends do not need Python or a software installation. This folder includes no Steam DLL, SDK headers, game files or assets. Build with the already-installed Visual Studio compiler using `build.cmd`.

The adapter ports the flat API contract in `../steam_backend.py` and the FOF1 stream protocol in `../relay.py`. Real Steam two-account delivery has **not** been tested through this executable. Root repeated the already-authorized bounded create/leave lobby check through this compiled relay: lobby ready, smoke pass, close and exit0, no invitations or game traffic. This does not establish native message delivery or authorize account gameplay testing.

## CLI and launcher integration

```
native-relay.exe host --steam-live <absolute-existing-x64-API-DLL> --steam-api-sha256 <SHA256> --local-port 49375 --seconds 0 --ready-file <absolute-NEW-JSON>
native-relay.exe join --steam-live <absolute-existing-x64-API-DLL> --steam-api-sha256 <SHA256> --local-port 49375 --seconds 0 --ready-file <absolute-NEW-JSON> --lobby <unsigned-64-bit-ID>
```

`--seconds 0` has no test deadline; positive values are limited to 600 seconds. Stream completion, peer failure/timeout, Ctrl+C or the stdin command `STOP\n` still end the process. Local port defaults to 49375 and must be 1024..65535. The host relay connects to the local game on 127.0.0.1 after the remote stream OPEN; the join relay listens on 127.0.0.1 for its local game. It never binds a public interface or sends invitations or launches games.

`steam_appid.txt` containing 480 must be beside the relay EXE. Only explicit `--steam-live` changes the process environment (`SteamAppId`/`SteamGameId`) or working directory and calls API initialization. That path validates an explicit absolute DLL path, SHA-256, x64 PE/DLL identity and all required exports. Its read handle denies DLL modification/deletion across hash validation and load. It does not find or download a DLL or start Steam.

The ready JSON is created exclusively (an existing path is an error), and contains `{"lobby":<uint64>,"transport":"steam-spacewar-live"}`. Stdout is flushed JSON lines. Useful events:

* `LOBBY_READY`: `lobby`, `host`, `transport`.
* `PEER_READY`: `peer` (membership validated; joiner also validates lobby owner and tag).
* `STREAM_COMPLETE`: `sent`, `received`, `discarded_after_game_reset`, `transport`.
* `RELAY_FAIL`: `reason`, exit 1.
* `RELAY_CANCELLED`: peer session close, lobby leave and API shutdown have been requested; exit 130.

A launcher can redirect standard input and write `STOP` followed by LF for graceful cleanup. The command is polled without a blocked reader thread. `--stdin-selftest` waits up to five seconds for this command and never loads Steam. External forced process termination cannot run orderly cleanup; use it only as an owned-process fallback.

`host ... --smoke` is a bounded 15-second create/leave lobby check, without a game stream, invites or game traffic. It passed under the owner's existing authorization on2026-10-20; raw identifiers/logs remain private. Obtain account authorization before any different account action.

## Wire and lifetime contract

Each reliable ordered Steam channel-0 message has a 16-byte header: ASCII `FOF1` at bytes0..3; type at4 (`OPEN=1 DATA=2 CLOSE=3 PING=4 ACK=5`); version1 at5; zero reserved bytes6..7; little-endian unsigned byte offset at8..15. DATA has 1..16384 payload bytes; other types have no payload. DATA offsets must exactly equal cumulative bytes received. CLOSE carries the final byte offset and is acknowledged with ACK at that offset. TCP half-close propagates after pending writes drain. The close/ack handshake drains before normal exit, with a one-second completion grace period.

Only the selected other member of the two-member lobby is accepted. Join requires lobby key `fof_native=fof-native-prototype-v1`; unsolicited/nonmember session and message requests are rejected. Callback processing tracks only its own lobby API-call handle; unrelated async completions are ignored, and duplicate notifications for the same lobby are idempotent. Every dequeued callback is freed, and every returned message pointer is released even on validation failure. APIs/interfaces and structure layout match the own ABI fixture, without copying SDK implementation code.

Receive buffering is capped at1MiB. The outgoing queue is capped at64 messages/1MiB, reserving space for close/ack; local reads pause under send backpressure and pings are not accumulated behind it. A peer that stops sending traffic times out after20 seconds. Invalid lengths, reserved bits, types, stream ordering, premature/repeated OPEN/CLOSE, unsolicited/wrong ACK, byte-offset overflow and unexpected lobby identities fail closed. Reset locally ends the send stream and discards subsequently received data with an explicit counter; bytes already accepted by the OS before a reset is observed cannot be retracted or counted as undelivered.

This stream adapter does not implement game determinism, reconnect, migration, multi-peer rooms, anti-desync recovery or input mapping. Those remain the separate native game protocol/harness responsibilities.

## Automated own-code verification

```
native-relay.exe --abi-selftest
native-relay.exe --protocol-selftest
echo STOP | native-relay.exe --stdin-selftest
```

ABI selftest loads **only** the exact adjacent-project `../api_abi_mock.dll`, pinned to SHA-256 `5e84877b8ba6a8bd99bb613fe9a51042bf19106ac18b50873984a09509f5049d`, and checks the `FOF_MOCK_NO_ACCOUNT` marker. This path leaves Steam environment and working directory unchanged. It covers host and join, unrelated/duplicate callback handling, identities/membership/tag, a16,400-byte message in each operation, release counts (2 messages and6 callbacks), leave/shutdown counts (2 each), zero mock errors, wrong-pin rejection and no `steam_api64.dll` module loaded. A rebuilt mock with different bytes deliberately requires a reviewed pin update.

Protocol selftest uses two in-memory backends and real TCP sockets on OS-selected ephemeral loopback ports; it never loads any Steam DLL or uses an account. It transfers32,768 bytes in both directions with fragmented writes, verifies exact bytes, offsets and half-close/ack; then tests forced local reset/discard and orderly completion. Negative cases cover header/payload bounds, endianness, nonmembers/session rejection, wrong tag, repeated/premature open/close/ack, ordering,1MiB receive bound, duplicate/oversized membership, departure and peer timeout. It avoids49375/49376/49480 used by the root game tests. The command sandbox blocks loopback connects with WSA10013, so this exact own-code selftest needs the already-reviewed loopback-only escalation on this machine.

Build and selftests passed on2026-10-20. See `verification.json` for the final binary hash and observed results. These establish local contract/ABI behavior, not real Steam/WAN delivery.
