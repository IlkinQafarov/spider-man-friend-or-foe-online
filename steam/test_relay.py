import contextlib
import ctypes
import io
from pathlib import Path
import socket
import tempfile
import threading
import time
import unittest

from mock_service import Broker, MockBackend, LOBBY
from relay import StreamRelay, Failed, frame, decode, OPEN, DATA, CLOSE, PING
from steam_backend import SteamBackend, Callback, Completed, Created, Entered

HERE = Path(__file__).absolute().parent

def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0)); return sock.getsockname()[1]

class RelayTests(unittest.TestCase):
    def test_protocol_rejects_malformed_and_out_of_order(self):
        for message in (b'', b'garbage', frame(DATA), frame(PING, data=b'bad'), frame(99)):
            with self.assertRaises(Failed): decode(message)
        self.assertEqual(decode(frame(DATA, 3, b'abc')), (DATA, 3, b'abc'))
        class API:
            me = 101
            def messages(self): return [(202, frame(DATA, 1, b'x'))]
            def members(self, lobby): return [101, 202]
        relay = StreamRelay(API(), True, 49000, 3); relay.lobby = LOBBY; relay.peer = 202; relay.open_requested = True
        with self.assertRaisesRegex(Failed, 'Out-of-order'): relay.receive()

    def test_nonmember_request_and_message_rejected(self):
        class API:
            me = 101
            accepted = []
            def events(self): return [{'type': 'request', 'user': 999}]
            def messages(self): return [(999, frame(OPEN))]
            def members(self, lobby): return [101, 202]
            def accept(self, user): self.accepted.append(user)
        api = API(); relay = StreamRelay(api, True, 49000, 3); relay.lobby = LOBBY; relay.peer = 202
        with contextlib.redirect_stdout(io.StringIO()): relay.callbacks(); relay.receive()
        self.assertFalse(api.accepted); self.assertFalse(relay.open_requested)

    def test_wrong_lobby_tag_rejected(self):
        class API:
            def lobby_tag(self, lobby): return 'another-game'
        with self.assertRaisesRegex(Failed, 'not this Friend'):
            StreamRelay(API(), False, 49000, 3).lobby_ready(LOBBY)

    def test_local_game_reset_propagates_close(self):
        class ResetSocket:
            def recv(self, count): raise ConnectionResetError('Fixture: game exited')
        relay = StreamRelay(None, True, 49000, 3)
        relay.local = ResetSocket(); relay.open_requested = True; relay.tx = 128
        with contextlib.redirect_stdout(io.StringIO()): relay.local_io()
        self.assertTrue(relay.local_reset and relay.eof)
        self.assertEqual(decode(relay.outgoing.popleft()), (CLOSE, 128, b''))
        relay.pending.extend(b'late bytes')
        with contextlib.redirect_stdout(io.StringIO()): relay.local_end(reset=True)
        self.assertEqual(relay.pending, b''); self.assertEqual(relay.discarded_after_reset, 10)

    def test_x64_callback_layout(self):
        self.assertEqual(ctypes.sizeof(ctypes.c_void_p), 8)
        self.assertEqual((ctypes.sizeof(Callback), Callback.param.offset), (24, 8))
        self.assertEqual((ctypes.sizeof(Completed), ctypes.sizeof(Created)), (16, 16))
        self.assertEqual((ctypes.sizeof(Entered), Entered.response.offset), (24, 16))

    def test_dispatch_consumes_only_owned_lobby_result(self):
        api = SteamBackend.__new__(SteamBackend)
        api.pipe = 1; api.pending_calls = {77: (513, ctypes.sizeof(Created))}; api.owned_lobby = 0
        completions = [Completed(66, 999, 8), Completed(77, 513, ctypes.sizeof(Created))]
        buffers = [ctypes.create_string_buffer(bytes(value)) for value in completions]
        fetched = []; freed = []; cursor = iter(buffers)
        def call(name, *args):
            if name == 'ManualDispatch_RunFrame': return
            if name == 'ManualDispatch_GetNextCallback':
                buffer = next(cursor, None)
                if buffer is None: return False
                message = ctypes.cast(args[1], ctypes.POINTER(Callback)).contents
                message.callback = 703; message.size = ctypes.sizeof(Completed)
                message.param = ctypes.addressof(buffer)
                return True
            if name == 'ManualDispatch_GetAPICallResult':
                fetched.append(args[1]); result = Created(1, LOBBY)
                ctypes.memmove(args[2], ctypes.byref(result), ctypes.sizeof(result))
                return True
            if name == 'ManualDispatch_FreeLastCallback': freed.append(True); return
            raise AssertionError(name)
        api.call = call
        self.assertEqual(api.events(), [{'type': 'lobby', 'lobby': LOBBY, 'success': True}])
        self.assertEqual(fetched, [77]); self.assertEqual(len(freed), 2)
        self.assertEqual(api.pending_calls, {}); self.assertEqual(api.owned_lobby, LOBBY)

    def test_mock_requires_acceptance(self):
        broker = Broker(free_port())
        events = []
        broker.server.close()
        broker.members = [101, 202]
        broker.users = {101: {'user': 101, 'out': bytearray()}, 202: {'user': 202, 'out': bytearray()}}
        import base64
        broker.command(broker.users[101], {'cmd': 'send', 'target': 202, 'data': base64.b64encode(frame(PING)).decode()})
        self.assertEqual(len(broker.held), 1)
        self.assertIn(b'request', broker.users[202]['out'])
        self.assertNotIn(b'message', broker.users[202]['out'])
        broker.command(broker.users[202], {'cmd': 'accept', 'target': 101})
        self.assertEqual(broker.held, [])
        self.assertIn(b'message', broker.users[202]['out'])

    def test_binary_stream_and_close_propagation(self):
        ports = []
        while len(ports) < 3:
            candidate = free_port()
            if candidate not in ports: ports.append(candidate)
        broker_port, game_port, client_port = ports
        broker = Broker(broker_port)
        failures = []; results = []
        def owned(function):
            try: function()
            except Exception as exc: failures.append(repr(exc))
        thread = threading.Thread(target=lambda: owned(lambda: broker.run(6)), daemon=True); thread.start()
        server = socket.socket(); server.bind(('127.0.0.1', game_port)); server.listen(1); server.settimeout(4)
        payload = bytes(range(256)) * 128
        def game_host():
            connection, _ = server.accept(); connection.settimeout(4)
            with connection:
                received = bytearray()
                while chunk := connection.recv(149): received.extend(chunk)
                if bytes(received) != payload: raise Failed('Binary stream content differed')
                connection.sendall(payload[::-1]); connection.shutdown(socket.SHUT_WR)
        game = threading.Thread(target=lambda: owned(game_host)); game.start()
        with tempfile.TemporaryDirectory(prefix='relay-test-', dir=HERE) as folder:
            assert Path(folder).resolve().is_relative_to(HERE.resolve())
            ready = Path(folder) / 'host-ready.json'
            host_api = MockBackend(broker_port, True)
            host = StreamRelay(host_api, True, game_port, 5, ready)
            host_thread = threading.Thread(target=lambda: owned(lambda: results.append(host.run())))
            host_thread.start()
            limit = time.monotonic() + 3
            while not ready.exists() and time.monotonic() < limit: time.sleep(0.01)
            self.assertTrue(ready.exists())
            peer = StreamRelay(MockBackend(broker_port, False), False, client_port, 5)
            peer_thread = threading.Thread(target=lambda: owned(lambda: results.append(peer.run(LOBBY))))
            peer_thread.start()
            limit = time.monotonic() + 3
            connection = None
            while connection is None and time.monotonic() < limit:
                try: connection = socket.create_connection(('127.0.0.1', client_port), timeout=0.1)
                except OSError: time.sleep(0.01)
            self.assertIsNotNone(connection)
            with connection:
                connection.settimeout(4)
                for offset in range(0, len(payload), 73): connection.sendall(payload[offset:offset+73])
                connection.shutdown(socket.SHUT_WR)
                received = bytearray()
                while chunk := connection.recv(211): received.extend(chunk)
            self.assertEqual(bytes(received), payload[::-1])
            host_thread.join(5); peer_thread.join(5)
            self.assertFalse(host_thread.is_alive() or peer_thread.is_alive())
        game.join(4); server.close(); thread.join(7)
        self.assertEqual(failures, []); self.assertEqual(sorted(results), [0, 0])

if __name__ == '__main__': unittest.main(verbosity=2)
