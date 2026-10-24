#include "relay.hpp"
#include <cstdio>
#include <limits>
#include <sstream>

namespace fof {
std::string jsonString(const std::string& value) {
    std::string out="\"";
    const char hex[]="0123456789abcdef";
    for(unsigned char c:value) {
        if(c=='"' || c=='\\') { out+='\\'; out+=static_cast<char>(c); }
        else if(c<32) { out+="\\u00"; out+=hex[c>>4]; out+=hex[c&15]; }
        else out+=static_cast<char>(c);
    }
    return out+'"';
}
void log(const std::string& event, const std::string& fields) {
    std::printf("{\"event\":%s%s%s}\n",jsonString(event).c_str(),fields.empty()?"":",",fields.c_str());
    std::fflush(stdout);
}
Bytes encode(Kind kind, Id offset, const Bytes& data) {
    Bytes result(HeaderSize+data.size());
    result[0]='F'; result[1]='O'; result[2]='F'; result[3]='1';
    result[4]=static_cast<unsigned char>(kind); result[5]=1;
    for(unsigned i=0;i<8;++i) result[8+i]=static_cast<unsigned char>(offset>>(8*i));
    std::copy(data.begin(),data.end(),result.begin()+HeaderSize);
    return result;
}
Frame decode(const Bytes& message) {
    if(message.size()<HeaderSize || message.size()>HeaderSize+MaxChunk) throw Failure("Invalid relay message length");
    if(message[0]!='F' || message[1]!='O' || message[2]!='F' || message[3]!='1' || message[5]!=1 || message[6] || message[7] || message[4]<1 || message[4]>5)
        throw Failure("Invalid relay header");
    Frame result{static_cast<Kind>(message[4]),0,{message.begin()+HeaderSize,message.end()}};
    for(unsigned i=0;i<8;++i) result.offset|=static_cast<Id>(message[8+i])<<(8*i);
    if((result.kind==Kind::Data && result.data.empty()) || (result.kind!=Kind::Data && !result.data.empty())) throw Failure("Invalid relay payload");
    return result;
}
Winsock::Winsock() {
    WSADATA data{};
    if(WSAStartup(MAKEWORD(2,2),&data)) throw Failure("Winsock initialization failed");
}
void nonblocking(SOCKET socket) {
    u_long value=1;
    if(ioctlsocket(socket,FIONBIO,&value)) throw Failure("Could not make loopback socket nonblocking");
    // Small game messages go out at once: with Nagle on this side, a reply waited for the game's delayed ACK
    // (up to ~200 ms per exchange; first two-PC test, 2026-10-24: hub frames ~285 ms apart).
    BOOL noDelay=TRUE; setsockopt(socket,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&noDelay),sizeof(noDelay));
}
Socket listenLoopback(unsigned short port) {
    Socket socket(::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP));
    if(!socket) throw Failure("Could not create loopback socket");
    const int exclusive=1;
    if(setsockopt(socket.get(),SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof(exclusive))) throw Failure("Could not reserve loopback port");
    sockaddr_in address{}; address.sin_family=AF_INET; address.sin_port=htons(port); address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(bind(socket.get(),reinterpret_cast<sockaddr*>(&address),sizeof(address)) || listen(socket.get(),1)) throw Failure("Could not listen on 127.0.0.1");
    nonblocking(socket.get());
    return socket;
}
unsigned short socketPort(SOCKET socket) {
    sockaddr_in address{}; int length=sizeof(address);
    if(getsockname(socket,reinterpret_cast<sockaddr*>(&address),&length)) throw Failure("Could not query local loopback port");
    return ntohs(address.sin_port);
}
static bool wouldBlock() { const int error=WSAGetLastError(); return error==WSAEWOULDBLOCK; }
static bool resetError() {
    const int error=WSAGetLastError();
    return error==WSAECONNRESET || error==WSAECONNABORTED || error==WSAESHUTDOWN || error==WSAENOTCONN;
}
std::vector<Id> Relay::checkedMembers() {
    // Steam can briefly report an empty (nil) or repeated member while someone joins, leaves or its client
    // reconnects (first two-PC test, 2026-10-24: "Invalid lobby member identity" right after PEER_READY ended both
    // sessions). Such entries are skipped; a player who really left is caught by the peer checks.
    std::vector<Id> list;
    for(Id member:api_.members(lobby)) {
        if(!member || std::find(list.begin(),list.end(),member)!=list.end()) {
            if(memberGaps_++<16) log("LOBBY_MEMBER_GAP_SKIPPED","\"member\":"+std::to_string(member));
            continue;
        }
        list.push_back(member);
    }
    if(list.size()>2) throw Failure("Expected a two-player lobby");
    return list;
}
void Relay::queue(Bytes message) {
    if(outgoing_.size()>=64 || outgoingBytes_+message.size()>MaxBuffer) throw Failure("Relay send buffer limit");
    outgoingBytes_+=message.size(); outgoing_.push_back(std::move(message));
}
void Relay::lobbyReady(Id id) {
    if(!id) throw Failure("Invalid zero lobby ID");
    if(lobby==id) { log("DUPLICATE_LOBBY_CALLBACK_IGNORED"); return; }
    if(lobby) throw Failure("Unexpected second lobby");
    if(!host_) {
        const std::string found=api_.tag(id), prefix=std::string(Tag)+";mode=";
        if(found!=lobbyTag()) {
            // A host with another mode, or another add-on version, is refused before any game data is exchanged.
            if(!mode_.empty() && found.rfind(prefix,0)==0) {
                std::string other; for(char c:found.substr(prefix.size())) if(c>='a' && c<='z' && other.size()<16) other+=c;
                throw Failure("PLAY_MODE_MISMATCH host="+other);
            }
            if(found==Tag || found.rfind(prefix,0)==0) throw Failure("ADDON_VERSION_MISMATCH");
            throw Failure("That lobby is not this Friend or Foe co-op prototype");
        }
    }
    lobby=id;
    if(host_) api_.setTag(lobby,lobbyTag().c_str());
    else listener_=listenLoopback(port_);
    log("LOBBY_READY","\"lobby\":"+std::to_string(lobby)+",\"host\":"+(host_?"true":"false")+",\"transport\":"+jsonString(api_.kind()));
    if(!readyFile_.empty()) {
        const HANDLE file=CreateFileW(readyFile_.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE) throw Failure("Could not exclusively create ready file");
        const auto text="{\"lobby\":"+std::to_string(lobby)+",\"transport\":"+jsonString(api_.kind())+"}\n";
        DWORD written=0;
        const bool ok=WriteFile(file,text.data(),static_cast<DWORD>(text.size()),&written,nullptr)!=FALSE && written==text.size();
        CloseHandle(file);
        if(!ok) throw Failure("Could not write relay ready file");
    }
}
void Relay::callbacks() {
    for(const auto& event:api_.events()) {
        switch(event.type) {
        case Event::Type::Lobby:
            if(!event.success) throw Failure("Steam lobby creation/join failed");
            lobbyReady(event.lobby); break;
        case Event::Type::Request: {
            const auto list=lobby?checkedMembers():std::vector<Id>{};
            if(event.user!=api_.me && std::find(list.begin(),list.end(),event.user)!=list.end() && (!peer || peer==event.user)) {
                if(!api_.accept(event.user)) throw Failure("Steam session acceptance failed");
                log("SESSION_ACCEPTED","\"peer\":"+std::to_string(event.user));
            } else { ++rejected; log("NONMEMBER_SESSION_REJECTED","\"peer\":"+std::to_string(event.user)); }
            break;
        }
        case Event::Type::Failed:
            if(event.user==peer) throw Failure("Steam peer session failed");
            break;
        case Event::Type::Left:
            if(event.user==peer && !handshakeComplete()) throw Failure("The other player left the Steam lobby");
            break;
        }
    }
}
void Relay::findPeer(std::uint64_t now) {
    if(!lobby) return;
    const auto list=checkedMembers();
    if(peer) {
        if(std::find(list.begin(),list.end(),peer)==list.end() && !handshakeComplete()) throw Failure("Peer is no longer a lobby member");
        return;
    }
    if(list.empty()) return;
    if(std::find(list.begin(),list.end(),api_.me)==list.end()) throw Failure("Local identity is not a lobby member");
    for(Id member:list) if(member!=api_.me) {
        peer=host_?member:api_.owner(lobby);
        if(peer!=member) throw Failure("Lobby owner is not the other player");
        peerSeen_=now;
        // The host announces its partner only once the partner's relay talks to it: a joiner that refused
        // the lobby (other mode or version) leaves without ever starting the host's link.
        if(!host_) { announced_=true; log("PEER_READY","\"peer\":"+std::to_string(peer)); }
    }
}
void Relay::receive(std::uint64_t now) {
    for(const auto& message:api_.messages()) {
        const auto list=lobby?checkedMembers():std::vector<Id>{};
        if(!peer || message.sender!=peer || std::find(list.begin(),list.end(),message.sender)==list.end()) {
            ++rejected; log("NONMEMBER_MESSAGE_REJECTED","\"peer\":"+std::to_string(message.sender)); continue;
        }
        const auto frame=decode(message.data);
        peerSeen_=now;
        if(!announced_) { announced_=true; log("PEER_READY","\"peer\":"+std::to_string(peer)); }
        switch(frame.kind) {
        case Kind::Ping: break;
        case Kind::Open:
            if(!host_ || openRequested_ || frame.offset) throw Failure("Invalid or repeated stream open");
            openRequested_=true; break;
        case Kind::Data:
            if(!(openRequested_ || (!host_ && local_)) || remoteClosed_ || frame.offset!=rx) throw Failure("Out-of-order relay stream data");
            if(frame.data.size()>std::numeric_limits<Id>::max()-rx) throw Failure("Relay byte offset overflow");
            if(localReset_) discarded+=frame.data.size();
            else {
                if(pending_.size()-pendingPosition_+frame.data.size()>MaxBuffer) throw Failure("Relay receive buffer limit");
                if(pendingPosition_) { pending_.erase(pending_.begin(),pending_.begin()+static_cast<std::ptrdiff_t>(pendingPosition_)); pendingPosition_=0; }
                pending_.insert(pending_.end(),frame.data.begin(),frame.data.end());
            }
            rx+=frame.data.size(); break;
        case Kind::Close:
            if(remoteClosed_ || frame.offset!=rx || !(openRequested_ || (!host_ && local_))) throw Failure("Invalid stream close");
            remoteClosed_=true; queue(encode(Kind::Ack,rx)); break;
        case Kind::Ack:
            if(!eof_ || frame.offset!=tx || closeAck_) throw Failure("Invalid stream close acknowledgement");
            closeAck_=true; break;
        }
    }
}
void Relay::localEnd(bool reset) {
    if(reset) {
        localReset_=true; discarded+=pending_.size()-pendingPosition_; pending_.clear(); pendingPosition_=0;
    }
    if(!eof_) { eof_=true; queue(encode(Kind::Close,tx)); log(reset?"GAME_STREAM_RESET":"GAME_STREAM_EOF"); }
}
void Relay::localIo(std::uint64_t now) {
    if(host_ && openRequested_ && !local_ && now>=nextConnect_) {
        nextConnect_=now+100;
        Socket candidate(::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP));
        if(!candidate) throw Failure("Could not create game loopback socket");
        nonblocking(candidate.get());
        sockaddr_in address{}; address.sin_family=AF_INET; address.sin_port=htons(port_); address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        const int result=connect(candidate.get(),reinterpret_cast<sockaddr*>(&address),sizeof(address));
        if(result==0 || wouldBlock()) { local_=std::move(candidate); connecting_=result!=0; if(!connecting_) log("GAME_STREAM_CONNECTED"); }
    } else if(!host_ && listener_ && !local_) {
        const SOCKET accepted=accept(listener_.get(),nullptr,nullptr);
        if(accepted!=INVALID_SOCKET) { local_.reset(accepted); nonblocking(local_.get()); listener_.reset(); queue(encode(Kind::Open)); log("GAME_STREAM_ACCEPTED"); }
        else if(!wouldBlock()) throw Failure("Game loopback accept failed");
    }
    if(!local_) return;
    if(connecting_) {
        WSAPOLLFD descriptor{local_.get(),POLLWRNORM,0};
        const int result=WSAPoll(&descriptor,1,0);
        if(result==SOCKET_ERROR) throw Failure("Game loopback connection poll failed");
        if(result==0) return;
        int error=0, length=sizeof(error);
        if(getsockopt(local_.get(),SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&length) || error) { local_.reset(); connecting_=false; return; }
        connecting_=false; log("GAME_STREAM_CONNECTED");
    }
    if(localReset_) return;
    if(pendingPosition_<pending_.size()) {
        const auto remaining=pending_.size()-pendingPosition_;
        const int sent=send(local_.get(),reinterpret_cast<const char*>(pending_.data()+pendingPosition_),static_cast<int>(remaining),0);
        if(sent>0) { pendingPosition_+=static_cast<std::size_t>(sent); if(pendingPosition_==pending_.size()) { pending_.clear(); pendingPosition_=0; } }
        else if(sent==0) throw Failure("Game write returned zero");
        else if(resetError()) localEnd(true);
        else if(!wouldBlock()) throw Failure("Game loopback write failed");
    }
    if(remoteClosed_ && pending_.empty() && !writeClosed_) {
        if(shutdown(local_.get(),SD_SEND) && !localReset_) localEnd(true);
        writeClosed_=true;
    }
    // Reserve room for close/ack; keep queue bytes and count bounded even under backpressure.
    if(!eof_ && outgoing_.size()<60 && outgoingBytes_+HeaderSize+MaxChunk<=MaxBuffer-1024) {
        Bytes data(MaxChunk);
        const int count=recv(local_.get(),reinterpret_cast<char*>(data.data()),static_cast<int>(data.size()),0);
        if(count>0) {
            data.resize(static_cast<std::size_t>(count));
            if(data.size()>std::numeric_limits<Id>::max()-tx) throw Failure("Relay byte offset overflow");
            queue(encode(Kind::Data,tx,data)); tx+=data.size();
        } else if(count==0) localEnd();
        else if(resetError()) localEnd(true);
        else if(!wouldBlock()) throw Failure("Game loopback read failed");
    }
}
void Relay::start(Id joinLobby) {
    if(started_) throw Failure("Relay already started");
    started_=true; peerSeen_=lastPing_=GetTickCount64(); api_.start(host_,joinLobby);
}
bool Relay::step(std::uint64_t now, unsigned completionGraceMs) {
    if(!started_ || closed_) throw Failure("Relay is not running");
    callbacks(); findPeer(now); receive(now); localIo(now);
    if(peer) {
        if(now-lastPing_>=1000 && outgoing_.empty()) { queue(encode(Kind::Ping)); lastPing_=now; }
        if(now-peerSeen_>20000) throw Failure("Peer keep-alive timed out");
        while(!outgoing_.empty()) {
            if(!api_.send(peer,outgoing_.front())) break;
            outgoingBytes_-=outgoing_.front().size(); outgoing_.pop_front();
        }
    }
    if(handshakeComplete() && pending_.empty() && outgoing_.empty()) {
        if(!doneSince_) doneSince_=now;
        if(now-doneSince_>=completionGraceMs) {
            log("STREAM_COMPLETE","\"sent\":"+std::to_string(tx)+",\"received\":"+std::to_string(rx)+",\"discarded_after_game_reset\":"+std::to_string(discarded)+",\"transport\":"+jsonString(api_.kind()));
            return true;
        }
    }
    return false;
}
void Relay::close() noexcept {
    if(closed_) return;
    closed_=true; listener_.reset(); local_.reset(); if(started_) api_.close(lobby,peer);
}
} // namespace fof
