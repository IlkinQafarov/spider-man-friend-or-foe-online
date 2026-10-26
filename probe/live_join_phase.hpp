#pragma once
#include "campaign_phase.hpp"
#include "live_join_journal.hpp"

namespace coop::live_join {
constexpr unsigned kOfferPhase=0x31000,kCatchupPhase=0x31001,kHandoffPhase=0x31002;
struct Offer {
    std::uint32_t tick=0,frames=0,baseTick=0,partyMode=0,delay=6;
    std::uint32_t loadSequence=0,cycle=0,cycleStartTick=0;
    std::uint64_t checkpoint=0;
};
inline PhaseState MakeOffer(const Offer& offer,const std::array<std::uint8_t,32>& content){
    PhaseState state{};state.kind=phaseLoad;state.id=kOfferPhase;state.flags=phaseReady;state.content=content;
    const std::uint8_t tag[8]={'F','O','F','L','I','V','E','6'};std::memcpy(state.identity.data(),tag,8);
    detail::Put32(state.identity.data()+8,offer.loadSequence);detail::Put32(state.identity.data()+12,offer.cycle);
    detail::Put32(state.identity.data()+16,offer.cycleStartTick);
    state.selection=offer.tick;state.auxiliary[1]=kSchemaVersion;state.auxiliary[2]=offer.frames;
    state.auxiliary[3]=std::uint32_t(offer.checkpoint);state.auxiliary[4]=std::uint32_t(offer.checkpoint>>32);
    state.auxiliary[5]=offer.baseTick;state.auxiliary[6]=offer.partyMode;state.auxiliary[7]=offer.delay;return state;
}
inline bool ReadOffer(const PhaseState& state,Offer& out){
    const std::uint8_t tag[8]={'F','O','F','L','I','V','E','6'};
    if(!CampaignPhaseValid(state)||state.kind!=phaseLoad||state.id!=kOfferPhase||state.flags!=phaseReady||
       std::memcmp(state.identity.data(),tag,8)||detail::Get32(state.identity.data()+16)>state.selection||
       state.auxiliary[1]!=kSchemaVersion||!state.auxiliary[2]||state.auxiliary[2]>kMaxFrames||
       state.auxiliary[6]>kPartyModeTwoHumans||state.auxiliary[7]!=6||
       std::uint64_t(state.auxiliary[5])+state.auxiliary[2]!=state.selection)return false;
    Offer decoded{};decoded.tick=state.selection;decoded.frames=state.auxiliary[2];
    for(unsigned i=20;i<32;i++)if(state.identity[i])return false;
    decoded.loadSequence=detail::Get32(state.identity.data()+8);decoded.cycle=detail::Get32(state.identity.data()+12);
    decoded.cycleStartTick=detail::Get32(state.identity.data()+16);
    decoded.baseTick=state.auxiliary[5];decoded.partyMode=state.auxiliary[6];decoded.delay=state.auxiliary[7];
    decoded.checkpoint=std::uint64_t(state.auxiliary[4])<<32|state.auxiliary[3];out=decoded;return true;
}
inline PhaseState MakeCheckpoint(unsigned id,unsigned tick,std::uint64_t checkpoint,unsigned mode,
                                 const std::array<std::uint8_t,32>& content){
    PhaseState state{};state.kind=phaseLoad;state.id=id;state.flags=phaseReady;state.content=content;
    state.selection=tick;state.auxiliary[1]=std::uint32_t(checkpoint);
    state.auxiliary[2]=std::uint32_t(checkpoint>>32);state.auxiliary[3]=mode;return state;
}
}
