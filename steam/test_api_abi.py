"""Exercise the actual ctypes adapter against an own-code x64 flat-API DLL."""
import ctypes
import hashlib
import os
from pathlib import Path
import unittest

from relay import frame, DATA, TAG
from steam_backend import SteamBackend

HERE = Path(__file__).absolute().parent
LOBBY = 480000123456

class BinaryABITest(unittest.TestCase):
    def test_host_join_and_message_ownership_through_binary_abi(self):
        path = HERE / 'api_abi_mock.dll'
        self.assertTrue(path.is_file(), 'Build the own-code ABI mock first')
        before = {key: os.environ.get(key) for key in ('SteamAppId', 'SteamGameId')}
        directory = Path.cwd()
        api = SteamBackend(path, hashlib.sha256(path.read_bytes()).hexdigest(), test_mock=True)
        metric = api.dll.FOF_MockMetric; metric.restype = ctypes.c_int; metric.argtypes = [ctypes.c_int]
        initial = [metric(i) for i in range(5)]
        for host in (True, False):
            try:
                api.start(host, 0 if host else LOBBY)
                events = api.events()
                self.assertEqual(len(events), 2)
                self.assertTrue(all(event == {'type': 'lobby', 'lobby': LOBBY, 'success': True} for event in events))
                self.assertEqual(api.pending_calls, {})
                self.assertEqual(api.me, 101)
                self.assertEqual(api.members(LOBBY), [101, 202]); self.assertEqual(api.owner(LOBBY), 101)
                api.set_tag(LOBBY, TAG); self.assertEqual(api.lobby_tag(LOBBY), TAG)
                self.assertTrue(api.accept(202))
                payload = frame(DATA, 7, bytes(range(256)) * 64)
                self.assertTrue(api.send(202, payload))
                self.assertEqual(api.messages(), [(202, payload)])
                self.assertEqual(api.messages(), [])
            finally: api.close(LOBBY, 202)
            # Interface getters are replaced by initialized pointers during start.
            api.user = api.interface('SteamUser_v023')
            api.matchmaking = api.interface('SteamMatchmaking_v009')
            api.network = api.interface('SteamNetworkingMessages_SteamAPI_v002')
        self.assertEqual([metric(i)-initial[i] for i in range(5)], [0, 2, 2, 2, 6])
        self.assertEqual(Path.cwd(), directory)
        self.assertEqual({key: os.environ.get(key) for key in before}, before)

if __name__ == '__main__': unittest.main(verbosity=2)
