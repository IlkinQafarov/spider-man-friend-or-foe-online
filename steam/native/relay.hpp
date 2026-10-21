// Own-code Windows x64 relay. No Steam SDK headers or redistributables included.
#pragma once
#ifndef _WIN64
#error This relay must be built for Windows x64.
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace fof {
using Id = std::uint64_t;
using Bytes = std::vector<unsigned char>;
constexpr char Tag[] = "fof-native-prototype-v1";
constexpr std::size_t HeaderSize = 16, MaxChunk = 16384, MaxBuffer = 1024 * 1024;
constexpr char MockSha256[] = "5e84877b8ba6a8bd99bb613fe9a51042bf19106ac18b50873984a09509f5049d";
enum class Kind : unsigned char { Open=1, Data=2, Close=3, Ping=4, Ack=5 };
struct Failure : std::runtime_error { using std::runtime_error::runtime_error; };
struct Frame { Kind kind; Id offset; Bytes data; };
Bytes encode(Kind kind, Id offset=0, const Bytes& data={});
Frame decode(const Bytes& message);
std::string jsonString(const std::string& value);
void log(const std::string& event, const std::string& fields="");
std::wstring executableDirectory();
std::wstring absolutePath(const std::wstring& value);
std::string sha256File(const std::wstring& path);

struct Event {
    enum class Type { Lobby, Request, Failed, Left } type;
    Id user=0, lobby=0;
    bool success=true;
};
struct Message { Id sender; Bytes data; };
struct Backend {
    virtual ~Backend()=default;
    Id me=0;
    virtual const char* kind() const=0;
    virtual void start(bool host, Id lobby)=0;
    virtual std::vector<Event> events()=0;
    virtual void setTag(Id lobby, const char* value)=0;
    virtual std::string tag(Id lobby)=0;
    virtual Id owner(Id lobby)=0;
    virtual std::vector<Id> members(Id lobby)=0;
    virtual bool accept(Id user)=0;
    virtual bool send(Id user, const Bytes& data)=0;
    virtual std::vector<Message> messages()=0;
    virtual void close(Id lobby, Id peer) noexcept=0;
};
std::unique_ptr<Backend> steamBackend(const std::wstring& path, const std::string& hash, bool ownMock);
int abiSelftest();
int protocolSelftest();

class Winsock {
public:
    Winsock();
    ~Winsock() { WSACleanup(); }
    Winsock(const Winsock&)=delete;
    Winsock& operator=(const Winsock&)=delete;
};
class Socket {
    SOCKET value_=INVALID_SOCKET;
public:
    Socket()=default;
    explicit Socket(SOCKET value): value_(value) {}
    ~Socket() { reset(); }
    Socket(const Socket&)=delete;
    Socket& operator=(const Socket&)=delete;
    Socket(Socket&& other) noexcept: value_(other.release()) {}
    Socket& operator=(Socket&& other) noexcept { if(this!=&other) reset(other.release()); return *this; }
    SOCKET get() const { return value_; }
    explicit operator bool() const { return value_!=INVALID_SOCKET; }
    SOCKET release() { const auto result=value_; value_=INVALID_SOCKET; return result; }
    void reset(SOCKET value=INVALID_SOCKET) { if(*this) closesocket(value_); value_=value; }
};
void nonblocking(SOCKET socket);
Socket listenLoopback(unsigned short port);
unsigned short socketPort(SOCKET socket);

class Relay {
    Backend& api_;
    bool host_, started_=false, closed_=false, connecting_=false;
    unsigned short port_;
    std::wstring readyFile_;
    Socket listener_, local_;
    std::deque<Bytes> outgoing_;
    std::size_t outgoingBytes_=0, pendingPosition_=0;
    Bytes pending_;
    bool openRequested_=false, eof_=false, remoteClosed_=false, closeAck_=false;
    bool writeClosed_=false, localReset_=false;
    std::uint64_t peerSeen_=0, lastPing_=0, doneSince_=0, nextConnect_=0;
    std::vector<Id> checkedMembers();
    void queue(Bytes message);
    void lobbyReady(Id id);
    void callbacks();
    void findPeer(std::uint64_t now);
    void receive(std::uint64_t now);
    void localEnd(bool reset=false);
    void localIo(std::uint64_t now);
public:
    Id lobby=0, peer=0, tx=0, rx=0, discarded=0, rejected=0;
    Relay(Backend& api, bool host, unsigned short port, std::wstring readyFile={}):
        api_(api),host_(host),port_(port),readyFile_(std::move(readyFile)) {}
    ~Relay() { close(); }
    Relay(const Relay&)=delete;
    Relay& operator=(const Relay&)=delete;
    void start(Id joinLobby=0);
    bool step(std::uint64_t now=GetTickCount64(), unsigned completionGraceMs=1000);
    void close() noexcept;
    unsigned short listenPort() const { return listener_ ? socketPort(listener_.get()) : 0; }
    bool resetSeen() const { return localReset_; }
    bool handshakeComplete() const { return eof_ && remoteClosed_ && closeAck_; }
    void pumpCallbacks() { callbacks(); }
};
} // namespace fof
