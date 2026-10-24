#include "relay.hpp"
#include <bcrypt.h>
#include <cstddef>
#include <cstring>
#include <map>

namespace fof {
namespace {
class Handle {
    HANDLE value_=INVALID_HANDLE_VALUE;
public:
    explicit Handle(HANDLE value):value_(value) {}
    ~Handle() { if(value_!=INVALID_HANDLE_VALUE) CloseHandle(value_); }
    HANDLE get() const { return value_; }
};
struct Identity { int type=0,size=0; unsigned char data[128]{}; };
struct Callback { int user,callback; void* param; int size; };
struct Completed { Id call; int callback; unsigned size; };
struct Created { int result; Id lobby; };
struct Entered { Id lobby; unsigned permissions; bool locked; unsigned response; };
struct Chat { Id lobby,changed,actor; unsigned flags; };
struct MessagePrefix { void* data; int size,connection; Identity sender; };
static_assert(sizeof(Identity)==136 && sizeof(Callback)==24 && sizeof(Completed)==16);
static_assert(sizeof(Created)==16 && sizeof(Entered)==24 && offsetof(Entered,response)==16);
static_assert(sizeof(Chat)==32 && offsetof(MessagePrefix,sender)==16);

// Steam's manual dispatch hands over every callback for the pipe, including ones this relay never reads.
// First two-PC test (2026-10-24): the joiner ended with "Steam callback size rejected" right after PEER_READY.
// Callbacks are now only rejected when the relay would actually read them; odd ones are skipped and logged.
bool usableCallback(const Callback& callback) {
    if(callback.size<0 || (callback.size && !callback.param)) return false;
    if(callback.callback==703) return callback.size>=static_cast<int>(sizeof(Completed));
    return true;
}
std::string hashHandle(HANDLE file) {
    if(file==INVALID_HANDLE_VALUE) throw Failure("Could not open pinned DLL read-only");
    BCRYPT_ALG_HANDLE algorithm=nullptr; BCRYPT_HASH_HANDLE hash=nullptr;
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) throw Failure("SHA-256 provider unavailable");
    struct Cleanup { BCRYPT_ALG_HANDLE& a; BCRYPT_HASH_HANDLE& h; ~Cleanup(){if(h) BCryptDestroyHash(h); if(a) BCryptCloseAlgorithmProvider(a,0);} } cleanup{algorithm,hash};
    if(BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)<0) throw Failure("Could not initialize SHA-256");
    std::array<unsigned char,65536> buffer{}; DWORD read=0;
    LARGE_INTEGER zero{};
    if(!SetFilePointerEx(file,zero,nullptr,FILE_BEGIN)) throw Failure("Could not seek pinned DLL");
    for(;;) {
        if(!ReadFile(file,buffer.data(),static_cast<DWORD>(buffer.size()),&read,nullptr)) throw Failure("Could not read pinned DLL");
        if(!read) break;
        if(BCryptHashData(hash,buffer.data(),read,0)<0) throw Failure("Could not hash pinned DLL");
    }
    std::array<unsigned char,32> digest{};
    if(BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)<0) throw Failure("Could not finish SHA-256");
    const char hex[]="0123456789abcdef"; std::string result;
    for(unsigned char c:digest) { result+=hex[c>>4]; result+=hex[c&15]; }
    return result;
}
void requireX64(HANDLE file) {
    IMAGE_DOS_HEADER dos{}; DWORD read=0; LARGE_INTEGER position{};
    if(!SetFilePointerEx(file,position,nullptr,FILE_BEGIN) || !ReadFile(file,&dos,sizeof(dos),&read,nullptr) || read!=sizeof(dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<static_cast<LONG>(sizeof(dos))) throw Failure("Invalid DLL DOS header");
    LARGE_INTEGER length{};
    if(!GetFileSizeEx(file,&length) || dos.e_lfanew>length.QuadPart-static_cast<LONGLONG>(sizeof(IMAGE_NT_HEADERS64))) throw Failure("Invalid DLL PE header bounds");
    position.QuadPart=dos.e_lfanew;
    IMAGE_NT_HEADERS64 nt{};
    if(!SetFilePointerEx(file,position,nullptr,FILE_BEGIN) || !ReadFile(file,&nt,sizeof(nt),&read,nullptr) || read!=sizeof(nt) || nt.Signature!=IMAGE_NT_SIGNATURE || nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 || !(nt.FileHeader.Characteristics&IMAGE_FILE_DLL) || nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC) throw Failure("Expected a Windows x64 DLL");
}
void configureLiveAppId() {
    const auto directory=executableDirectory();
    Handle file(CreateFileW((directory+L"\\steam_appid.txt").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(file.get()==INVALID_HANDLE_VALUE) throw Failure("Missing steam_appid.txt beside native relay executable");
    char buffer[32]{}; DWORD read=0;
    if(!ReadFile(file.get(),buffer,sizeof(buffer),&read,nullptr) || read==sizeof(buffer)) throw Failure("Invalid Spacewar AppID file");
    std::string text(buffer,read);
    const auto first=text.find_first_not_of(" \t\r\n"), last=text.find_last_not_of(" \t\r\n");
    if(first==std::string::npos || text.substr(first,last-first+1)!="480") throw Failure("Existing relay AppID file is not Spacewar");
    if(!SetEnvironmentVariableW(L"SteamAppId",L"480") || !SetEnvironmentVariableW(L"SteamGameId",L"480") || !SetCurrentDirectoryW(directory.c_str())) throw Failure("Could not configure Spacewar environment");
}
template<class T> T exportFunction(HMODULE module,const char* name) {
    const FARPROC address=GetProcAddress(module,name);
    if(!address) throw Failure(std::string("Missing flat Steam API export: ")+name);
    T result=nullptr; static_assert(sizeof(result)==sizeof(address));
    std::memcpy(&result,&address,sizeof(result)); return result;
}
class Steam final:public Backend {
    std::wstring path_;
    Handle pinnedFile_;
    HMODULE dll_=nullptr;
    bool ownMock_=false,initialized_=false;
    int pipe_=0;
    void* user_=nullptr; void* matchmaking_=nullptr; void* network_=nullptr;
    Id ownedLobby_=0,requestedLobby_=0;
    std::map<Id,std::pair<int,unsigned>> pending_;
    unsigned skipped_=0;
#define FLAT(name,result,...) using name##Type=result(__cdecl*)(__VA_ARGS__); name##Type name=nullptr
    FLAT(InitFlat,int,char*); FLAT(Init,bool); FLAT(Shutdown,void); FLAT(GetHSteamPipe,int);
    FLAT(ManualDispatch_Init,void); FLAT(ManualDispatch_RunFrame,void,int);
    FLAT(ManualDispatch_GetNextCallback,bool,int,Callback*);
    FLAT(ManualDispatch_FreeLastCallback,void,int);
    FLAT(ManualDispatch_GetAPICallResult,bool,int,Id,void*,int,int,bool*);
    FLAT(SteamUser_v023,void*); FLAT(SteamMatchmaking_v009,void*); FLAT(SteamNetworkingMessages_SteamAPI_v002,void*);
    FLAT(ISteamUser_GetSteamID,Id,void*); FLAT(ISteamUser_BLoggedOn,bool,void*);
    FLAT(ISteamMatchmaking_CreateLobby,Id,void*,int,int); FLAT(ISteamMatchmaking_JoinLobby,Id,void*,Id);
    FLAT(ISteamMatchmaking_LeaveLobby,void,void*,Id); FLAT(ISteamMatchmaking_GetLobbyOwner,Id,void*,Id);
    FLAT(ISteamMatchmaking_GetNumLobbyMembers,int,void*,Id); FLAT(ISteamMatchmaking_GetLobbyMemberByIndex,Id,void*,Id,int);
    FLAT(ISteamMatchmaking_SetLobbyData,bool,void*,Id,const char*,const char*);
    FLAT(ISteamMatchmaking_GetLobbyData,const char*,void*,Id,const char*);
    FLAT(SteamNetworkingIdentity_SetSteamID64,void,Identity*,Id); FLAT(SteamNetworkingIdentity_GetSteamID64,Id,const Identity*);
    FLAT(ISteamNetworkingMessages_SendMessageToUser,int,void*,const Identity*,const void*,unsigned,int,int);
    FLAT(ISteamNetworkingMessages_ReceiveMessagesOnChannel,int,void*,int,void**,int);
    FLAT(ISteamNetworkingMessages_AcceptSessionWithUser,bool,void*,const Identity*);
    FLAT(ISteamNetworkingMessages_CloseSessionWithUser,bool,void*,const Identity*);
    FLAT(SteamNetworkingMessage_t_Release,void,void*);
#undef FLAT
    Identity identity(Id user) const { Identity value{}; SteamNetworkingIdentity_SetSteamID64(&value,user); return value; }
    void addEvent(std::vector<Event>& output,int callback,const unsigned char* data,std::size_t size) {
        if(callback==513 && size>=sizeof(Created)) {
            Created value{}; std::memcpy(&value,data,sizeof(value));
            if(value.result==1) ownedLobby_=value.lobby;
            output.push_back({Event::Type::Lobby,0,value.lobby,value.result==1});
        } else if(callback==504 && size>=sizeof(Entered)) {
            Entered value{}; std::memcpy(&value,data,sizeof(value));
            // Ignore unsolicited lobby notifications, preserve duplicate callbacks for our lobby.
            if(value.lobby!=ownedLobby_ && value.lobby!=requestedLobby_) return;
            if(value.response==1) ownedLobby_=value.lobby;
            output.push_back({Event::Type::Lobby,0,value.lobby,value.response==1});
        } else if((callback==1251 || callback==1252) && size>=sizeof(Identity)) {
            Identity value{}; std::memcpy(&value,data,sizeof(value));
            output.push_back({callback==1251?Event::Type::Request:Event::Type::Failed,SteamNetworkingIdentity_GetSteamID64(&value),0,true});
        } else if(callback==506 && size>=sizeof(Chat)) {
            Chat value{}; std::memcpy(&value,data,sizeof(value));
            if(value.lobby==ownedLobby_ && (value.flags&0x1e)) output.push_back({Event::Type::Left,value.changed,0,true});
        }
    }
public:
    Steam(const std::wstring& path,const std::string& expected,bool ownMock):
        path_(absolutePath(path)),pinnedFile_(CreateFileW(path_.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)),ownMock_(ownMock) {
        if(expected.size()!=64 || !std::all_of(expected.begin(),expected.end(),[](char c){return (c>='0'&&c<='9') || (c>='a'&&c<='f') || (c>='A'&&c<='F');})) throw Failure("A verified DLL SHA-256 is required");
        std::string lower=expected; std::transform(lower.begin(),lower.end(),lower.begin(),[](char c){return c>='A'&&c<='F'?static_cast<char>(c-'A'+'a'):c;});
        if(ownMock_) {
            const auto exact=absolutePath(executableDirectory()+L"\\..\\api_abi_mock.dll");
            if(_wcsicmp(exact.c_str(),path_.c_str()) || lower!=MockSha256) throw Failure("ABI selftest requires the exact pinned project-built mock DLL");
        }
        if(hashHandle(pinnedFile_.get())!=lower) throw Failure("Steam API DLL SHA-256 mismatch");
        requireX64(pinnedFile_.get());
        // The read handle denies writes/deletion through load, closing the hash/load race.
        dll_=LoadLibraryExW(path_.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if(!dll_) throw Failure("Could not load the pinned x64 API DLL");
        try {
            if(ownMock_) {
                const auto marker=exportFunction<const char*(__cdecl*)()>(dll_,"FOF_MockIdentity");
                if(std::strcmp(marker(),"FOF_MOCK_NO_ACCOUNT")) throw Failure("Invalid own-code ABI fixture marker");
            }
#define LOAD(name) name=exportFunction<name##Type>(dll_,"SteamAPI_" #name)
            // SDKs before InitFlat export only SteamAPI_Init (bool); either entry point is accepted.
            if(GetProcAddress(dll_,"SteamAPI_InitFlat")) LOAD(InitFlat); else LOAD(Init);
            LOAD(Shutdown); LOAD(GetHSteamPipe); LOAD(ManualDispatch_Init); LOAD(ManualDispatch_RunFrame);
            LOAD(ManualDispatch_GetNextCallback); LOAD(ManualDispatch_FreeLastCallback); LOAD(ManualDispatch_GetAPICallResult);
            LOAD(SteamUser_v023); LOAD(SteamMatchmaking_v009); LOAD(SteamNetworkingMessages_SteamAPI_v002);
            LOAD(ISteamUser_GetSteamID); LOAD(ISteamUser_BLoggedOn); LOAD(ISteamMatchmaking_CreateLobby); LOAD(ISteamMatchmaking_JoinLobby);
            LOAD(ISteamMatchmaking_LeaveLobby); LOAD(ISteamMatchmaking_GetLobbyOwner); LOAD(ISteamMatchmaking_GetNumLobbyMembers);
            LOAD(ISteamMatchmaking_GetLobbyMemberByIndex); LOAD(ISteamMatchmaking_SetLobbyData); LOAD(ISteamMatchmaking_GetLobbyData);
            LOAD(SteamNetworkingIdentity_SetSteamID64); LOAD(SteamNetworkingIdentity_GetSteamID64);
            LOAD(ISteamNetworkingMessages_SendMessageToUser); LOAD(ISteamNetworkingMessages_ReceiveMessagesOnChannel);
            LOAD(ISteamNetworkingMessages_AcceptSessionWithUser); LOAD(ISteamNetworkingMessages_CloseSessionWithUser); LOAD(SteamNetworkingMessage_t_Release);
#undef LOAD
        } catch(...) { FreeLibrary(dll_); dll_=nullptr; throw; }
    }
    ~Steam() override { close(0,0); if(dll_) FreeLibrary(dll_); }
    const char* kind() const override { return ownMock_?"mock-steam-flat-api-abi (no Steam account)":"steam-spacewar-live"; }
    HMODULE module() const { return dll_; }
    void start(bool host,Id lobby) override {
        if(initialized_) throw Failure("Steam backend already initialized");
        if(!ownMock_) {
            // Reached exclusively via --steam-live; no live discovery or auto-start.
            configureLiveAppId();
        }
        char error[1024]{};
        const int result=InitFlat?InitFlat(error):(Init()?0:1);
        if(result!=0) throw Failure("Steam initialization failed ("+std::to_string(result)+")");
        initialized_=true; ownedLobby_=0; requestedLobby_=lobby; pending_.clear();
        user_=SteamUser_v023(); matchmaking_=SteamMatchmaking_v009(); network_=SteamNetworkingMessages_SteamAPI_v002();
        if(!user_ || !matchmaking_ || !network_ || !ISteamUser_BLoggedOn(user_)) throw Failure("Steam interfaces unavailable or account not logged in");
        me=ISteamUser_GetSteamID(user_); if(!me) throw Failure("Steam returned a zero identity");
        pipe_=GetHSteamPipe(); if(!pipe_) throw Failure("Steam returned no callback pipe"); ManualDispatch_Init();
        const Id call=host?ISteamMatchmaking_CreateLobby(matchmaking_,1,2):ISteamMatchmaking_JoinLobby(matchmaking_,lobby);
        if(!call) throw Failure("Steam rejected the lobby operation");
        pending_[call]={host?513:504,static_cast<unsigned>(host?sizeof(Created):sizeof(Entered))};
    }
    void setTag(Id lobby,const char* value) override { if(!ISteamMatchmaking_SetLobbyData(matchmaking_,lobby,"fof_native",value)) throw Failure("Could not set Friend or Foe lobby metadata"); }
    std::string tag(Id lobby) override {
        const char* value=ISteamMatchmaking_GetLobbyData(matchmaking_,lobby,"fof_native");
        if(!value) return {};
        const auto size=strnlen_s(value,128); if(size==128) throw Failure("Oversized lobby metadata");
        return {value,size};
    }
    Id owner(Id lobby) override { return ISteamMatchmaking_GetLobbyOwner(matchmaking_,lobby); }
    std::vector<Id> members(Id lobby) override {
        const int count=ISteamMatchmaking_GetNumLobbyMembers(matchmaking_,lobby);
        if(count<0 || count>2) throw Failure("Invalid Steam lobby member count");
        std::vector<Id> result; for(int i=0;i<count;++i) result.push_back(ISteamMatchmaking_GetLobbyMemberByIndex(matchmaking_,lobby,i));
        return result;
    }
    bool accept(Id user) override { const auto value=identity(user); return ISteamNetworkingMessages_AcceptSessionWithUser(network_,&value); }
    bool send(Id user,const Bytes& data) override {
        if(data.size()<HeaderSize || data.size()>HeaderSize+MaxChunk) throw Failure("Invalid outgoing Steam message size");
        const auto value=identity(user);
        const int result=ISteamNetworkingMessages_SendMessageToUser(network_,&value,data.data(),static_cast<unsigned>(data.size()),9,0);
        if(result==1) return true;
        if(result==10 || result==25) return false;
        throw Failure("Steam reliable send failed ("+std::to_string(result)+")");
    }
    std::vector<Event> events() override {
        ManualDispatch_RunFrame(pipe_);
        std::vector<Event> result; Callback callback{};
        unsigned count=0;
        while(ManualDispatch_GetNextCallback(pipe_,&callback)) {
            struct Release { Steam& s; ~Release(){s.ManualDispatch_FreeLastCallback(s.pipe_);} } release{*this};
            if(++count>256) throw Failure("Steam callback batch limit");
            if(!usableCallback(callback)) {
                if(skipped_++<16) log("STEAM_CALLBACK_SKIPPED","\"callback\":"+std::to_string(callback.callback)+",\"size\":"+std::to_string(callback.size));
                continue;
            }
            const auto* data=static_cast<const unsigned char*>(callback.param);
            if(callback.callback==703) {
                Completed done{}; std::memcpy(&done,data,sizeof(done));
                const auto pending=pending_.find(done.call);
                if(pending==pending_.end()) continue;
                if(pending->second!=std::make_pair(done.callback,done.size)) throw Failure("Steam lobby callback layout rejected");
                Bytes buffer(done.size); bool failed=false;
                if(!ManualDispatch_GetAPICallResult(pipe_,done.call,buffer.data(),static_cast<int>(done.size),done.callback,&failed) || failed) throw Failure("Steam lobby call result failed");
                pending_.erase(pending);
                addEvent(result,done.callback,buffer.data(),buffer.size());
            } else if(callback.callback!=513) addEvent(result,callback.callback,data,static_cast<std::size_t>(callback.size));
        }
        return result;
    }
    std::vector<Message> messages() override {
        std::array<void*,32> pointers{};
        const int count=ISteamNetworkingMessages_ReceiveMessagesOnChannel(network_,0,pointers.data(),static_cast<int>(pointers.size()));
        // Release all returned pointers even when a later message fails validation.
        struct Release {
            Steam& s; std::array<void*,32>& pointers;
            ~Release() { for(auto pointer:pointers) if(pointer) s.SteamNetworkingMessage_t_Release(pointer); }
        } release{*this,pointers};
        if(count<0 || count>static_cast<int>(pointers.size())) throw Failure("Steam message count rejected");
        std::vector<Message> result;
        for(int i=0;i<count;++i) {
            if(!pointers[static_cast<std::size_t>(i)]) throw Failure("Null Steam message");
            MessagePrefix value{}; std::memcpy(&value,pointers[static_cast<std::size_t>(i)],sizeof(value));
            if(!value.data || value.size<static_cast<int>(HeaderSize) || value.size>static_cast<int>(HeaderSize+MaxChunk)) throw Failure("Steam message size rejected");
            const Id sender=SteamNetworkingIdentity_GetSteamID64(&value.sender);
            const auto* bytes=static_cast<const unsigned char*>(value.data);
            result.push_back({sender,Bytes(bytes,bytes+value.size)});
        }
        return result;
    }
    void close(Id lobby,Id peer) noexcept override {
        if(!initialized_) return;
        if(peer && network_) { const auto value=identity(peer); ISteamNetworkingMessages_CloseSessionWithUser(network_,&value); }
        const Id leaving=lobby?lobby:ownedLobby_;
        if(leaving && matchmaking_) ISteamMatchmaking_LeaveLobby(matchmaking_,leaving);
        Shutdown(); initialized_=false; pending_.clear();
    }
};
void require(bool condition,const char* why) { if(!condition) throw Failure(why); }
std::wstring currentDirectory() {
    const DWORD length=GetCurrentDirectoryW(0,nullptr); std::wstring result(length,L'\0');
    const DWORD copied=GetCurrentDirectoryW(length,result.data());
    if(!copied || copied>=length) throw Failure("Could not read current directory");
    result.resize(copied); return result;
}
std::wstring environment(const wchar_t* name) {
    SetLastError(0); const DWORD length=GetEnvironmentVariableW(name,nullptr,0);
    if(!length) return GetLastError()==ERROR_ENVVAR_NOT_FOUND?L"<ABSENT>":L"";
    std::wstring result(length,L'\0'); const DWORD copied=GetEnvironmentVariableW(name,result.data(),length);
    if(copied>=length) throw Failure("Environment changed during selftest"); result.resize(copied); return result;
}
} // namespace
std::wstring executableDirectory() {
    std::wstring value(32768,L'\0'); const DWORD count=GetModuleFileNameW(nullptr,value.data(),static_cast<DWORD>(value.size()));
    if(!count || count>=value.size()) throw Failure("Could not locate native relay executable");
    value.resize(count); const auto slash=value.find_last_of(L"\\/");
    if(slash==std::wstring::npos) throw Failure("Native relay executable has no directory"); return value.substr(0,slash);
}
std::wstring absolutePath(const std::wstring& value) {
    const DWORD length=GetFullPathNameW(value.c_str(),0,nullptr,nullptr);
    if(!length) throw Failure("Invalid DLL/file path");
    std::wstring result(length,L'\0'); const DWORD copied=GetFullPathNameW(value.c_str(),length,result.data(),nullptr);
    if(!copied || copied>=length) throw Failure("Could not resolve absolute path"); result.resize(copied); return result;
}
std::string sha256File(const std::wstring& path) {
    Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)); return hashHandle(file.get());
}
std::unique_ptr<Backend> steamBackend(const std::wstring& path,const std::string& hash,bool ownMock) { return std::make_unique<Steam>(path,hash,ownMock); }
int abiSelftest() {
    const auto directory=currentDirectory(), app=environment(L"SteamAppId"), game=environment(L"SteamGameId");
    const auto path=absolutePath(executableDirectory()+L"\\..\\api_abi_mock.dll");
    require(GetModuleHandleW(L"steam_api64.dll")==nullptr,"Unexpected real Steam API loaded in ABI selftest");
    bool rejected=false;
    try { auto invalid=steamBackend(path,std::string(64,'0'),true); } catch(const Failure&) { rejected=true; }
    require(rejected,"Wrong mock SHA-256 was accepted");
    auto api=steamBackend(path,MockSha256,true);
    auto* concrete=dynamic_cast<Steam*>(api.get()); require(concrete!=nullptr,"ABI fixture backend type");
    const auto metric=exportFunction<int(__cdecl*)(int)>(concrete->module(),"FOF_MockMetric");
    std::array<int,5> before{}; for(int i=0;i<5;++i) before[static_cast<std::size_t>(i)]=metric(i);
    constexpr Id lobby=480000123456ULL;
    int big=0; Callback odd[]={{1,1999999,&big,1<<20},{1,1222,nullptr,8},{1,7,nullptr,-1},{1,703,&big,4},{1,504,&big,4}};
    require(!usableCallback(odd[1]) && !usableCallback(odd[2]) && !usableCallback(odd[3]) && usableCallback(odd[0]) && usableCallback(odd[4]),"Callback size rules");
    for(bool host:{true,false}) {
        api->start(host,host?0:lobby);
        const auto events=api->events(); require(events.size()==2,"Owned and duplicate lobby callbacks expected");
        for(const auto& event:events) require(event.type==Event::Type::Lobby && event.lobby==lobby && event.success,"Lobby callback ABI mismatch");
        require(api->me==101 && api->members(lobby)==std::vector<Id>{101,202} && api->owner(lobby)==101,"Identity/member ABI mismatch");
        api->setTag(lobby,Tag); require(api->tag(lobby)==Tag && api->accept(202),"Metadata/session ABI mismatch");
        Bytes body(MaxChunk); for(std::size_t i=0;i<body.size();++i) body[i]=static_cast<unsigned char>(i&255);
        const auto payload=encode(Kind::Data,7,body); require(payload.size()==16400,"Full-size ABI payload");
        require(api->send(202,payload),"ABI message send failed");
        const auto messages=api->messages(); require(messages.size()==1 && messages[0].sender==202 && messages[0].data==payload,"ABI message/payload identity mismatch");
        require(api->messages().empty(),"ABI message was not released/consumed");
        api->close(lobby,202);
    }
    const std::array<int,5> expected{0,2,2,2,6};
    for(int i=0;i<5;++i) require(metric(i)-before[static_cast<std::size_t>(i)]==expected[static_cast<std::size_t>(i)],"Mock reports ABI ownership or lifetime error");
    require(directory==currentDirectory() && app==environment(L"SteamAppId") && game==environment(L"SteamGameId") && GetModuleHandleW(L"steam_api64.dll")==nullptr,"ABI selftest changed Steam environment/directory or loaded a real Steam API");
    log("ABI_SELFTEST_PASS","\"mock_sha256\":"+jsonString(MockSha256)+",\"host_join_operations\":2,\"payload_bytes_per_operation\":16400,\"released_messages\":2,\"freed_callbacks\":6,\"account_used\":false,\"environment_unchanged\":true");
    return 0;
}
} // namespace fof
