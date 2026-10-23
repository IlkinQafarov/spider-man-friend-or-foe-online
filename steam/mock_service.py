"""Own loopback-only lobby/messaging stand-in. Never loads Steam or uses accounts."""
import argparse
import base64
from collections import deque
import json
import select
import socket
import time

LOBBY = 480000123456
LIMIT = 1024 * 1024

def packet(value):
    return json.dumps(value, separators=(',', ':')).encode() + b'\n'

class Broker:
    def __init__(self, port):
        self.server = socket.socket()
        self.server.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        self.server.bind(('127.0.0.1', port)); self.server.listen(4); self.server.setblocking(False)
        self.clients = {}; self.users = {}; self.members = []; self.tag = ''; self.accepted = set(); self.held = []

    def emit(self, client, value):
        client['out'].extend(packet(value))
        if len(client['out']) > LIMIT: raise RuntimeError('Mock output buffer limit')

    def state(self):
        for user in self.members:
            self.emit(self.users[user], {'type': 'state', 'members': self.members, 'owner': 101, 'tag': self.tag})

    def deliver(self, sender, target, message):
        self.emit(self.users[target], {'type': 'message', 'user': sender, 'data': message})

    def command(self, client, value):
        command = value['cmd']; user = client.get('user', 0)
        if command == 'identify':
            user = int(value['user'])
            if user not in (101, 202) or user in self.users: raise RuntimeError('Invalid mock user')
            client['user'] = user; self.users[user] = client; return
        if not user: raise RuntimeError('Mock identification required')
        if command == 'create':
            if user != 101 or self.members: raise RuntimeError('Invalid mock create')
            self.members.append(user); self.state()
            event = {'type': 'lobby', 'lobby': LOBBY, 'success': True}
            self.emit(client, event); self.emit(client, event)  # Real Steam duplicate callback/call-result.
        elif command == 'tag':
            if user != 101: raise RuntimeError('Only host sets metadata')
            self.tag = value['tag']; self.state()
        elif command == 'join':
            if value['lobby'] != LOBBY or self.members != [101] or user != 202:
                self.emit(client, {'type': 'lobby', 'lobby': value['lobby'], 'success': False}); return
            self.members.append(user); self.state()
            self.emit(client, {'type': 'lobby', 'lobby': LOBBY, 'success': True})
        elif command == 'send':
            target = int(value['target'])
            if user not in self.members or target not in self.members or target == user:
                raise RuntimeError('Mock nonmember send rejected')
            raw = base64.b64decode(value['data'], validate=True)
            if not 16 <= len(raw) <= 16400: raise RuntimeError('Mock message size limit')
            # Steam sending implicitly accepts the reverse session.
            self.accepted.add((user, target)); self.flush_held()
            if (target, user) in self.accepted:
                self.deliver(user, target, value['data'])
            else:
                if sum(len(m[2]) for m in self.held) + len(value['data']) > LIMIT:
                    raise RuntimeError('Mock pending session buffer limit')
                if not any(a == user and b == target for a, b, _ in self.held):
                    self.emit(self.users[target], {'type': 'request', 'user': user})
                self.held.append((user, target, value['data']))
        elif command == 'accept':
            target = int(value['target'])
            if target not in self.members: raise RuntimeError('Mock accept nonmember')
            self.accepted.add((user, target)); self.flush_held()
        elif command == 'leave':
            self.leave(user)
        else: raise RuntimeError('Unknown mock command')

    def flush_held(self):
        pending, self.held = self.held, []
        for sender, target, message in pending:
            if (target, sender) in self.accepted:
                self.deliver(sender, target, message)
            else: self.held.append((sender, target, message))

    def leave(self, user):
        if user in self.members:
            self.members.remove(user)
            for remaining in self.members:
                self.emit(self.users[remaining], {'type': 'left', 'user': user})
            self.state()

    def run(self, seconds):
        print('MOCK_BROKER_READY (loopback only, no Steam account)', flush=True)
        deadline = time.monotonic() + seconds
        try:
            while time.monotonic() < deadline:
                readers = [self.server, *self.clients]
                writers = [sock for sock, client in self.clients.items() if client['out']]
                readable, writable, _ = select.select(readers, writers, [], 0.05)
                for sock in readable:
                    if sock == self.server:
                        conn, _ = sock.accept(); conn.setblocking(False)
                        if len(self.clients) >= 4: conn.close(); continue
                        self.clients[conn] = {'in': bytearray(), 'out': bytearray()}; continue
                    client = self.clients[sock]
                    try: chunk = sock.recv(65536)
                    except (ConnectionResetError, ConnectionAbortedError): chunk = b''
                    if not chunk:
                        self.leave(client.get('user', 0)); self.users.pop(client.get('user', 0), None)
                        sock.close(); del self.clients[sock]; continue
                    client['in'].extend(chunk)
                    if len(client['in']) > LIMIT: raise RuntimeError('Mock input buffer limit')
                    while b'\n' in client['in']:
                        line, _, rest = client['in'].partition(b'\n'); client['in'] = rest
                        self.command(client, json.loads(line))
                for sock in writable:
                    if sock not in self.clients: continue
                    buffer = self.clients[sock]['out']; count = sock.send(buffer); del buffer[:count]
        finally:
            for sock in self.clients: sock.close()
            self.server.close()

class MockBackend:
    kind = 'mock-steam-contract-loopback'
    def __init__(self, port, host):
        if not 1024 <= port <= 65535: raise ValueError('Invalid mock broker port')
        self.socket = socket.create_connection(('127.0.0.1', port), timeout=3)
        self.buffer = bytearray(); self.callbacks = deque(); self.incoming = deque()
        self.me = 101 if host else 202
        self._members = []; self._owner = 0; self._tag = ''
        self.write(cmd='identify', user=self.me)

    def write(self, **value): self.socket.sendall(packet(value))
    def start(self, host, lobby): self.write(cmd='create') if host else self.write(cmd='join', lobby=lobby)
    def set_tag(self, lobby, tag): self._tag = tag; self.write(cmd='tag', tag=tag)
    def lobby_tag(self, lobby): return self._tag
    def members(self, lobby): return list(self._members)
    def owner(self, lobby): return self._owner
    def accept(self, user): self.write(cmd='accept', target=user)
    def send(self, user, message):
        self.write(cmd='send', target=user, data=base64.b64encode(message).decode()); return True

    def pump(self):
        while select.select([self.socket], [], [], 0)[0]:
            chunk = self.socket.recv(65536)
            if not chunk: raise RuntimeError('Mock broker disconnected')
            self.buffer.extend(chunk)
            if len(self.buffer) > LIMIT: raise RuntimeError('Mock receive buffer limit')
            while b'\n' in self.buffer:
                line, _, rest = self.buffer.partition(b'\n'); self.buffer = rest
                event = json.loads(line)
                if event['type'] == 'state':
                    self._members, self._owner, self._tag = event['members'], event['owner'], event['tag']
                elif event['type'] == 'message':
                    self.incoming.append((event['user'], base64.b64decode(event['data'], validate=True)))
                else: self.callbacks.append(event)

    def events(self):
        self.pump(); events = list(self.callbacks); self.callbacks.clear(); return events
    def messages(self):
        self.pump(); messages = list(self.incoming); self.incoming.clear(); return messages
    def close(self, lobby, peer):
        try: self.write(cmd='leave')
        except OSError: pass
        self.socket.close()

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=49480); parser.add_argument('--seconds', type=int, default=300)
    args = parser.parse_args()
    if not 1024 <= args.port <= 65535 or not 1 <= args.seconds <= 1200: parser.error('Invalid port/duration')
    Broker(args.port).run(args.seconds)
