#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include "input_wire.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
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
    explicit Pair(unsigned delay=3,unsigned timeoutMs=1500){std::uint16_t port=0;SOCKET reservation=listenAt(port);closesocket(reservation);Digest d={};
        auto host=std::async(std::launch::async,[&]{return a.Open(true,"127.0.0.1",port,d,d,delay,timeoutMs);});
        bool client=b.Open(false,"127.0.0.1",port,d,d,delay,timeoutMs);bool server=host.get();require(client&&server,"pair open");
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
struct Frame {std::uint32_t magic,tick;Actions actions;std::uint64_t state;MenuPointer pointer;std::uint32_t reserved;};
static_assert(sizeof(Hello)==80&&sizeof(Frame)==144,"raw fixture ABI");
struct PhasePacket {std::uint32_t magic,sequence;PhaseState state;std::uint64_t reserved;};
static_assert(sizeof(PhasePacket)==128,"raw phase ABI");
struct BlobHeader {std::uint32_t magic,version,reserved,id,total,offset,length,checksum,headerChecksum;};
static_assert(sizeof(BlobHeader)==36,"raw blob ABI");
std::uint32_t hashBytes(const std::uint8_t* data,std::size_t size){std::uint32_t h=2166136261u;for(std::size_t i=0;i<size;i++)h=(h^data[i])*16777619u;return h;}
BlobHeader blobHeader(std::uint32_t id,std::uint32_t total,std::uint32_t offset,std::uint32_t length,const std::vector<std::uint8_t>& body){
    BlobHeader h={0x31424c42,1,0,id,total,offset,length,hashBytes(body.data(),body.size()),0};h.headerChecksum=hashBytes((const std::uint8_t*)&h,offsetof(BlobHeader,headerChecksum));return h;
}
PhaseState phase(unsigned role,unsigned sequence){
    PhaseState s={};s.kind=sequence%2?phaseLoad:phaseMenu;s.id=0xfeed0000u+sequence;s.selection=role;s.flags=sequence%4;
    s.identity[sequence%32]=std::uint8_t(role+1);s.content[31]=std::uint8_t(0xae^sequence);
    for(unsigned i=0;i<8;i++)s.auxiliary[i]=sequence*100+i+role*10000;return s;
}
bool equal(const PhaseState& a,const PhaseState& b){return a.kind==b.kind&&a.id==b.id&&a.selection==b.selection&&a.flags==b.flags&&a.identity==b.identity&&a.content==b.content&&a.auxiliary==b.auxiliary;}
void phaseRange(Pair& p,unsigned first,unsigned end){
    for(unsigned sequence=first;sequence<end;sequence++){
        PhaseState a=phase(0,sequence),b=phase(1,sequence),ra={},rb={};
        auto f=std::async(std::launch::async,[&]{return p.a.ExchangePhase(sequence,a,ra);});bool ok=p.b.ExchangePhase(sequence,b,rb);
        require(f.get()&&ok&&equal(ra,b)&&equal(rb,a),"phase state altered, mixed, or semantically auto-approved");
    }
}
void phaseAndInput(){Pair p;phaseRange(p,0,8);ticks(p,24);phaseRange(p,8,20);auto f=std::async(std::launch::async,[&]{return p.a.Finish();});require(p.b.Finish()&&f.get(),"FIN after phase/input interleave");require(p.a.Error().empty()&&p.b.Error().empty(),"phase/input error");}
void phaseRemoteStop(){Pair p;PhaseState s=phase(0,0),r={};auto f=std::async(std::launch::async,[&]{return p.a.ExchangePhase(0,s,r);});Sleep(40);auto start=GetTickCount64();require(p.b.RequestStop(),"phase remote STOP send");require(!f.get()&&p.a.PeerStopped()&&p.a.Error().empty(),"phase wait STOP classification");require(GetTickCount64()-start<500,"phase STOP wake too slow");}
void phaseLocalStop(){Pair p;PhaseState s=phase(0,0),r={};auto f=std::async(std::launch::async,[&]{return p.a.ExchangePhase(0,s,r);});Sleep(40);auto start=GetTickCount64();require(p.a.RequestStop(),"phase local STOP send");require(!f.get()&&p.a.Error().empty(),"local phase stop diagnosis");require(GetTickCount64()-start<500,"local phase STOP wake too slow");require(until([&]{return p.b.PeerStopped();}),"phase local STOP missing");}
void phaseDisconnect(){Pair p;PhaseState s=phase(0,0),r={};auto f=std::async(std::launch::async,[&]{return p.a.ExchangePhase(0,s,r);});Sleep(40);p.b.Close();require(!f.get()&&!p.a.Error().empty()&&!p.a.PeerStopped(),"phase EOF ignored");}
void phaseTimeout(){Pair p;PhaseState s=phase(0,0),r={};require(!p.a.ExchangePhase(0,s,r)&&!p.a.Error().empty(),"unanswered phase did not fail");}
void idleThenExchange(){Pair p(3,300);Sleep(900);require(p.a.Error().empty()&&p.b.Error().empty(),"idle beyond API timeout broke connection");phaseRange(p,0,3);ticks(p,8);auto f=std::async(std::launch::async,[&]{return p.a.Finish();});bool b=p.b.Finish();require(f.get()&&b&&p.a.Error().empty()&&p.b.Error().empty(),"exchange after idle failed");}
struct RawPeer {
    InputWire wire;SOCKET peer=INVALID_SOCKET;
    explicit RawPeer(unsigned timeoutMs=1500){std::uint16_t port=0;SOCKET listener=listenAt(port);Digest d={};auto f=std::async(std::launch::async,[&]{return wire.Open(false,"127.0.0.1",port,d,d,3,timeoutMs);});
        peer=accept(listener,0,0);closesocket(listener);require(peer!=INVALID_SOCKET,"phase raw accept");Hello h={};require(bytes(peer,&h,sizeof(h),false)&&h.version==6,"phase hello receive");h.role=0;require(bytes(peer,&h,sizeof(h),true),"phase hello reply");require(f.get(),"phase raw handshake");}
    ~RawPeer(){if(peer!=INVALID_SOCKET)closesocket(peer);}
};
void sendRawBlob(RawPeer& raw,BlobHeader h,const std::vector<std::uint8_t>& body){
    h.headerChecksum=hashBytes((const std::uint8_t*)&h,offsetof(BlobHeader,headerChecksum));require(bytes(raw.peer,&h,sizeof(h),true),"raw blob header send");
    if(h.length&&body.size()>=h.offset+h.length)require(bytes(raw.peer,(void*)(body.data()+h.offset),(int)h.length,true),"raw blob body send");
}
void badPhase(unsigned mutation){
    RawPeer r;PhasePacket packet={0x31534850,0,phase(0,0),0};
    if(mutation==0)packet.state.kind=3;if(mutation==1)packet.state.flags=4;if(mutation==2)packet.reserved=1;if(mutation==3)packet.sequence=1;
    require(bytes(r.peer,&packet,sizeof(packet),true),"malformed phase send");
    if(mutation==4)require(bytes(r.peer,&packet,sizeof(packet),true),"duplicate phase send");
    if(mutation==5)for(unsigned i=1;i<=16;i++){packet.sequence=i;require(bytes(r.peer,&packet,sizeof(packet),true),"phase queue fill");}
    require(until([&]{return !r.wire.Error().empty();}),"malformed/duplicate/overflow phase accepted");require(!r.wire.PeerStopped(),"bad phase became STOP");
}
void invalidLocalPhase(){Pair p;PhaseState s=phase(0,0),r={};s.flags=4;require(!p.a.ExchangePhase(0,s,r)&&p.a.Error().find("Invalid local phase")!=std::string::npos,"invalid local phase accepted");}
void invalidLocalPhaseSequence(){Pair p;PhaseState s=phase(0,0),r={};require(!p.a.ExchangePhase(1,s,r)&&p.a.Error().find("Invalid local phase")!=std::string::npos,"local phase gap accepted");}
void phaseFinWake(){RawPeer r;PhaseState s=phase(0,0),out={};auto f=std::async(std::launch::async,[&]{return r.wire.ExchangePhase(0,s,out);});PhasePacket received={};require(bytes(r.peer,&received,sizeof(received),false),"phase FIN fixture receive");Frame fin={0x314e4946,0,{},0};auto start=GetTickCount64();require(bytes(r.peer,&fin,sizeof(fin),true),"phase FIN send");require(!f.get()&&r.wire.Error().empty()&&!r.wire.PeerStopped(),"phase FIN wake classification");require(GetTickCount64()-start<500,"phase FIN wake too slow");}
// Run 057: the peer's last phase and its FIN can arrive together; the queued
// phase must still complete the exchange, and the following Finish must succeed.
void phaseThenFin(){for(unsigned n=0;n<20;n++){RawPeer r;PhaseState s=phase(0,0),out={};PhasePacket packet={0x31534850,0,phase(1,0),0},received={};Frame fin={0x314e4946,0,{},0};
    auto f=std::async(std::launch::async,[&]{return r.wire.ExchangePhase(0,s,out);});
    require(bytes(r.peer,&received,sizeof(received),false),"phase+FIN fixture receive");
    require(bytes(r.peer,&packet,sizeof(packet),true)&&bytes(r.peer,&fin,sizeof(fin),true),"phase+FIN send");
    require(f.get()&&equal(out,packet.state),"phase queued before FIN was dropped");
    require(r.wire.Finish()&&r.wire.Error().empty(),"Finish after phase+FIN failed");}}
void partialFrameTimeout(unsigned operation){
    RawPeer r(300);Actions actions={},remote={};PhaseState local=phase(0,0),out={};
    if(operation==1)for(unsigned i=0;i<3;i++)require(r.wire.Step(i,actions,remote),"partial Step setup");
    auto start=GetTickCount64();auto f=std::async(std::launch::async,[&]{return operation==0?r.wire.ExchangePhase(0,local,out):operation==1?r.wire.Step(3,actions,remote):r.wire.Finish();});
    if(operation==0){PhasePacket received={};require(bytes(r.peer,&received,sizeof(received),false),"partial phase fixture receive");}
    else {Frame received={};for(unsigned i=0;i<(operation==1?4u:1u);i++)require(bytes(r.peer,&received,sizeof(received),false),"partial fixture receive");}
    PhasePacket stalled={0x31534850,0,phase(0,0),0};require(bytes(r.peer,&stalled,16,true),"partial fixture prefix");
    require(!f.get()&&r.wire.Error().find("timed out")!=std::string::npos,"partial packet hid API timeout");r.wire.Close();
    require(GetTickCount64()-start<1200,"partial reader failed to unblock after timeout");
}
void malformed(std::uint32_t magic,std::uint32_t tick,bool badPayload,const char* expected){
    std::uint16_t port=0;SOCKET listener=listenAt(port);InputWire wire;Digest d={};auto opened=std::async(std::launch::async,[&]{return wire.Open(false,"127.0.0.1",port,d,d,3,1500);});
    SOCKET peer=accept(listener,0,0);closesocket(listener);require(peer!=INVALID_SOCKET,"raw accept");Hello hello={};require(bytes(peer,&hello,sizeof(hello),false),"hello receive");require(hello.version==6,"not protocol6");hello.role=0;require(bytes(peer,&hello,sizeof(hello),true),"hello reply");require(opened.get(),"raw open");
    Frame marker={magic,tick,{},0};if(badPayload)marker.actions[1]=.25f;require(bytes(peer,&marker,sizeof(marker),true),"bad marker send");require(until([&]{return !wire.Error().empty();}),"bad marker accepted");require(wire.Error().find(expected)!=std::string::npos,"wrong marker diagnosis");require(!wire.PeerStopped()&&!wire.RequestStop(),"malformed became normal stop");closesocket(peer);
}
void invalidRemotePointer(unsigned mutation){
    RawPeer r;Frame packet={0x314d5246,3,{},0,{},0};
    if(mutation==0)packet.pointer.x=65536;if(mutation==1)packet.pointer.y=65536;
    if(mutation==2)packet.pointer.flags=4;if(mutation==3)packet.pointer.flags=menuPointerClick;
    if(mutation==4)packet.pointer.x=1;if(mutation==5)packet.reserved=1;
    if(mutation==6){packet.magic=0x31504f53;packet.tick=0;packet.pointer={1,1,menuPointerActive};}
    if(mutation==7){packet.magic=0x314e4946;packet.tick=0;packet.pointer={1,1,menuPointerActive};}
    require(bytes(r.peer,&packet,sizeof(packet),true),"invalid pointer fixture send");
    require(until([&]{return !r.wire.Error().empty();}),"invalid pointer metadata accepted");
    require(!r.wire.PeerStopped(),"invalid pointer became normal stop");
}
void invalidLocalPointer(){
    const MenuPointer values[]={{65536,0,menuPointerActive},{0,65536,menuPointerActive},{0,0,4},{0,0,menuPointerClick},{1,0,0},{0,1,0}};
    for(const auto& value:values){Pair p;Actions a={},r={};require(!p.a.Step(0,a,r,0,nullptr,&value)&&p.a.Error()=="Invalid local input sequence","invalid local pointer accepted");}
}
void oldProtocol(unsigned version){
    std::uint16_t port=0;SOCKET listener=listenAt(port);InputWire wire;Digest d={};auto opened=std::async(std::launch::async,[&]{return wire.Open(false,"127.0.0.1",port,d,d,3,1500);});SOCKET peer=accept(listener,0,0);closesocket(listener);Hello hello={};require(bytes(peer,&hello,sizeof(hello),false),"old hello read");hello.role=0;hello.version=version;require(bytes(peer,&hello,sizeof(hello),true),"old hello send");require(!opened.get()&&wire.Error().find("protocol mismatch")!=std::string::npos,"old protocol accepted");closesocket(peer);
}
void malformedBlob(unsigned mutation){
    RawPeer raw;std::vector<std::uint8_t> body(8,0x5a);BlobHeader h=blobHeader(0,8,0,8,body);
    if(mutation==0)h.version=2;else if(mutation==1)h.reserved=1;else if(mutation==2)h.id=1;
    else if(mutation==3)h.total=0;else if(mutation==4)h.total=blobMaxBytes+1;
    else if(mutation==5)h.offset=1;else if(mutation==6)h.length=0;else if(mutation==7)h.length=blobChunkBytes+1;
    else if(mutation==8)h.headerChecksum^=1;else if(mutation==9)h.checksum^=1;
    if(mutation!=8)h.headerChecksum=hashBytes((const std::uint8_t*)&h,offsetof(BlobHeader,headerChecksum));
    require(bytes(raw.peer,&h,sizeof(h),true),"malformed blob header send");
    if(mutation==9)require(bytes(raw.peer,body.data(),(int)body.size(),true),"bad checksum payload send");
    require(until([&]{return !raw.wire.Error().empty();}),"malformed blob accepted");require(!raw.wire.PeerStopped(),"bad blob became STOP");
}
void blobChunkOffsetMismatch(){
    RawPeer raw;std::vector<std::uint8_t> body(blobChunkBytes+1,0x37);BlobHeader first=blobHeader(0,(std::uint32_t)body.size(),0,blobChunkBytes,body);
    sendRawBlob(raw,first,body);BlobHeader second=blobHeader(0,(std::uint32_t)body.size(),1,blobChunkBytes,body);
    require(bytes(raw.peer,&second,sizeof(second),true),"offset mismatch header send");
    require(until([&]{return !raw.wire.Error().empty();}),"noncontiguous blob offset accepted");
}
void blobChunkTotalMismatch(){
    RawPeer raw;std::vector<std::uint8_t> body(blobChunkBytes+1,0x31);BlobHeader first=blobHeader(0,(std::uint32_t)body.size(),0,blobChunkBytes,body);
    sendRawBlob(raw,first,body);BlobHeader second=blobHeader(0,(std::uint32_t)body.size()+1,blobChunkBytes,2,body);
    require(bytes(raw.peer,&second,sizeof(second),true),"total mismatch header send");
    require(until([&]{return !raw.wire.Error().empty();}),"inconsistent blob total accepted");
}
void blobQueueCap(){
    RawPeer raw;for(std::uint32_t id=0;id<3;id++){std::vector<std::uint8_t> body(1,(std::uint8_t)id);sendRawBlob(raw,blobHeader(id,1,0,1,body),body);}
    require(until([&]{return !raw.wire.Error().empty();}),"blob completed queue exceeded cap");
}
void blobLocalValidation(){
    Pair p;std::vector<std::uint8_t> empty,tooLarge(blobMaxBytes+1,0);require(!p.a.SendBlob(0,empty),"empty blob accepted");require(!p.a.SendBlob(0,tooLarge),"oversize blob accepted");require(p.a.Error().empty(),"local size rejection poisoned link");
}
void blobLocalCancelPartial(){
    RawPeer raw;std::vector<std::uint8_t> body(blobChunkBytes+1,0x42),out={9,8,7};BlobHeader h=blobHeader(0,(std::uint32_t)body.size(),0,blobChunkBytes,body);
    require(bytes(raw.peer,&h,sizeof(h),true),"partial blob header send");require(bytes(raw.peer,body.data(),64,true),"partial blob bytes send");
    require(raw.wire.RequestStop(),"local STOP during partial blob");require(raw.wire.TryReceiveBlob(0,out)==-1,"local STOP returned incomplete blob");require(out==std::vector<std::uint8_t>({9,8,7}),"local STOP modified output");require(raw.wire.Error().empty(),"local cancellation became failure");
}
void blobRemoteStop(){
    RawPeer raw;std::vector<std::uint8_t> out={4,5};Frame stop={0x31504f53,0,{},0};require(bytes(raw.peer,&stop,sizeof(stop),true),"remote STOP send");
    require(until([&]{return raw.wire.PeerStopped();}),"remote STOP not observed");require(raw.wire.TryReceiveBlob(0,out)==-1&&out==std::vector<std::uint8_t>({4,5}),"remote STOP delivered blob or changed output");
}
void blobEofPartial(){
    RawPeer raw;std::vector<std::uint8_t> body(blobChunkBytes+1,0x22),out={1,3};BlobHeader h=blobHeader(0,(std::uint32_t)body.size(),0,blobChunkBytes,body);
    require(bytes(raw.peer,&h,sizeof(h),true)&&bytes(raw.peer,body.data(),64,true),"partial EOF fixture send");shutdown(raw.peer,SD_SEND);closesocket(raw.peer);raw.peer=INVALID_SOCKET;
    require(until([&]{return !raw.wire.Error().empty();}),"partial blob EOF accepted");require(raw.wire.TryReceiveBlob(0,out)==-1&&out==std::vector<std::uint8_t>({1,3}),"partial EOF delivered or changed output");
}
void blobSendBackpressureCancel(){
    RawPeer raw(10000);int requested=2048;
    require(setsockopt(raw.peer,SOL_SOCKET,SO_RCVBUF,(const char*)&requested,sizeof(requested))==0,"small receive window setup");
    int receiveBuffer=0;int optionSize=sizeof(receiveBuffer);require(getsockopt(raw.peer,SOL_SOCKET,SO_RCVBUF,(char*)&receiveBuffer,&optionSize)==0&&receiveBuffer>0,"small receive window query");
    std::vector<std::uint8_t> body(blobMaxBytes,0x5c),out={3,1,4};auto sender=std::async(std::launch::async,[&]{return raw.wire.SendBlob(0,body);});
    bool queued=false;ULONGLONG queueDeadline=GetTickCount64()+5000;
    while(GetTickCount64()<queueDeadline){u_long pending=0;if(ioctlsocket(raw.peer,FIONREAD,&pending)==0&&pending>=(u_long)(receiveBuffer/2)){queued=true;break;}Sleep(1);}
    if(!queued||sender.wait_for(std::chrono::milliseconds(0))==std::future_status::ready){raw.wire.Close();require(false,"large send did not reach controlled backpressure");}
    auto stopStarted=GetTickCount64();auto stopper=std::async(std::launch::async,[&]{return raw.wire.RequestStop();});
    require(until([&]{return raw.wire.TryReceiveBlob(0,out)==-1;} )&&out==std::vector<std::uint8_t>({3,1,4}),"stop intent not published before blocked writer");
    std::uint32_t magic=0,offset=0,total=0;bool sawPartial=false,sawStop=false;
    for(unsigned packets=0;packets<10000;packets++){
        require(bytes(raw.peer,&magic,sizeof(magic),false),"backpressure drain packet magic");
        if(magic==0x31424c42){BlobHeader h={};h.magic=magic;require(bytes(raw.peer,(char*)&h+sizeof(magic),sizeof(h)-sizeof(magic),false),"backpressure drain blob header");
            require(h.version==1&&!h.reserved&&h.id==0&&h.total==blobMaxBytes&&h.offset==offset&&h.length>0&&h.length<=blobChunkBytes,"backpressure drain blob sequence");
            if(!total)total=h.total;require(total==h.total,"backpressure total changed");char chunk[blobChunkBytes];require(bytes(raw.peer,chunk,(int)h.length,false),"backpressure drain blob data");
            offset+=h.length;if(offset<total)sawPartial=true;continue;}
        if(magic==0x31504f53){Frame stop={};stop.magic=magic;require(bytes(raw.peer,(char*)&stop+sizeof(magic),sizeof(stop)-sizeof(magic),false),"backpressure drain STOP body");
            require(stop.tick==0&&stop.state==0&&stop.reserved==0&&stop.pointer.x==0&&stop.pointer.y==0&&stop.pointer.flags==0,"backpressure STOP payload");
            for(float value:stop.actions)require(value==0,"backpressure STOP actions");sawStop=true;break;}
        require(false,"unexpected packet in backpressured stream");
    }
    require(sawStop&&sawPartial&&offset<total,"STOP did not end an incomplete blob at chunk boundary");
    Frame peerStop={0x31504f53,0,{},0};require(bytes(raw.peer,&peerStop,sizeof(peerStop),true),"backpressure peer STOP reply");
    require(stopper.get(),"bounded RequestStop failed");require(!sender.get(),"canceled blob sender reported success");
    require(GetTickCount64()-stopStarted<5000,"RequestStop/send cancellation exceeded bound");
    require(until([&]{return raw.wire.PeerStopped();})&&raw.wire.Error().empty(),"backpressure cancellation became transport failure");
}
}
int main(){
    setvbuf(stdout,0,_IONBF,0);
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
        {"phase opaque state and independent gameplay sequence",phaseAndInput},
        {"phase remote STOP before first Step",phaseRemoteStop},
        {"phase local STOP before first Step",phaseLocalStop},
        {"phase EOF failure",phaseDisconnect},{"phase timeout failure",phaseTimeout},
        {"phase invalid kind",[]{badPhase(0);}},{"phase invalid flags",[]{badPhase(1);}},
        {"phase nonzero reserved",[]{badPhase(2);}},{"phase out of order",[]{badPhase(3);}},
        {"phase duplicate rejection",[]{badPhase(4);}},{"phase queue bound",[]{badPhase(5);}},
        {"phase invalid local flags",invalidLocalPhase},{"phase invalid local sequence",invalidLocalPhaseSequence},
        {"phase wait FIN wake",phaseFinWake},{"phase queued before FIN x20",phaseThenFin},
        {"idle longer than API timeout then exchange",idleThenExchange},
        {"partial packet bounded phase wait",[]{partialFrameTimeout(0);}},
        {"partial packet bounded Step wait",[]{partialFrameTimeout(1);}},
        {"partial packet bounded Finish wait",[]{partialFrameTimeout(2);}},
        {"invalid local pointer bounds/flags/inactive",invalidLocalPointer},
        {"remote pointer x overflow",[]{invalidRemotePointer(0);}},
        {"remote pointer y overflow",[]{invalidRemotePointer(1);}},
        {"remote pointer reserved flag",[]{invalidRemotePointer(2);}},
        {"remote pointer click without active",[]{invalidRemotePointer(3);}},
        {"remote inactive pointer coordinates",[]{invalidRemotePointer(4);}},
        {"remote frame reserved field",[]{invalidRemotePointer(5);}},
        {"STOP nonzero pointer",[]{invalidRemotePointer(6);}},
        {"FIN nonzero pointer",[]{invalidRemotePointer(7);}},
        {"protocol5 rejection",[]{oldProtocol(5);}}, {"protocol4 rejection",[]{oldProtocol(4);}},
        {"protocol2 rejection",[]{oldProtocol(2);}},{"protocol3 rejection",[]{oldProtocol(3);}},
        {"blob malformed version",[]{malformedBlob(0);}}, {"blob reserved field",[]{malformedBlob(1);}},
        {"blob ID order",[]{malformedBlob(2);}}, {"blob zero total",[]{malformedBlob(3);}},
        {"blob total cap",[]{malformedBlob(4);}}, {"blob initial offset",[]{malformedBlob(5);}},
        {"blob zero chunk",[]{malformedBlob(6);}}, {"blob chunk cap",[]{malformedBlob(7);}},
        {"blob header checksum",[]{malformedBlob(8);}}, {"blob payload checksum",[]{malformedBlob(9);}},
        {"blob inter-chunk offset",blobChunkOffsetMismatch}, {"blob inter-chunk total",blobChunkTotalMismatch},
        {"blob completed queue cap",blobQueueCap},
        {"blob local empty/oversize reject",blobLocalValidation}, {"blob local STOP partial",blobLocalCancelPartial},
        {"blob remote STOP",blobRemoteStop}, {"blob EOF partial",blobEofPartial},
        {"blob backpressured send STOP cancellation",blobSendBackpressureCancel}
    };
    unsigned failures=0;for(const auto& test:tests){try{test.run();printf("PASS %s\n",test.name);}catch(const std::exception& e){++failures;printf("FAIL %s: %s\n",test.name,e.what());}}
    WSACleanup();printf("WIRE_STOP_SELFTEST %u/%u passed\n",unsigned(sizeof(tests)/sizeof(tests[0]))-failures,unsigned(sizeof(tests)/sizeof(tests[0])));return failures?1:0;
}
