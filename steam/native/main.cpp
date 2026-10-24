#include <windows.h>
#include <timeapi.h>
#include "relay.hpp"
#include <cerrno>
#include <cstdio>
#include <cwchar>
#include <limits>

namespace {
volatile LONG stopped=0;
BOOL WINAPI control(DWORD event) {
    if(event==CTRL_C_EVENT || event==CTRL_BREAK_EVENT || event==CTRL_CLOSE_EVENT) { InterlockedExchange(&stopped,1); return TRUE; }
    return FALSE;
}
bool stdinStop() {
    static std::string pending;
    const HANDLE input=GetStdHandle(STD_INPUT_HANDLE);
    if(input==INVALID_HANDLE_VALUE || input==nullptr || GetFileType(input)!=FILE_TYPE_PIPE) return false;
    DWORD available=0;
    if(!PeekNamedPipe(input,nullptr,0,nullptr,&available,nullptr) || !available) return false;
    char buffer[128]{}; DWORD read=0;
    if(!ReadFile(input,buffer,std::min<DWORD>(available,sizeof(buffer)),&read,nullptr)) return false;
    pending.append(buffer,read);
    if(pending.size()>256) throw fof::Failure("Oversized relay stdin command");
    const auto newline=pending.find('\n');
    if(newline==std::string::npos) return false;
    auto line=pending.substr(0,newline); pending.erase(0,newline+1);
    if(!line.empty() && line.back()=='\r') line.pop_back();
    if(line!="STOP") throw fof::Failure("Unknown relay stdin command");
    return true;
}
fof::Id number(const std::wstring& text,const char* name) {
    if(text.empty() || text[0]<L'0' || text[0]>L'9') throw fof::Failure(std::string("Invalid ")+name);
    for(auto c:text) if(c<L'0' || c>L'9') throw fof::Failure(std::string("Invalid ")+name);
    errno=0; wchar_t* end=nullptr; const auto result=_wcstoui64(text.c_str(),&end,10);
    if(errno==ERANGE || !end || *end) throw fof::Failure(std::string("Invalid ")+name); return result;
}
std::string ascii(const std::wstring& value) {
    std::string result; for(auto c:value) { if(c>127) throw fof::Failure("Expected ASCII SHA-256"); result+=static_cast<char>(c); } return result;
}
bool isAbsolute(const std::wstring& path) {
    return path.size()>2 && ((path[1]==L':' && (path[2]==L'\\' || path[2]==L'/')) || (path[0]==L'\\' && path[1]==L'\\'));
}
void help() {
    std::puts("Own-code x64 Windows Friend or Foe stream relay\n"
        "  native-relay.exe --abi-selftest\n"
        "  native-relay.exe --protocol-selftest\n"
        "  native-relay.exe host|join --steam-live <absolute existing API DLL>\n"
        "      --steam-api-sha256 <SHA256> [--lobby <uint64>] [--local-port 49375]\n"
        "      [--seconds 0..600] [--ready-file <absolute NEW JSON>] [--smoke]\n"
        "seconds0 persists until stream completion or cancellation.\n"
        "Live mode uses Steam Spacewar (480); it never sends invitations or launches a game.");
}
}
int wmain(int argc,wchar_t** argv) {
    // 1 ms timer: the relay loop's Sleep(2) otherwise lasts a 15.6 ms tick, adding up to ~60 ms per round trip.
    timeBeginPeriod(1);
    try {
        if(argc==2 && std::wstring(argv[1])==L"--abi-selftest") return fof::abiSelftest();
        if(argc==2 && std::wstring(argv[1])==L"--protocol-selftest") return fof::protocolSelftest();
        if(argc==2 && std::wstring(argv[1])==L"--stdin-selftest") {
            const auto deadline=GetTickCount64()+5000;
            while(GetTickCount64()<deadline) {
                if(stdinStop()) { fof::log("STDIN_SELFTEST_PASS","\"stop_command\":true,\"account_used\":false"); return 0; }
                Sleep(1);
            }
            throw fof::Failure("Stdin selftest STOP command timed out");
        }
        if(argc==2 && (std::wstring(argv[1])==L"--help" || std::wstring(argv[1])==L"-h")) { help(); return 0; }
        if(argc<2) { help(); return 2; }
        const std::wstring role=argv[1];
        if(role!=L"host" && role!=L"join") throw fof::Failure("Role must be host or join");
        std::wstring path,hash,ready,mode; fof::Id lobby=0,port=49375,seconds=240; bool smoke=false;
        std::vector<std::wstring> seen;
        for(int i=2;i<argc;++i) {
            const std::wstring option=argv[i];
            if(std::find(seen.begin(),seen.end(),option)!=seen.end()) throw fof::Failure("Duplicate command-line option");
            seen.push_back(option);
            if(option==L"--smoke") { smoke=true; continue; }
            if(i+1>=argc) throw fof::Failure("Missing command-line option value");
            const std::wstring value=argv[++i];
            if(option==L"--steam-live") path=value;
            else if(option==L"--steam-api-sha256") hash=value;
            else if(option==L"--ready-file") ready=value;
            else if(option==L"--lobby") lobby=number(value,"lobby ID");
            else if(option==L"--local-port") port=number(value,"local port");
            else if(option==L"--seconds") seconds=number(value,"duration");
            else if(option==L"--mode") mode=value;
            else throw fof::Failure("Unknown command-line option");
        }
        if(!isAbsolute(path)) throw fof::Failure("Explicit --steam-live absolute DLL path is required");
        if(hash.empty()) throw fof::Failure("Explicit --steam-api-sha256 is required");
        if(!ready.empty() && !isAbsolute(ready)) throw fof::Failure("Ready file must be an absolute NEW file path");
        if(port<1024 || port>65535 || seconds>600) throw fof::Failure("Invalid local port or duration");
        if(role==L"join" && !lobby) throw fof::Failure("Join requires --lobby");
        if(role==L"host" && lobby) throw fof::Failure("Host creates a lobby; do not supply --lobby");
        if(mode.size()>16 || std::any_of(mode.begin(),mode.end(),[](wchar_t c){return c<L'a' || c>L'z';})) throw fof::Failure("Invalid play mode");
        if(smoke && role!=L"host") throw fof::Failure("Smoke check creates a host lobby only");
        auto api=fof::steamBackend(path,ascii(hash),false);
        fof::Winsock winsock;
        fof::Relay relay(*api,role==L"host",static_cast<unsigned short>(port),ready);
        relay.setMode(ascii(mode));
        SetConsoleCtrlHandler(control,TRUE); relay.start(lobby);
        const auto start=GetTickCount64();
        for(;;) {
            if(InterlockedCompareExchange(&stopped,0,0) || stdinStop()) { relay.close(); fof::log("RELAY_CANCELLED"); return 130; }
            const auto now=GetTickCount64();
            if(smoke) {
                relay.pumpCallbacks();
                if(relay.lobby) { fof::log("LOBBY_SMOKE_PASS","\"transport\":"+fof::jsonString(api->kind())+",\"game_traffic\":false,\"invites_sent\":false"); relay.close(); fof::log("LOBBY_SMOKE_CLOSED","\"lobby_was_ready\":true"); return 0; }
                if(now-start>15000) throw fof::Failure("Lobby smoke check timed out");
            } else {
                if(relay.step(now)) return 0;
                if(seconds && now-start>=seconds*1000) throw fof::Failure("Relay test deadline expired");
            }
            Sleep(2);
        }
    } catch(const std::exception& error) { fof::log("RELAY_FAIL","\"reason\":"+fof::jsonString(error.what())); return 1; }
}
