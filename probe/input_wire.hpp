#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace coop {
using Actions = std::array<float,28>;
using Digest = std::array<std::uint8_t,32>;
// Private two-peer prototype. Game simulation/replay remains a separate concern.
class InputWire {
public:
    InputWire();
    ~InputWire();
    InputWire(const InputWire&)=delete;
    InputWire& operator=(const InputWire&)=delete;
    bool Open(bool host,const char* ipv4,std::uint16_t port,const Digest& build,
              const Digest& scenario,unsigned delay=3,unsigned timeoutMs=15000);
    bool Step(std::uint32_t tick,const Actions& local,Actions& remote,
              std::uint64_t localState=0,std::uint64_t* remoteState=nullptr);
    bool Finish();
    // Ordered user cancellation, distinct from finite-test completion and failure.
    // Safe alongside a blocked Step; Close/Open still require external ownership.
    bool RequestStop();
    bool PeerStopped() const;
    void Close();
    std::string Error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
