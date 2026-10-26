#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace coop {
using Actions = std::array<float,28>;
using Digest = std::array<std::uint8_t,32>;
constexpr std::uint32_t menuPointerActive=1,menuPointerClick=2;
// Client-area coordinates normalized to 0..65535. Inactive samples are all zero;
// click is meaningful only with active. Transport delays this alongside actions.
struct MenuPointer {std::uint32_t x,y,flags;};
static_assert(sizeof(MenuPointer)==12,"menu pointer size");
constexpr std::uint32_t phaseMenu=1,phaseLoad=2,phaseReady=1,phaseConfirmed=2;
constexpr std::uint32_t blobChunkBytes=16384,blobMaxBytes=64u*1024u*1024u;
// Fixed-size menu/load metadata. Digests, IDs, selection and auxiliary values are
// opaque: the game layer must compare their meaning before acting on a choice.
struct PhaseState {
    std::uint32_t kind,id,selection,flags;
    Digest identity,content;
    std::array<std::uint32_t,8> auxiliary;
};
static_assert(sizeof(PhaseState)==112,"phase state size");
// Private two-peer prototype. Game simulation/replay remains a separate concern.
class InputWire {
public:
    InputWire();
    ~InputWire();
    InputWire(const InputWire&)=delete;
    InputWire& operator=(const InputWire&)=delete;
    bool Open(bool host,const char* ipv4,std::uint16_t port,const Digest& build,
              const Digest& scenario,unsigned delay=3,unsigned timeoutMs=15000);
    // Widen or narrow the wait used by Step/ExchangePhase/Finish after Open. A caller that
    // expects the peer to be slow for a while (loading, not yet past a one-time menu) can
    // raise this, then lower it again once both sides are actively exchanging every frame.
    void SetTimeout(unsigned timeoutMs);
    // Change the input delay agreed in Open, only before any input frame was sent or received; both peers must set
    // the same value before either sends its first frame (agree it in a phase exchange first).
    bool SetDelay(unsigned frames);
    bool Step(std::uint32_t tick,const Actions& local,Actions& remote,
              std::uint64_t localState=0,std::uint64_t* remoteState=nullptr,
              const MenuPointer* localPointer=nullptr,MenuPointer* remotePointer=nullptr);
    // Sequence starts at zero independently of gameplay ticks. Returns the peer
    // state; it does not certify matching identities or compatible selections.
    bool ExchangePhase(std::uint32_t sequence,const PhaseState& local,PhaseState& remote);
    // True once the peer has already sent this phase, so ExchangePhase returns without waiting.
    bool PhaseAvailable(std::uint32_t sequence) const;
    // ExchangePhase split in two, for a side that must keep running while its partner is still busy elsewhere
    // (e.g. watching a cutscene the other skipped): send this side's state once, then poll each frame.
    // TryReceivePhase returns 1 with the peer's state, 0 while it has not arrived, -1 when the link failed or stopped.
    bool SendPhase(std::uint32_t sequence,const PhaseState& local);
    int TryReceivePhase(std::uint32_t sequence,PhaseState& remote);
    // Reliable ordered bulk transfer, independent from frame and phase sequence spaces.
    // IDs start at zero. Empty/oversize sends fail. Receive returns 1 when the complete
    // blob is verified, 0 while pending, and -1 after stop/failure. Output is unchanged
    // unless a complete verified blob is delivered.
    bool SendBlob(std::uint32_t id,const std::vector<std::uint8_t>& bytes);
    int TryReceiveBlob(std::uint32_t id,std::vector<std::uint8_t>& bytes);
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
