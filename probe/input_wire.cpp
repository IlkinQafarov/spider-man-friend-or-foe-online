#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "input_wire.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#pragma comment(lib,"ws2_32.lib")

namespace coop {
namespace {
constexpr std::uint32_t helloMagic=0x314f4f43,frameMagic=0x314d5246,finishMagic=0x314e4946,stopMagic=0x31504f53,phaseMagic=0x31534850;
struct Hello {std::uint32_t magic,version,role,delay;Digest build,scenario;};
struct Frame {std::uint32_t magic,tick;Actions actions;std::uint64_t state;MenuPointer pointer;std::uint32_t reserved;};
struct PhasePacket {std::uint32_t magic,sequence;PhaseState state;std::uint64_t reserved;};
static_assert(sizeof(Hello)==80,"wire hello size");
static_assert(sizeof(Frame)==144,"wire frame size");
static_assert(sizeof(PhasePacket)==128,"wire phase packet size");
bool valid(const Actions& actions){for(float value:actions)if(!std::isfinite(value)||value<0||value>1)return false;return true;}
bool valid(const MenuPointer& pointer){return pointer.x<=65535&&pointer.y<=65535&&!(pointer.flags&~(menuPointerActive|menuPointerClick))&&
    ((pointer.flags&menuPointerActive)||(pointer.x==0&&pointer.y==0&&pointer.flags==0));}
bool valid(const PhaseState& state){return (state.kind==phaseMenu||state.kind==phaseLoad)&&!(state.flags&~(phaseReady|phaseConfirmed));}
bool transfer(SOCKET socket,void* bytes,int count,bool sending){
    char* position=(char*)bytes;
    while(count){int done=sending?send(socket,position,count,0):recv(socket,position,count,0);if(done<=0)return false;position+=done;count-=done;}
    return true;
}
}
struct InputWire::Impl {
    SOCKET socket=INVALID_SOCKET;
    bool wsa=false;
    std::atomic<unsigned> delay{3};unsigned timeoutMs=15000;std::atomic<bool> framesSeen{false};
    std::uint32_t outgoing=0;
    std::uint32_t phaseOutgoing=0;
    std::atomic<std::uint32_t> consumed{0};
    std::atomic<std::uint32_t> phaseConsumed{0};
    std::atomic<bool> running{false};
    std::atomic<bool> localStopped{false},peerStopped{false};
    // Serialize the frame/marker bytes and outgoing sequence as one operation.
    std::mutex sendMutex;
    std::thread reader;
    mutable std::mutex errorMutex;
    std::string error;
    std::mutex queueMutex;
    std::condition_variable ready;
    std::map<std::uint32_t,Frame> queue;
    std::map<std::uint32_t,PhaseState> phaseQueue;
    std::atomic<bool> peerFinished{false},ownFinished{false};
    bool Fail(const char* message){
        {std::lock_guard<std::mutex> lock(errorMutex);if(error.empty())error=message;}
        {std::lock_guard<std::mutex> lock(queueMutex);running=false;}
        if(socket!=INVALID_SOCKET)shutdown(socket,SD_BOTH);ready.notify_all();return false;
    }
    bool ReceiveBytes(void* destination,int remaining){
        char* position=(char*)destination;
        while(remaining&&running){
            // A local Winsock shutdown need not release an already blocked recv
            // immediately. Poll readiness (no network traffic), then receive only
            // available bytes. Both idle and partial packets stay cancellable.
            fd_set readable;FD_ZERO(&readable);FD_SET(socket,&readable);timeval interval={0,100000};
            int available=select(0,&readable,0,0,&interval);if(available==SOCKET_ERROR)return false;
            if(!available)continue;if(!running)return false;
            int received=recv(socket,position,remaining,0);if(received<=0)return false;
            position+=received;remaining-=received;
        }
        return remaining==0;
    }
    bool ReceiveFrame(Frame& frame){
        // Phase packets remain 128 bytes; input/terminal packets are 144. Read
        // the discriminator first so adjacent mixed packet types never overlap.
        if(!ReceiveBytes(&frame.magic,sizeof(frame.magic)))return false;
        if(frame.magic!=phaseMagic&&frame.magic!=frameMagic&&frame.magic!=stopMagic&&frame.magic!=finishMagic)return false;
        int size=frame.magic==phaseMagic?sizeof(PhasePacket):sizeof(Frame);
        return ReceiveBytes((char*)&frame+sizeof(frame.magic),size-sizeof(frame.magic));
    }
    void Read(){
        // The input delay may be set once before the first frame (SetDelay); read it when the first frame arrives.
        std::uint32_t expected=~0u,phaseExpected=0;
        while(running){
            Frame frame={};if(!ReceiveFrame(frame)){if(running&&!localStopped)Fail("Peer disconnected or receive failed");break;}
            if(frame.magic==phaseMagic){
                PhasePacket packet={};memcpy(&packet,&frame,sizeof(packet));
                if(packet.reserved||!valid(packet.state)||packet.sequence>0xffffff00u||packet.sequence!=phaseExpected||
                   std::uint64_t(packet.sequence)>std::uint64_t(phaseConsumed.load())+16){Fail("Invalid or out-of-order phase packet");break;}
                bool queued=false;{std::lock_guard<std::mutex> lock(queueMutex);if(phaseQueue.size()<16)queued=phaseQueue.emplace(packet.sequence,packet.state).second;}
                if(!queued){Fail("Phase queue limit or duplicate phase packet");break;}
                ++phaseExpected;ready.notify_all();continue;
            }
            if(frame.magic!=phaseMagic&&expected==~0u)expected=delay.load();
            if(frame.magic==stopMagic){
                bool empty=frame.state==0&&frame.pointer.x==0&&frame.pointer.y==0&&frame.pointer.flags==0&&frame.reserved==0;for(float value:frame.actions)if(value!=0)empty=false;
                if(frame.tick>0xffffff00u||frame.tick+delay!=expected||!empty){Fail("Invalid stop sequence or payload");break;}
                {std::lock_guard<std::mutex> lock(queueMutex);peerStopped=true;}
                ready.notify_all();return;
            }
            if(frame.magic==finishMagic){
                bool empty=frame.state==0&&frame.pointer.x==0&&frame.pointer.y==0&&frame.pointer.flags==0&&frame.reserved==0;for(float value:frame.actions)if(value!=0)empty=false;
                if(frame.tick+delay!=expected||!empty){Fail("Invalid finish sequence or payload");break;}
                {std::lock_guard<std::mutex> lock(queueMutex);peerFinished=true;}
                ready.notify_all();return;
            }
            if(frame.magic!=frameMagic||frame.tick!=expected||!valid(frame.actions)||!valid(frame.pointer)||frame.reserved||frame.tick>consumed.load()+128){Fail("Invalid or out-of-order input frame");break;}
            framesSeen=true;{std::lock_guard<std::mutex> lock(queueMutex);queue.emplace(frame.tick,frame);}
            ++expected;ready.notify_all();
        }
    }
};
InputWire::InputWire():impl(new Impl){}
InputWire::~InputWire(){Close();}
void InputWire::Close(){
    impl->running=false;if(impl->socket!=INVALID_SOCKET)shutdown(impl->socket,SD_BOTH);impl->ready.notify_all();
    if(impl->reader.joinable())impl->reader.join();
    if(impl->socket!=INVALID_SOCKET){closesocket(impl->socket);impl->socket=INVALID_SOCKET;}
    if(impl->wsa){WSACleanup();impl->wsa=false;}
}
std::string InputWire::Error()const{std::lock_guard<std::mutex> lock(impl->errorMutex);return impl->error;}
void InputWire::SetTimeout(unsigned timeoutMs){if(impl&&timeoutMs>=100&&timeoutMs<=120000)impl->timeoutMs=timeoutMs;}
bool InputWire::Open(bool host,const char* ipv4,std::uint16_t port,const Digest& build,const Digest& scenario,unsigned delay,unsigned timeoutMs){
    Close();impl.reset(new Impl);impl->delay=delay;impl->timeoutMs=timeoutMs;
    if(!port||delay>16||timeoutMs<100||timeoutMs>120000)return impl->Fail("Invalid connection options");
    WSADATA data={};if(WSAStartup(MAKEWORD(2,2),&data))return impl->Fail("WSAStartup failed");impl->wsa=true;
    sockaddr_in address={};address.sin_family=AF_INET;address.sin_port=htons(port);
    if(!ipv4||InetPtonA(AF_INET,ipv4,&address.sin_addr)!=1)return impl->Fail("An IPv4 address is required");
    if(host){
        SOCKET listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(listener==INVALID_SOCKET)return impl->Fail("Listener creation failed");
        BOOL exclusive=TRUE;setsockopt(listener,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,(char*)&exclusive,sizeof(exclusive));
        bool ok=bind(listener,(sockaddr*)&address,sizeof(address))!=SOCKET_ERROR&&listen(listener,1)!=SOCKET_ERROR;
        if(ok){fd_set readable;FD_ZERO(&readable);FD_SET(listener,&readable);timeval timeout={(long)(timeoutMs/1000),(long)(timeoutMs%1000*1000)};ok=select(0,&readable,0,0,&timeout)>0;}
        if(ok)impl->socket=accept(listener,0,0);closesocket(listener);
        if(!ok||impl->socket==INVALID_SOCKET)return impl->Fail("Host accept failed or timed out");
    }else{
        ULONGLONG deadline=GetTickCount64()+timeoutMs;
        do{
            SOCKET peer=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(peer==INVALID_SOCKET)return impl->Fail("Client socket creation failed");
            u_long nonblocking=1;ioctlsocket(peer,FIONBIO,&nonblocking);int result=connect(peer,(sockaddr*)&address,sizeof(address));
            bool connected=result==0;
            if(!connected&&WSAGetLastError()==WSAEWOULDBLOCK){
                fd_set writable,failed;FD_ZERO(&writable);FD_ZERO(&failed);FD_SET(peer,&writable);FD_SET(peer,&failed);timeval timeout={0,100000};
                if(select(0,0,&writable,&failed,&timeout)>0&&FD_ISSET(peer,&writable)){int error=0,length=sizeof(error);connected=getsockopt(peer,SOL_SOCKET,SO_ERROR,(char*)&error,&length)==0&&error==0;}
            }
            if(connected){nonblocking=0;ioctlsocket(peer,FIONBIO,&nonblocking);impl->socket=peer;break;}
            closesocket(peer);Sleep(20);
        }while(GetTickCount64()<deadline);
        if(impl->socket==INVALID_SOCKET)return impl->Fail("Client connect timed out");
    }
    BOOL noDelay=TRUE;DWORD timeout=timeoutMs;
    setsockopt(impl->socket,IPPROTO_TCP,TCP_NODELAY,(char*)&noDelay,sizeof(noDelay));
    setsockopt(impl->socket,SOL_SOCKET,SO_RCVTIMEO,(char*)&timeout,sizeof(timeout));
    setsockopt(impl->socket,SOL_SOCKET,SO_SNDTIMEO,(char*)&timeout,sizeof(timeout));
    Hello local={helloMagic,5,host?0u:1u,delay,build,scenario},peer={};
    if(!transfer(impl->socket,&local,sizeof(local),true)||!transfer(impl->socket,&peer,sizeof(peer),false))return impl->Fail("Handshake transfer failed");
    if(peer.magic!=helloMagic||peer.version!=5||peer.role==local.role||peer.role>1||peer.delay!=delay||peer.build!=build||peer.scenario!=scenario)return impl->Fail("Build, scenario, role, or protocol mismatch");
    // Movies/modal screens may produce no packets while no API is waiting.
    // API waits remain bounded and Fail/Close shuts down a stalled reader.
    DWORD idleReceive=0;if(setsockopt(impl->socket,SOL_SOCKET,SO_RCVTIMEO,(char*)&idleReceive,sizeof(idleReceive))==SOCKET_ERROR)return impl->Fail("Receive idle configuration failed");
    impl->running=true;impl->reader=std::thread([this]{impl->Read();});return true;
}
bool InputWire::SetDelay(unsigned frames){
    std::lock_guard<std::mutex> sending(impl->sendMutex);
    if(!impl->running||frames<1||frames>16||impl->outgoing!=0||impl->framesSeen)return false;
    impl->delay=frames;return true;
}
bool InputWire::Step(std::uint32_t tick,const Actions& local,Actions& remote,std::uint64_t localState,std::uint64_t* remoteState,const MenuPointer* localPointer,MenuPointer* remotePointer){
    std::unique_lock<std::mutex> sending(impl->sendMutex);
    if(impl->localStopped||impl->peerStopped)return false;
    if(!impl->running)return false;
    if(impl->ownFinished)return impl->Fail("Input after finish");
    const MenuPointer pointer=localPointer?*localPointer:MenuPointer{};
    if(tick!=impl->outgoing||tick>0xffffff00u||!valid(local)||!valid(pointer))return impl->Fail("Invalid local input sequence");
    impl->consumed=tick;
    Frame frame={frameMagic,tick+impl->delay,local,localState,pointer,0};
    if(!transfer(impl->socket,&frame,sizeof(frame),true)){if(impl->peerStopped)return false;return impl->Fail("Input send failed");}++impl->outgoing;
    sending.unlock();
    if(tick<impl->delay){remote.fill(0);if(remoteState)*remoteState=0;if(remotePointer)*remotePointer={};return true;}
    std::unique_lock<std::mutex> lock(impl->queueMutex);
    bool available=impl->ready.wait_for(lock,std::chrono::milliseconds(impl->timeoutMs),[&]{return !impl->running||impl->localStopped||impl->peerStopped||impl->queue.count(tick)!=0;});
    if(!available){lock.unlock();return impl->Fail("Input frame timed out");}
    if(!impl->running||impl->localStopped||impl->peerStopped)return false;
    auto found=impl->queue.find(tick);if(found==impl->queue.end())return false;
    remote=found->second.actions;if(remoteState)*remoteState=found->second.state;if(remotePointer)*remotePointer=found->second.pointer;impl->queue.erase(found);return true;
}
bool InputWire::ExchangePhase(std::uint32_t sequence,const PhaseState& local,PhaseState& remote){
    std::unique_lock<std::mutex> sending(impl->sendMutex);
    if(!impl->running||impl->localStopped||impl->peerStopped||impl->peerFinished)return false;
    if(impl->ownFinished)return impl->Fail("Phase exchange after finish");
    if(sequence!=impl->phaseOutgoing||sequence>0xffffff00u||!valid(local))return impl->Fail("Invalid local phase sequence or state");
    impl->phaseConsumed=sequence;
    PhasePacket packet={phaseMagic,sequence,local,0};
    if(!transfer(impl->socket,&packet,sizeof(packet),true)){if(impl->peerStopped)return false;return impl->Fail("Phase send failed");}
    ++impl->phaseOutgoing;sending.unlock();
    std::unique_lock<std::mutex> lock(impl->queueMutex);
    bool available=impl->ready.wait_for(lock,std::chrono::milliseconds(impl->timeoutMs),[&]{return !impl->running||impl->localStopped||impl->peerStopped||impl->ownFinished||impl->peerFinished||impl->phaseQueue.count(sequence)!=0;});
    if(!available){lock.unlock();return impl->Fail("Phase exchange timed out");}
    // The peer may send this phase and then FIN back to back; a phase already
    // queued before the terminal marker is still a completed exchange.
    auto found=impl->phaseQueue.find(sequence);
    if(found!=impl->phaseQueue.end()&&impl->running&&!impl->localStopped){remote=found->second;impl->phaseQueue.erase(found);return true;}
    return false;
}
bool InputWire::SendPhase(std::uint32_t sequence,const PhaseState& local){
    std::unique_lock<std::mutex> sending(impl->sendMutex);
    if(!impl->running||impl->localStopped||impl->peerStopped||impl->peerFinished)return false;
    if(impl->ownFinished)return impl->Fail("Phase exchange after finish");
    if(sequence!=impl->phaseOutgoing||sequence>0xffffff00u||!valid(local))return impl->Fail("Invalid local phase sequence or state");
    impl->phaseConsumed=sequence;
    PhasePacket packet={phaseMagic,sequence,local,0};
    if(!transfer(impl->socket,&packet,sizeof(packet),true)){if(impl->peerStopped)return false;return impl->Fail("Phase send failed");}
    ++impl->phaseOutgoing;return true;
}
int InputWire::TryReceivePhase(std::uint32_t sequence,PhaseState& remote){
    std::lock_guard<std::mutex> lock(impl->queueMutex);
    auto found=impl->phaseQueue.find(sequence);
    if(found!=impl->phaseQueue.end()&&impl->running&&!impl->localStopped){remote=found->second;impl->phaseQueue.erase(found);return 1;}
    if(!impl->running||impl->localStopped||impl->peerStopped||impl->ownFinished||impl->peerFinished)return -1;
    return 0;
}
bool InputWire::PhaseAvailable(std::uint32_t sequence) const{
    if(!impl)return false;std::lock_guard<std::mutex> lock(impl->queueMutex);return impl->phaseQueue.count(sequence)!=0;
}
bool InputWire::Finish(){
    std::unique_lock<std::mutex> sending(impl->sendMutex);
    if(impl->localStopped||impl->peerStopped)return false;
    if(!impl->running||impl->ownFinished)return false;
    Frame marker={finishMagic,impl->outgoing,{},0};
    if(!transfer(impl->socket,&marker,sizeof(marker),true))return impl->Fail("Finish send failed");
    {std::lock_guard<std::mutex> lock(impl->queueMutex);impl->ownFinished=true;}
    impl->ready.notify_all();
    sending.unlock();
    std::unique_lock<std::mutex> lock(impl->queueMutex);
    bool notified=impl->ready.wait_for(lock,std::chrono::milliseconds(impl->timeoutMs),[&]{return impl->peerFinished||!impl->running||impl->localStopped||impl->peerStopped;});
    if(!notified){lock.unlock();return impl->Fail("Peer finish timed out");}
    return impl->peerFinished;
}
bool InputWire::RequestStop(){
    std::lock_guard<std::mutex> sending(impl->sendMutex);
    if(!Error().empty())return false;
    if(impl->localStopped)return true;
    if(!impl->running||impl->ownFinished)return false;
    // Publish before sending: an immediate peer close may race the receive loop.
    {std::lock_guard<std::mutex> lock(impl->queueMutex);impl->localStopped=true;}
    impl->ready.notify_all();
    Frame marker={stopMagic,impl->outgoing,{},0};
    if(!transfer(impl->socket,&marker,sizeof(marker),true))return impl->Fail("Stop send failed");
    // Queue an orderly FIN behind the terminal marker before the window exits.
    if(shutdown(impl->socket,SD_SEND)==SOCKET_ERROR&&!impl->peerStopped)return impl->Fail("Stop send shutdown failed");
    return Error().empty();
}
bool InputWire::PeerStopped()const{return impl->peerStopped&&Error().empty();}
}
