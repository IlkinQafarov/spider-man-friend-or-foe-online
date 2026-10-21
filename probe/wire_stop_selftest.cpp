#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include "input_wire.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <thread>
#pragma comment(lib,"ws2_32.lib")
namespace {
using namespace coop;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F> bool until(F predicate){for(unsigned i=0;i<200;i++){if(predicate())return true;Sleep(5);}return predicate();}
bool bytes(SOCKET s,void* data,int length,bool sendData){char* p=(char*)data;while(length){int n=sendData?send(s,p,length,0):recv(s,p,length,0);if(n<=0)return false;p+=n;length-=n;}return true;}
SOCKET listenAt(std::uint16_t& port){
    SOCKET s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);require(s!=INVALID_SOCKET,"raw socket");
    sockaddr_in a={};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a.sin_port=0;
    require(bind(s,(sockaddr*)&a,sizeof(a))==0,"raw bind");int size=sizeof(a);require(getsockname(s,(sockaddr*)&a,&size)==0,"getsockname");port=ntohs(a.sin_port);require(listen(s,1)==0,"raw listen");return s;
}
struct Pair {
    InputWire a,b;
    explicit Pair(unsigned delay=3){std::uint16_t port=0;SOCKET reservation=listenAt(port);closesocket(reservation);Digest d={};
        auto host=std::async(std::launch::async,[&]{return a.Open(true,"127.0.0.1",port,d,d,delay,1500);});
        bool client=b.Open(false,"127.0.0.1",port,d,d,delay,1500);bool server=host.get();require(client&&server,"pair open");
    }
};
void ticks(Pair& p,unsigned count,unsigned delay=3){
    for(unsigned tick=0;tick<count;tick++){
        Actions x={},y={},rx={},ry={};x[3]=float(tick%4)*.25f;y[7]=float(tick%4)*.25f;
        std::uint64_t sx=0,sy=0;
        auto first=std::async(std::launch::async,[&]{return p.a.Step(tick,x,rx,1000+tick,&sx);});
        bool second=p.b.Step(tick,y,ry,2000+tick,&sy);bool ok=first.get();require(ok&&second,"tick failed");
        if(tick<delay){require(rx==Actions{}&&ry==Actions{}&&sx==0&&sy==0,"delay neutrality");}
        else {require(rx[7]==float((tick-delay)%4)*.25f&&ry[3]==float((tick-delay)%4)*.25f,"action stream");require(sx==2000+tick-delay&&sy==1000+tick-delay,"state stream");}
    }
}
void oneSided(){Pair p;ticks(p,18);require(p.a.RequestStop(),"request stop");require(p.a.RequestStop(),"stop idempotence");require(until([&]{return p.b.PeerStopped();}),"peer stop missing");p.a.Close();Actions x={},r={};require(!p.b.Step(18,x,r),"peer step after stop");require(p.a.Error().empty()&&p.b.Error().empty(),"normal stop produced error");require(!p.b.Finish(),"stop counted as finite finish");}
void simultaneous(){
    for(unsigned n=0;n<20;n++){Pair p;ticks(p,4);std::atomic<bool> go{false};auto f=std::async(std::launch::async,[&]{while(!go)std::this_thread::yield();return p.a.RequestStop();});go=true;bool b=p.b.RequestStop();bool a=f.get();require(a&&b,"simultaneous send");require(until([&]{return p.a.PeerStopped()&&p.b.PeerStopped();}),"simultaneous marker");require(p.a.Error().empty()&&p.b.Error().empty(),"simultaneous error");}
}
void immediateClose(){for(unsigned n=0;n<20;n++){Pair p;ticks(p,4);require(p.a.RequestStop(),"immediate stop");p.a.Close();require(until([&]{return p.b.PeerStopped()||!p.b.Error().empty();}),"immediate stop missing");require(p.b.PeerStopped()&&p.b.Error().empty(),"immediate close lost STOP");}}
void blockedRemote(){Pair p(0);Actions x={},r={};auto f=std::async(std::launch::async,[&]{return p.a.Step(0,x,r);});Sleep(40);auto start=GetTickCount64();require(p.b.RequestStop(),"remote blocked stop");require(!f.get(),"blocked Step accepted stop as input");require(GetTickCount64()-start<500,"remote Step wake too slow");require(p.a.PeerStopped()&&p.a.Error().empty()&&p.b.Error().empty(),"remote stop diagnosis");}
void blockedLocal(){Pair p(0);Actions x={},r={};auto f=std::async(std::launch::async,[&]{return p.a.Step(0,x,r);});Sleep(40);auto start=GetTickCount64();require(p.a.RequestStop(),"local blocked stop");require(!f.get(),"local Step ignored stop");require(GetTickCount64()-start<500,"local Step wake too slow");require(until([&]{return p.b.PeerStopped();}),"local marker missing");p.b.Close();Sleep(20);require(p.a.Error().empty(),"intentional EOF not suppressed");}
void unexpectedEof(){Pair p;p.b.Close();require(until([&]{return !p.a.Error().empty();}),"unexpected EOF accepted");std::string before=p.a.Error();require(!p.a.PeerStopped()&&!p.a.RequestStop(),"failure became normal stop");require(p.a.Error()==before,"earlier error lost");}
void finite(){Pair p;ticks(p,40);auto a=std::async(std::launch::async,[&]{return p.a.Finish();});bool b=p.b.Finish();require(a.get()&&b,"finite FIN regression");require(!p.a.PeerStopped()&&!p.b.PeerStopped(),"FIN became STOP");require(p.a.Error().empty()&&p.b.Error().empty(),"finite error");}
struct Hello {std::uint32_t magic,version,role,delay;Digest build,scenario;};
struct Frame {std::uint32_t magic,tick;Actions actions;std::uint64_t state;};
static_assert(sizeof(Hello)==80&&sizeof(Frame)==128,"raw fixture ABI");
void malformed(std::uint32_t magic,std::uint32_t tick,bool badPayload,const char* expected){
    std::uint16_t port=0;SOCKET listener=listenAt(port);InputWire wire;Digest d={};auto opened=std::async(std::launch::async,[&]{return wire.Open(false,"127.0.0.1",port,d,d,3,1500);});
    SOCKET peer=accept(listener,0,0);closesocket(listener);require(peer!=INVALID_SOCKET,"raw accept");Hello hello={};require(bytes(peer,&hello,sizeof(hello),false),"hello receive");require(hello.version==3,"not protocol3");hello.role=0;require(bytes(peer,&hello,sizeof(hello),true),"hello reply");require(opened.get(),"raw open");
    Frame marker={magic,tick,{},0};if(badPayload)marker.actions[1]=.25f;require(bytes(peer,&marker,sizeof(marker),true),"bad marker send");require(until([&]{return !wire.Error().empty();}),"bad marker accepted");require(wire.Error().find(expected)!=std::string::npos,"wrong marker diagnosis");require(!wire.PeerStopped()&&!wire.RequestStop(),"malformed became normal stop");closesocket(peer);
}
void oldProtocol(){
    std::uint16_t port=0;SOCKET listener=listenAt(port);InputWire wire;Digest d={};auto opened=std::async(std::launch::async,[&]{return wire.Open(false,"127.0.0.1",port,d,d,3,1500);});SOCKET peer=accept(listener,0,0);closesocket(listener);Hello hello={};require(bytes(peer,&hello,sizeof(hello),false),"old hello read");hello.role=0;hello.version=2;require(bytes(peer,&hello,sizeof(hello),true),"old hello send");require(!opened.get()&&wire.Error().find("protocol mismatch")!=std::string::npos,"old protocol accepted");closesocket(peer);
}
}
int main(){
    WSADATA data={};if(WSAStartup(MAKEWORD(2,2),&data))return 2;
    struct Test {const char* name;void(*run)();};
    Test tests[]={
        {"one-sided normal stop",oneSided},{"simultaneous stop x20",simultaneous},
        {"STOP then immediate Close x20",immediateClose},
        {"blocked Step remote wake",blockedRemote},{"blocked Step local wake and intentional EOF",blockedLocal},
        {"unexpected EOF retains failure",unexpectedEof},{"finite FIN and state/input regression",finite},
        {"invalid STOP order",[]{malformed(0x31504f53,1,false,"Invalid stop");}},
        {"invalid STOP payload",[]{malformed(0x31504f53,0,true,"Invalid stop");}},
        {"invalid input order",[]{malformed(0x314d5246,4,false,"out-of-order");}},
        {"invalid FIN order",[]{malformed(0x314e4946,1,false,"Invalid finish");}},
        {"protocol2 rejection",oldProtocol}
    };
    unsigned failures=0;for(const auto& test:tests){try{test.run();printf("PASS %s\n",test.name);}catch(const std::exception& e){++failures;printf("FAIL %s: %s\n",test.name,e.what());}}
    WSACleanup();printf("WIRE_STOP_SELFTEST %u/%u passed\n",unsigned(sizeof(tests)/sizeof(tests[0]))-failures,unsigned(sizeof(tests)/sizeof(tests[0])));return failures?1:0;
}
