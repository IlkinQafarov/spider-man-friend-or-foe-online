"""x64 Steam flat-API adapter. Imported mock tests do not instantiate this class."""
import ctypes as C
import hashlib
import os
from pathlib import Path
import re
import struct

class Callback(C.Structure):
    _fields_ = [('user', C.c_int), ('callback', C.c_int), ('param', C.c_void_p), ('size', C.c_int)]
class Completed(C.Structure):
    _fields_ = [('call', C.c_uint64), ('callback', C.c_int), ('size', C.c_uint32)]
class Created(C.Structure):
    _fields_ = [('result', C.c_int), ('lobby', C.c_uint64)]
class Entered(C.Structure):
    _fields_ = [('lobby', C.c_uint64), ('permissions', C.c_uint32), ('locked', C.c_bool), ('response', C.c_uint32)]
class Chat(C.Structure):
    _fields_ = [('lobby', C.c_uint64), ('changed', C.c_uint64), ('actor', C.c_uint64), ('flags', C.c_uint32)]

class SteamBackend:
    kind = 'steam-spacewar-live'
    def __init__(self, path, expected_hash, *, test_mock=False):
        if os.name != 'nt' or C.sizeof(C.c_void_p) != 8:
            raise RuntimeError('This adapter requires existing x64 Windows Python')
        path = Path(path).absolute()
        if test_mock and path != Path(__file__).absolute().with_name('api_abi_mock.dll'):
            raise ValueError('ABI fixture must use the project-built api_abi_mock.dll')
        if not expected_hash or not re.fullmatch('[0-9a-fA-F]{64}', expected_hash):
            raise ValueError('A verified existing Steam API DLL SHA-256 is required')
        data = path.read_bytes()
        if hashlib.sha256(data).hexdigest() != expected_hash.lower():
            raise ValueError('Steam API DLL SHA-256 mismatch')
        pe = struct.unpack_from('<I', data, 0x3c)[0]
        if data[:2] != b'MZ' or data[pe:pe+4] != b'PE\0\0' or struct.unpack_from('<H', data, pe+4)[0] != 0x8664:
            raise ValueError('Expected a Windows x64 Steam API DLL')
        if not test_mock:
            appid = Path(__file__).absolute().with_name('steam_appid.txt')
            if appid.exists():
                if appid.read_text().strip() != '480': raise ValueError('Existing AppID file is not Spacewar')
            else: appid.write_text('480\n', encoding='ascii')
            os.environ['SteamAppId'] = os.environ['SteamGameId'] = '480'
            os.chdir(Path(__file__).absolute().parent)
        # Loading and API initialization are reachable only through explicit --steam-live.
        self.dll = C.CDLL(str(path), winmode=0x1100)
        if test_mock:
            marker = self.dll.FOF_MockIdentity; marker.restype = C.c_char_p; marker.argtypes = []
            if marker() != b'FOF_MOCK_NO_ACCOUNT': raise ValueError('Invalid own-code ABI fixture')
            self.kind = 'mock-steam-flat-api-abi (no Steam account)'
        self.functions = {}; self.initialized = False
        self.pending_calls = {}; self.owned_lobby = 0
        V, I, U, Q, B, S = C.c_void_p, C.c_int, C.c_uint32, C.c_uint64, C.c_bool, C.c_char_p
        signatures = {
            'InitFlat': (I, [V]), 'Shutdown': (None, []), 'GetHSteamPipe': (I, []),
            'ManualDispatch_Init': (None, []), 'ManualDispatch_RunFrame': (None, [I]),
            'ManualDispatch_GetNextCallback': (B, [I, C.POINTER(Callback)]),
            'ManualDispatch_FreeLastCallback': (None, [I]),
            'ManualDispatch_GetAPICallResult': (B, [I, Q, V, I, I, C.POINTER(B)]),
            'ISteamUser_GetSteamID': (Q, [V]), 'ISteamUser_BLoggedOn': (B, [V]),
            'ISteamMatchmaking_CreateLobby': (Q, [V, I, I]), 'ISteamMatchmaking_JoinLobby': (Q, [V, Q]),
            'ISteamMatchmaking_LeaveLobby': (None, [V, Q]),
            'ISteamMatchmaking_GetLobbyOwner': (Q, [V, Q]),
            'ISteamMatchmaking_GetNumLobbyMembers': (I, [V, Q]),
            'ISteamMatchmaking_GetLobbyMemberByIndex': (Q, [V, Q, I]),
            'ISteamMatchmaking_SetLobbyData': (B, [V, Q, S, S]),
            'ISteamMatchmaking_GetLobbyData': (S, [V, Q, S]),
            'SteamNetworkingIdentity_SetSteamID64': (None, [V, Q]),
            'SteamNetworkingIdentity_GetSteamID64': (Q, [V]),
            'ISteamNetworkingMessages_SendMessageToUser': (I, [V, V, V, U, I, I]),
            'ISteamNetworkingMessages_ReceiveMessagesOnChannel': (I, [V, I, C.POINTER(V), I]),
            'ISteamNetworkingMessages_AcceptSessionWithUser': (B, [V, V]),
            'ISteamNetworkingMessages_CloseSessionWithUser': (B, [V, V]),
            'SteamNetworkingMessage_t_Release': (None, [V]),
        }
        for name, (result, arguments) in signatures.items():
            function = getattr(self.dll, 'SteamAPI_' + name)
            function.restype, function.argtypes = result, arguments
            self.functions[name] = function
        self.user = self.interface('SteamUser_v023')
        self.matchmaking = self.interface('SteamMatchmaking_v009')
        self.network = self.interface('SteamNetworkingMessages_SteamAPI_v002')
        # Interface getters are called after InitFlat in start(), not here.

    def interface(self, name):
        function = getattr(self.dll, 'SteamAPI_' + name)
        function.restype, function.argtypes = C.c_void_p, []
        return function
    def call(self, name, *arguments): return self.functions[name](*arguments)
    def identity(self, user):
        identity = C.create_string_buffer(136)
        self.call('SteamNetworkingIdentity_SetSteamID64', identity, user)
        return identity

    def start(self, host, lobby):
        error = C.create_string_buffer(1024)
        result = self.call('InitFlat', error)
        if result != 0: raise RuntimeError(f'Steam initialization failed ({result}): {error.value.decode(errors="replace")}')
        self.initialized = True
        self.user, self.matchmaking, self.network = self.user(), self.matchmaking(), self.network()
        if not all((self.user, self.matchmaking, self.network)) or not self.call('ISteamUser_BLoggedOn', self.user):
            raise RuntimeError('Steam interfaces unavailable or account not logged in')
        self.me = self.call('ISteamUser_GetSteamID', self.user)
        self.pipe = self.call('GetHSteamPipe'); self.call('ManualDispatch_Init')
        operation = 'ISteamMatchmaking_CreateLobby' if host else 'ISteamMatchmaking_JoinLobby'
        handle = self.call(operation, self.matchmaking, *([1, 2] if host else [lobby]))
        if not handle: raise RuntimeError('Steam rejected the lobby operation')
        self.pending_calls[handle] = (513, C.sizeof(Created)) if host else (504, C.sizeof(Entered))

    def set_tag(self, lobby, tag):
        if not self.call('ISteamMatchmaking_SetLobbyData', self.matchmaking, lobby, b'fof_native', tag.encode()):
            raise RuntimeError('Could not set Friend or Foe lobby metadata')
    def lobby_tag(self, lobby):
        value = self.call('ISteamMatchmaking_GetLobbyData', self.matchmaking, lobby, b'fof_native')
        return value.decode(errors='replace') if value else ''
    def owner(self, lobby): return self.call('ISteamMatchmaking_GetLobbyOwner', self.matchmaking, lobby)
    def members(self, lobby):
        count = self.call('ISteamMatchmaking_GetNumLobbyMembers', self.matchmaking, lobby)
        if not 0 <= count <= 2: raise RuntimeError('Invalid Steam lobby member count')
        return [self.call('ISteamMatchmaking_GetLobbyMemberByIndex', self.matchmaking, lobby, i) for i in range(count)]
    def accept(self, user): return self.call('ISteamNetworkingMessages_AcceptSessionWithUser', self.network, self.identity(user))
    def send(self, user, message):
        raw = C.create_string_buffer(message)
        result = self.call('ISteamNetworkingMessages_SendMessageToUser', self.network, self.identity(user), raw, len(message), 9, 0)
        if result == 1: return True
        if result in (10, 25): return False
        raise RuntimeError(f'Steam reliable send failed ({result})')

    def event(self, callback, data):
        if callback == 513 and len(data) >= C.sizeof(Created):
            value = Created.from_buffer_copy(data)
            if value.result == 1: self.owned_lobby = value.lobby
            return {'type': 'lobby', 'lobby': value.lobby, 'success': value.result == 1}
        if callback == 504 and len(data) >= C.sizeof(Entered):
            value = Entered.from_buffer_copy(data)
            if value.response == 1: self.owned_lobby = value.lobby
            return {'type': 'lobby', 'lobby': value.lobby, 'success': value.response == 1}
        if callback == 1251 and len(data) >= 136:
            raw = C.create_string_buffer(data)
            return {'type': 'request', 'user': self.call('SteamNetworkingIdentity_GetSteamID64', raw)}
        if callback == 1252 and len(data) >= 136:
            raw = C.create_string_buffer(data)
            return {'type': 'failed', 'user': self.call('SteamNetworkingIdentity_GetSteamID64', raw)}
        if callback == 506 and len(data) >= C.sizeof(Chat):
            value = Chat.from_buffer_copy(data)
            if value.flags & 0x1e: return {'type': 'left', 'user': value.changed}
        return None

    def events(self):
        self.call('ManualDispatch_RunFrame', self.pipe)
        events = []; message = Callback()
        while self.call('ManualDispatch_GetNextCallback', self.pipe, C.byref(message)):
            try:
                if not 0 <= message.size <= 4096: raise RuntimeError('Steam callback size rejected')
                data = C.string_at(message.param, message.size)
                callback = message.callback
                if callback == 703:
                    if len(data) < C.sizeof(Completed): raise RuntimeError('Invalid Steam call completion')
                    done = Completed.from_buffer_copy(data)
                    expected = self.pending_calls.get(done.call)
                    # Initialization can queue other SDK operations. Only consume results
                    # for lobby calls issued by this helper, as with a typed CCallResult.
                    if expected is None: continue
                    if (done.callback, done.size) != expected:
                        raise RuntimeError(f'Steam lobby callback layout rejected (type={done.callback}, size={done.size}, expected={expected})')
                    buffer = C.create_string_buffer(done.size); failed = C.c_bool()
                    retrieved = self.call('ManualDispatch_GetAPICallResult', self.pipe, done.call, buffer, done.size, done.callback, C.byref(failed))
                    if not retrieved or failed.value:
                        raise RuntimeError(f'Steam lobby result failed (type={done.callback}, size={done.size}, retrieved={bool(retrieved)}, failed={failed.value})')
                    del self.pending_calls[done.call]
                    callback, data = done.callback, buffer.raw
                event = self.event(callback, data)
                if event: events.append(event)
            finally: self.call('ManualDispatch_FreeLastCallback', self.pipe)
        return events

    def messages(self):
        pointers = (C.c_void_p * 32)()
        count = self.call('ISteamNetworkingMessages_ReceiveMessagesOnChannel', self.network, 0, pointers, len(pointers))
        if not 0 <= count <= len(pointers): raise RuntimeError('Steam message count rejected')
        messages = []
        for i in range(count):
            pointer = pointers[i]
            try:
                if not pointer: raise RuntimeError('Null Steam message')
                size = C.c_int.from_address(pointer+8).value
                data = C.c_void_p.from_address(pointer).value
                if not data or not 16 <= size <= 16400: raise RuntimeError('Steam message size rejected')
                sender = self.call('SteamNetworkingIdentity_GetSteamID64', pointer+16)
                messages.append((sender, C.string_at(data, size)))
            finally:
                if pointer: self.call('SteamNetworkingMessage_t_Release', pointer)
        return messages

    def close(self, lobby, peer):
        if not self.initialized: return
        if peer and self.network: self.call('ISteamNetworkingMessages_CloseSessionWithUser', self.network, self.identity(peer))
        lobby = lobby or self.owned_lobby
        if lobby and self.matchmaking: self.call('ISteamMatchmaking_LeaveLobby', self.matchmaking, lobby)
        self.call('Shutdown'); self.initialized = False
