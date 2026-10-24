#include "relay.hpp"
#include <cstring>

namespace fof {
namespace {
constexpr Id Lobby=480000123456ULL;
void check(bool condition,const char* message) { if(!condition) throw Failure(message); }
template<class F> void mustFail(F&& test,const char* message) {
    bool failed=false; try { test(); } catch(const Failure&) { failed=true; }
    check(failed,message);
}
struct Hub {
    std::array<std::deque<Message>,2> mail;
    std::string tag;
    std::vector<Id> members{101,202};
    bool blocked=false;
    unsigned sent=0,accepted=0,closed=0;
};
class Memory final:public Backend {
    Hub& hub_; std::size_t index_; bool ready_=false;
public:
    std::vector<Event> extra;
    Memory(Hub& hub,std::size_t index):hub_(hub),index_(index) { me=index?202:101; }
    const char* kind() const override { return "own-memory-backend-no-account"; }
    void start(bool host,Id lobby) override { check(host==(index_==0) && (host || lobby==Lobby),"Fixture role mismatch"); ready_=true; }
    std::vector<Event> events() override {
        auto result=std::move(extra); extra.clear();
        if(ready_) { result.push_back({Event::Type::Lobby,0,Lobby,true}); ready_=false; } return result;
    }
    void setTag(Id lobby,const char* value) override { check(lobby==Lobby,"Fixture lobby"); hub_.tag=value; }
    std::string tag(Id lobby) override { check(lobby==Lobby,"Fixture tag lobby"); return hub_.tag; }
    Id owner(Id) override { return 101; }
    std::vector<Id> members(Id) override { return hub_.members; }
    bool accept(Id) override { ++hub_.accepted; return true; }
    bool send(Id user,const Bytes& data) override {
        check(user==(index_?101ULL:202ULL),"Fixture send identity");
        if(hub_.blocked) return false;
        check(hub_.mail[1-index_].size()<128,"Fixture message queue limit");
        hub_.mail[1-index_].push_back({me,data}); ++hub_.sent; return true;
    }
    std::vector<Message> messages() override {
        std::vector<Message> result;
        auto& queue=hub_.mail[index_];
        for(unsigned i=0;i<32 && !queue.empty();++i) { result.push_back(std::move(queue.front())); queue.pop_front(); }
        return result;
    }
    void close(Id,Id) noexcept override { ++hub_.closed; }
};
Socket connectGame(unsigned short port) {
    Socket socket(::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP)); check(static_cast<bool>(socket),"Fixture socket create");
    sockaddr_in address{}; address.sin_family=AF_INET; address.sin_port=htons(port); address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(connect(socket.get(),reinterpret_cast<sockaddr*>(&address),sizeof(address))) throw Failure("Fixture loopback connect failed (WSA="+std::to_string(WSAGetLastError())+")");
    nonblocking(socket.get()); return socket;
}
void appendReceived(SOCKET socket,Bytes& output,bool& eof) {
    std::array<unsigned char,4096> buffer{};
    const int count=recv(socket,reinterpret_cast<char*>(buffer.data()),static_cast<int>(buffer.size()),0);
    if(count>0) output.insert(output.end(),buffer.begin(),buffer.begin()+count);
    else if(count==0) eof=true;
    else check(WSAGetLastError()==WSAEWOULDBLOCK,"Fixture receive failed");
}
void writeSome(SOCKET socket,const Bytes& data,std::size_t& sent,bool& ended,bool halfCloseWhenComplete=true) {
    if(sent<data.size()) {
        // Fragment writes deliberately, so DATA packet boundaries differ from TCP reads.
        const int count=send(socket,reinterpret_cast<const char*>(data.data()+sent),static_cast<int>(std::min<std::size_t>(997,data.size()-sent)),0);
        if(count>0) sent+=static_cast<std::size_t>(count);
        else check(count==SOCKET_ERROR && WSAGetLastError()==WSAEWOULDBLOCK,"Fixture send failed");
    }
    if(halfCloseWhenComplete && sent==data.size() && !ended) { check(shutdown(socket,SD_SEND)==0,"Fixture half close"); ended=true; }
}
Bytes pattern(unsigned char key) {
    Bytes result(32768); for(std::size_t i=0;i<result.size();++i) result[i]=static_cast<unsigned char>((i*37+key)&255); return result;
}
void transferTest(bool resetJoin) {
    Hub hub; Memory hostApi(hub,0),joinApi(hub,1);
    Socket gameListener=listenLoopback(0);
    const auto hostPort=socketPort(gameListener.get());
    Relay host(hostApi,true,hostPort),join(joinApi,false,0);
    host.start(); host.step(); join.start(Lobby); join.step();
    const auto joinPort=join.listenPort();
    check(joinPort && hostPort!=49375 && hostPort!=49376 && hostPort!=49480 && joinPort!=49375 && joinPort!=49376 && joinPort!=49480,"Fixture unexpectedly chose a reserved test port");
    Socket joinGame=connectGame(joinPort),hostGame;
    const Bytes toHost=pattern(31),toJoin=pattern(197);
    Bytes hostRead,joinRead;
    std::size_t hostSent=0,joinSent=0;
    bool hostEnded=false,joinEnded=false,hostEof=false,joinEof=false;
    bool hostDone=false,joinDone=false,resetIssued=false;
    const auto deadline=GetTickCount64()+10000;
    while(GetTickCount64()<deadline && !(hostDone && joinDone)) {
        const auto now=GetTickCount64();
        if(!hostDone) hostDone=host.step(now,30);
        if(!joinDone) joinDone=join.step(now,30);
        if(!hostGame) {
            const SOCKET accepted=accept(gameListener.get(),nullptr,nullptr);
            if(accepted!=INVALID_SOCKET) { hostGame.reset(accepted); nonblocking(hostGame.get()); gameListener.reset(); }
            else check(WSAGetLastError()==WSAEWOULDBLOCK,"Fixture host accept");
        }
        if(joinGame) {
            writeSome(joinGame.get(),toHost,joinSent,joinEnded,!resetJoin);
            if(!resetJoin) appendReceived(joinGame.get(),joinRead,joinEof);
        }
        if(hostGame) {
            appendReceived(hostGame.get(),hostRead,hostEof);
            if(resetJoin && !resetIssued && hostRead==toHost) {
                // Abort the local join game before host sends return data.
                linger reset{1,0};
                check(setsockopt(joinGame.get(),SOL_SOCKET,SO_LINGER,reinterpret_cast<const char*>(&reset),sizeof(reset))==0,"Fixture reset option");
                joinGame.reset(); resetIssued=true;
            }
            if(!resetJoin || (resetIssued && join.resetSeen())) writeSome(hostGame.get(),toJoin,hostSent,hostEnded);
        }
        Sleep(1);
    }
    check(hostDone && joinDone,"Fixture relay did not finish close/ack handshake");
    check(hostRead==toHost && hostEof,"Fixture bytes/EOF toward host differ");
    check(host.tx==toJoin.size() && host.rx==toHost.size() && join.tx==toHost.size() && join.rx==toJoin.size(),"Fixture byte offsets differ");
    if(resetJoin) check(join.resetSeen() && join.discarded==toJoin.size(),"Fixture did not account for data after local reset");
    else check(joinRead==toJoin && joinEof && !host.discarded && !join.discarded,"Fixture bytes/EOF toward join differ");
    check(host.handshakeComplete() && join.handshakeComplete(),"Fixture close/ack incomplete");
    host.close(); join.close(); check(hub.closed==2,"Fixture backend cleanup mismatch");
    log(resetJoin?"PROTOCOL_RESET_PASS":"PROTOCOL_LOOPBACK_PASS","\"bytes_each_direction\":32768,\"host_port\":"+std::to_string(hostPort)+",\"join_port\":"+std::to_string(joinPort));
}
void validationTests() {
    const auto original=encode(Kind::Data,0x123456789abcdef0ULL,Bytes{1,2,3});
    const auto decoded=decode(original);
    check(decoded.offset==0x123456789abcdef0ULL && original[8]==0xf0 && original[15]==0x12 && decoded.data==Bytes{1,2,3},"Protocol byte offsets/endian mismatch");
    for(std::size_t position:{std::size_t(0),std::size_t(5),std::size_t(6),std::size_t(7)}) {
        auto invalid=original; invalid[position]^=0x7f;
        mustFail([&]{decode(invalid);},"Malformed protocol header accepted");
    }
    mustFail([]{decode(encode(Kind::Data));},"Empty data accepted");
    mustFail([]{decode(encode(Kind::Ack,0,Bytes{1}));},"Control payload accepted");
    mustFail([]{decode(Bytes(15));},"Short frame accepted");
    mustFail([]{decode(encode(Kind::Data,0,Bytes(MaxChunk+1)));},"Oversized frame accepted");
    {
        Hub hub; Memory api(hub,1); Relay relay(api,false,0); relay.start(Lobby);
        mustFail([&]{relay.step();},"Wrong lobby tag accepted");
    }
    {
        // The joiner's mode must match the host's; a host on an older add-on version (plain tag) is told apart.
        auto reason=[](const char* hostTag,const char* mode) {
            Hub hub; hub.tag=hostTag; Memory api(hub,1); Relay relay(api,false,0); relay.setMode(mode); relay.start(Lobby);
            try { relay.step(); } catch(const Failure& error) { return std::string(error.what()); }
            return std::string("accepted");
        };
        check(reason("fof-native-prototype-v1;mode=together","hub")=="PLAY_MODE_MISMATCH host=together","Mode mismatch not reported");
        check(reason("fof-native-prototype-v1;mode=hub","together")=="PLAY_MODE_MISMATCH host=hub","Reverse mode mismatch not reported");
        check(reason("fof-native-prototype-v1","hub")=="ADDON_VERSION_MISMATCH","Older host version not reported");
        check(reason("fof-native-prototype-v1;mode=hub","hub")=="accepted","Matching mode refused");
    }
    {
        // The host announces its partner only after the partner's relay has spoken.
        Hub hub; Memory api(hub,0); Relay relay(api,true,0); relay.setMode("hub"); relay.start(); relay.step();
        check(hub.tag=="fof-native-prototype-v1;mode=hub" && relay.peer==202 && !relay.announced(),"Host announced a silent partner");
        hub.mail[0].push_back({202,encode(Kind::Ping)}); relay.step();
        check(relay.announced(),"Host did not announce a partner that spoke");
    }
    {
        Hub hub; Memory api(hub,0); Relay relay(api,true,0); relay.start(); relay.step();
        api.extra.push_back({Event::Type::Request,999,0,true});
        hub.mail[0].push_back({999,Bytes{0}}); relay.step();
        check(relay.rejected==2 && !hub.accepted,"Nonmember session/message accepted");
        api.extra.push_back({Event::Type::Request,202,0,true}); relay.step(); check(hub.accepted==1,"Member session rejected");
        api.extra.push_back({Event::Type::Lobby,0,Lobby,true}); relay.step();
        hub.mail[0].push_back({202,encode(Kind::Open)}); relay.step();
        hub.mail[0].push_back({202,encode(Kind::Data,1,Bytes{1})});
        mustFail([&]{relay.step();},"Out-of-order stream data accepted");
    }
    {
        Hub hub; Memory api(hub,0); Relay relay(api,true,0); relay.start(); relay.step();
        hub.mail[0].push_back({202,encode(Kind::Open)}); relay.step();
        hub.mail[0].push_back({202,encode(Kind::Open)});
        mustFail([&]{relay.step();},"Repeated stream open accepted");
    }
    {
        Hub hub; Memory api(hub,0); Relay relay(api,true,0); relay.start(); relay.step();
        hub.mail[0].push_back({202,encode(Kind::Close)});
        mustFail([&]{relay.step();},"Premature stream close accepted");
    }
    {
        Hub hub; Memory api(hub,0); Relay relay(api,true,0); relay.start(); relay.step();
        hub.mail[0].push_back({202,encode(Kind::Ack)});
        mustFail([&]{relay.step();},"Unsolicited close acknowledgement accepted");
    }
    {
        Hub hub; Memory api(hub,0); Relay relay(api,true,0); relay.start(); relay.step();
        hub.mail[0].push_back({202,encode(Kind::Open)}); relay.step();
        for(unsigned i=0;i<64;++i) { hub.mail[0].push_back({202,encode(Kind::Data,static_cast<Id>(i)*MaxChunk,Bytes(MaxChunk,17))}); relay.step(); }
        hub.mail[0].push_back({202,encode(Kind::Data,MaxBuffer,Bytes{1})});
        mustFail([&]{relay.step();},"Receive buffer overflow accepted");
    }
    {
        Hub hub; hub.members={101,202,999}; Memory api(hub,0); Relay relay(api,true,0); relay.start();
        mustFail([&]{relay.step();},"Oversized lobby accepted");
    }
    {
        // Transient nil/repeated members (seen on real Steam) are skipped: the real peer is still found.
        Hub hub; hub.members={101,0,101,202}; Memory api(hub,0); Relay relay(api,true,0); relay.start(); relay.step();
        check(relay.peer==202 && relay.memberGaps_==2,"Transient lobby member gaps not skipped");
    }
    {
        Hub hub; hub.members={101,101}; Memory api(hub,0); Relay relay(api,true,0); relay.start(); relay.step();
        check(relay.peer==0,"Repeated own identity produced a peer");
    }
    {
        Hub hub; Memory api(hub,0); Relay relay(api,true,0); relay.start(); relay.step();
        api.extra.push_back({Event::Type::Left,202,0,true});
        mustFail([&]{relay.step();},"Peer lobby departure accepted");
    }
    {
        Hub hub; Memory api(hub,0); Relay relay(api,true,0); relay.start(); const auto now=GetTickCount64(); relay.step(now);
        mustFail([&]{relay.step(now+20001);},"Peer keep-alive timeout ignored");
    }
    log("PROTOCOL_VALIDATION_PASS","\"malformed_out_of_order_tag_membership_buffers_close_timeout\":true");
}
} // namespace
int protocolSelftest() {
    Winsock winsock;
    validationTests(); transferTest(false); transferTest(true);
    log("PROTOCOL_SELFTEST_PASS","\"loopback_only\":true,\"account_used\":false"); return 0;
}
} // namespace fof
