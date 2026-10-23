#pragma once
#include "input_wire.hpp"
namespace coop {
constexpr std::uint32_t campaignMenuMask = 0x0fffffffu;
inline bool CampaignPhaseValid(const PhaseState& p) {
    return (p.kind == phaseMenu || p.kind == phaseLoad) && p.id != 0 &&
           (p.flags & ~3u) == 0 && p.flags != phaseConfirmed && (p.auxiliary[0] & ~campaignMenuMask) == 0 &&
           (p.kind != phaseLoad || p.auxiliary[0] == 0);
}
inline bool CampaignPhaseEqual(const PhaseState& a,const PhaseState& b,bool ignoreMenuBits=false) {
    if (!CampaignPhaseValid(a) || !CampaignPhaseValid(b) || a.kind != b.kind || a.id != b.id ||
        a.selection != b.selection || a.flags != b.flags || a.identity != b.identity || a.content != b.content)
        return false;
    for (unsigned i=ignoreMenuBits?1:0;i<8;++i) if(a.auxiliary[i]!=b.auxiliary[i])return false;
    return true;
}
enum class CampaignAgreement { invalid, waiting, ready };
struct CampaignSaveAckFacts {
    bool readable=false,profileInitialized=false,active=false,exactSceneType=false,exactTaskType=false,handlerIsNativeTask=false;
    std::uint32_t close=1,row=0xffffffffu,slot=0xffffffffu,operation=0xffffffffu,state=0xffffffffu,uiWord=0,storageToken=0xffffffffu;
};
inline bool CampaignSaveAckReady(const CampaignSaveAckFacts& f) {
    // This identifies the observed native acknowledgement, not successful disk
    // persistence. Bytes above task+3C belong to other flags/padding.
    return f.readable&&f.profileInitialized&&f.active&&f.exactSceneType&&f.exactTaskType&&f.handlerIsNativeTask&&
        !f.close&&!f.row&&!f.slot&&!f.operation&&f.state==9&&(f.uiWord&0xffu)==1&&!f.storageToken;
}
struct CampaignAutosaveNoticeFacts {
    bool readable=false,sourceAttested=false,captionMatches=false,profileInitialized=false,exactSceneType=false,exactProfileType=false,active=false;
    std::uint32_t row=0xffffffffu,count=0,slot=0xffffffffu,operation=0xffffffffu,profileState=0,
        label0=0xffffffffu,label1=0,label2=0,forcedClose=1,styleFlag=0;
};
inline bool CampaignAutosaveNoticeReady(const CampaignAutosaveNoticeFacts& f) {
    return f.readable&&f.sourceAttested&&f.captionMatches&&f.profileInitialized&&f.exactSceneType&&f.exactProfileType&&f.active&&
        !f.row&&f.count==1&&!f.slot&&!f.operation&&f.profileState==5&&!f.label0&&f.label1==0xffffffffu&&f.label2==0xffffffffu&&!f.forcedClose&&f.styleFlag==1;
}
inline CampaignAgreement CampaignMenuAgreement(const PhaseState& a,const PhaseState& b) {
    if(!CampaignPhaseValid(a)||!CampaignPhaseValid(b)||a.kind!=phaseMenu||b.kind!=phaseMenu||
       a.id!=b.id||a.content!=b.content||a.flags>phaseReady||b.flags>phaseReady)return CampaignAgreement::invalid;
    // A selected row may not exist yet while native animation/resource setup
    // finishes. Neither peer receives choices until both native rows are ready.
    if(!a.flags||!b.flags)return CampaignAgreement::waiting;
    return CampaignPhaseEqual(a,b,true)?CampaignAgreement::ready:CampaignAgreement::invalid;
}
}
