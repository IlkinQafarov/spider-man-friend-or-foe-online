"""Own-code one-stream relay; real Steam requires explicit --steam-live."""
import argparse
from collections import deque
import json
from pathlib import Path
import socket
import struct
import time

TAG = 'fof-native-prototype-v1'
HEADER = struct.Struct('<4sBBHQ')
OPEN, DATA, CLOSE, PING, ACK = range(1, 6)
MAX_CHUNK, MAX_BUFFER = 16384, 1024 * 1024

class Failed(RuntimeError):
    pass

def frame(kind, offset=0, data=b''):
    return HEADER.pack(b'FOF1', kind, 1, 0, offset) + data

def decode(message):
    if len(message) < HEADER.size or len(message) > HEADER.size + MAX_CHUNK:
        raise Failed('Invalid relay message length')
    magic, kind, version, reserved, offset = HEADER.unpack_from(message)
    body = message[HEADER.size:]
    if magic != b'FOF1' or version != 1 or reserved or kind not in (OPEN, DATA, CLOSE, PING, ACK):
        raise Failed('Invalid relay header')
    if (kind != DATA and body) or (kind == DATA and not body):
        raise Failed('Invalid relay payload')
    return kind, offset, body

class StreamRelay:
    def __init__(self, backend, host, port, seconds, ready_file=None):
        self.api, self.host, self.port = backend, host, port
        self.deadline = time.monotonic() + seconds if seconds else None
        self.ready_file = ready_file
        self.lobby = self.peer = 0
        self.listener = self.local = None
        self.open_requested = False
        self.outgoing = deque()
        self.pending = bytearray()
        self.tx = self.rx = 0
        self.eof = self.remote_closed = self.close_ack = self.write_closed = False
        self.local_reset = False
        self.discarded_after_reset = 0
        self.peer_seen = self.last_ping = time.monotonic()
        self.done_since = None

    def log(self, event, **fields):
        print(json.dumps({'event': event, **fields}), flush=True)

    def lobby_ready(self, lobby):
        if self.lobby == lobby:
            self.log('DUPLICATE_LOBBY_CALLBACK_IGNORED'); return
        if self.lobby:
            raise Failed('Unexpected second lobby')
        if not self.host and self.api.lobby_tag(lobby) != TAG:
            raise Failed('That lobby is not this Friend or Foe co-op prototype')
        self.lobby = lobby
        if self.host:
            self.api.set_tag(lobby, TAG)
        else:
            self.listener = socket.socket()
            self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            self.listener.bind(('127.0.0.1', self.port)); self.listener.listen(1)
            self.listener.setblocking(False)
        self.log('LOBBY_READY', lobby=lobby, host=self.host, transport=self.api.kind)
        if self.ready_file:
            with Path(self.ready_file).open('x', encoding='utf-8') as out:
                json.dump({'lobby': lobby, 'transport': self.api.kind}, out)

    def callbacks(self):
        for event in self.api.events():
            kind = event['type']
            if kind == 'lobby':
                if event.get('success', True): self.lobby_ready(event['lobby'])
                else: raise Failed('Steam lobby creation/join failed')
            elif kind == 'request':
                user = event['user']
                members = self.api.members(self.lobby) if self.lobby else []
                if user in members and user != self.api.me and (not self.peer or user == self.peer):
                    self.api.accept(user); self.log('SESSION_ACCEPTED', peer=user)
                else:
                    self.log('NONMEMBER_SESSION_REJECTED', peer=user)
            elif kind == 'failed' and event.get('user') == self.peer:
                raise Failed('Steam peer session failed')
            elif kind == 'left' and event['user'] == self.peer:
                if not (self.eof and self.remote_closed and self.close_ack):
                    raise Failed('The other player left the Steam lobby')

    def find_peer(self):
        if not self.lobby or self.peer: return
        members = self.api.members(self.lobby)
        if len(members) > 2: raise Failed('Expected a two-player lobby')
        candidates = [user for user in members if user != self.api.me]
        if candidates:
            self.peer = candidates[0] if self.host else self.api.owner(self.lobby)
            if self.peer not in candidates: raise Failed('Lobby owner is not the other player')
            self.peer_seen = time.monotonic()
            self.log('PEER_READY', peer=self.peer)

    def receive(self):
        for user, message in self.api.messages():
            if not self.peer or user != self.peer or user not in self.api.members(self.lobby):
                self.log('NONMEMBER_MESSAGE_REJECTED', peer=user); continue
            kind, offset, data = decode(message)
            self.peer_seen = time.monotonic()
            if kind == PING: continue
            if kind == OPEN:
                if not self.host or self.open_requested or offset:
                    raise Failed('Invalid or repeated stream open')
                self.open_requested = True
            elif kind == DATA:
                if not (self.open_requested or (not self.host and self.local)) or self.remote_closed or offset != self.rx:
                    raise Failed('Out-of-order relay stream data')
                if len(self.pending) + len(data) > MAX_BUFFER: raise Failed('Relay receive buffer limit')
                if self.local_reset: self.discarded_after_reset += len(data)
                else: self.pending.extend(data)
                self.rx += len(data)
            elif kind == CLOSE:
                if self.remote_closed or offset != self.rx: raise Failed('Invalid stream close')
                self.remote_closed = True; self.outgoing.append(frame(ACK, self.rx))
            elif kind == ACK:
                if not self.eof or offset != self.tx: raise Failed('Invalid stream close acknowledgement')
                self.close_ack = True

    def local_end(self, reset=False):
        if reset:
            self.local_reset = True
            self.discarded_after_reset += len(self.pending); self.pending.clear()
        if not self.eof:
            self.eof = True; self.outgoing.append(frame(CLOSE, self.tx))
            self.log('GAME_STREAM_RESET' if reset else 'GAME_STREAM_EOF')

    def local_io(self):
        if self.host and self.open_requested and not self.local:
            try:
                self.local = socket.create_connection(('127.0.0.1', self.port), timeout=0.05)
                self.local.setblocking(False); self.log('GAME_STREAM_CONNECTED')
            except (ConnectionError, TimeoutError, OSError):
                self.local = None
        elif not self.host and self.listener and not self.local:
            try:
                self.local, _ = self.listener.accept(); self.local.setblocking(False)
                self.listener.close(); self.listener = None
                self.outgoing.append(frame(OPEN)); self.log('GAME_STREAM_ACCEPTED')
            except BlockingIOError: pass
        if not self.local: return
        if self.local_reset: return
        if self.pending:
            try:
                sent = self.local.send(self.pending)
                if not sent: raise Failed('Game write returned zero')
                del self.pending[:sent]
            except BlockingIOError: pass
            except (ConnectionResetError, BrokenPipeError, ConnectionAbortedError): self.local_end(reset=True)
        if self.remote_closed and not self.pending and not self.write_closed:
            try: self.local.shutdown(socket.SHUT_WR)
            except OSError: self.local_end(reset=True)
            self.write_closed = True
        if not self.eof and len(self.outgoing) < 64:
            try:
                data = self.local.recv(MAX_CHUNK)
                if data:
                    self.outgoing.append(frame(DATA, self.tx, data)); self.tx += len(data)
                else:
                    self.local_end()
            except BlockingIOError: pass
            except (ConnectionResetError, BrokenPipeError, ConnectionAbortedError): self.local_end(reset=True)

    def run(self, lobby=0):
        try:
            self.api.start(self.host, lobby)
            while self.deadline is None or time.monotonic() < self.deadline:
                self.callbacks(); self.find_peer(); self.receive(); self.local_io()
                now = time.monotonic()
                if self.peer:
                    if now - self.last_ping >= 1:
                        self.outgoing.append(frame(PING)); self.last_ping = now
                    if now - self.peer_seen > 20: raise Failed('Peer keep-alive timed out')
                    while self.outgoing:
                        if not self.api.send(self.peer, self.outgoing[0]): break
                        self.outgoing.popleft()
                if self.eof and self.remote_closed and self.close_ack and not self.pending and not self.outgoing:
                    if self.done_since is None: self.done_since = now
                    if now - self.done_since >= 1:
                        self.log('STREAM_COMPLETE', sent=self.tx, received=self.rx, discarded_after_game_reset=self.discarded_after_reset, transport=self.api.kind)
                        return 0
                time.sleep(0.002)
            raise Failed('Relay test deadline expired')
        finally:
            if self.listener: self.listener.close()
            if self.local: self.local.close()
            self.api.close(self.lobby, self.peer)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('role', choices=('host', 'join'))
    parser.add_argument('--lobby', type=int, default=0)
    parser.add_argument('--local-port', type=int, default=49375)
    parser.add_argument('--seconds', type=int, default=240)
    parser.add_argument('--ready-file', type=Path)
    backend = parser.add_mutually_exclusive_group(required=True)
    backend.add_argument('--mock', type=int, metavar='BROKER_PORT')
    backend.add_argument('--steam-live', type=Path, metavar='EXISTING_API_DLL')
    parser.add_argument('--steam-api-sha256')
    parser.add_argument('--smoke', action='store_true', help='Create and leave one lobby; no game or invites')
    args = parser.parse_args()
    if args.ready_file: args.ready_file = args.ready_file.absolute()
    if not 1024 <= args.local_port <= 65535 or not 0 <= args.seconds <= 1200:
        parser.error('Invalid local port or duration')
    if args.role == 'join' and not args.lobby: parser.error('Join requires a lobby id')
    if not 0 <= args.lobby < 2**64: parser.error('Lobby id must be an unsigned 64-bit number')
    if args.smoke and args.role != 'host': parser.error('Smoke check creates a host lobby only')
    try:
        if args.mock:
            from mock_service import MockBackend
            api = MockBackend(args.mock, args.role == 'host')
        else:
            from steam_backend import SteamBackend
            api = SteamBackend(args.steam_live, args.steam_api_sha256)
        relay = StreamRelay(api, args.role == 'host', args.local_port, args.seconds, args.ready_file)
        if args.smoke:
            try:
                api.start(True, 0); deadline=time.monotonic()+15
                while time.monotonic()<deadline:
                    relay.callbacks()
                    if relay.lobby:
                        relay.log('LOBBY_SMOKE_PASS', transport=api.kind, game_traffic=False, invites_sent=False)
                        return 0
                    time.sleep(0.01)
                raise Failed('Lobby smoke check timed out')
            finally:
                api.close(relay.lobby, relay.peer)
                relay.log('LOBBY_SMOKE_CLOSED', lobby_was_ready=bool(relay.lobby))
        return relay.run(args.lobby)
    except (Failed, OSError, ValueError, RuntimeError) as exc:
        print(json.dumps({'event': 'RELAY_FAIL', 'reason': str(exc)}), flush=True)
        return 1

if __name__ == '__main__':
    raise SystemExit(main())
