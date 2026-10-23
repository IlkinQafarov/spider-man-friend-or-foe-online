#include "campaign_phase.hpp"
#include <cstdio>
#include <cstring>
static bool ignoreProgress=false,applyUnready=false,wholeUiWord=false,ignoreNativeTask=false,ignoreNoticeSource=false,ignoreNoticeCaption=false;
static coop::PhaseState peerForTest(coop::PhaseState a,coop::PhaseState b){if(ignoreProgress)b.auxiliary[1]=a.auxiliary[1];return b;}
static bool equal(const coop::PhaseState& a,const coop::PhaseState& b,bool menu=false){return coop::CampaignPhaseEqual(a,peerForTest(a,b),menu);}
static coop::CampaignAgreement agree(coop::PhaseState a,coop::PhaseState b){b=peerForTest(a,b);if(applyUnready){if(!a.flags)a.flags=coop::phaseReady;if(!b.flags)b.flags=coop::phaseReady;}return coop::CampaignMenuAgreement(a,b);}
static bool ack(coop::CampaignSaveAckFacts facts){if(wholeUiWord&&facts.uiWord!=1)return false;if(ignoreNativeTask)facts.handlerIsNativeTask=true;return coop::CampaignSaveAckReady(facts);}
static bool notice(coop::CampaignAutosaveNoticeFacts facts){if(ignoreNoticeSource)facts.sourceAttested=true;if(ignoreNoticeCaption)facts.captionMatches=true;return coop::CampaignAutosaveNoticeReady(facts);}
int main(int argc,char** argv){
    if(argc>1){ignoreProgress=!strcmp(argv[1],"--mutant-ignore-progress");applyUnready=!strcmp(argv[1],"--mutant-apply-unready");wholeUiWord=!strcmp(argv[1],"--mutant-whole-ui-word");ignoreNativeTask=!strcmp(argv[1],"--mutant-ignore-native-task");ignoreNoticeSource=!strcmp(argv[1],"--mutant-ignore-notice-source");ignoreNoticeCaption=!strcmp(argv[1],"--mutant-ignore-notice-caption");if(!ignoreProgress&&!applyUnready&&!wholeUiWord&&!ignoreNativeTask&&!ignoreNoticeSource&&!ignoreNoticeCaption)return 2;}
    unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::printf("CAMPAIGN_PHASE_SELFTEST_FAIL check=%u line=%u\n",checks,(unsigned)__LINE__);return 1;}}while(0)
    coop::PhaseState a={};a.kind=coop::phaseMenu;a.id=1;a.flags=coop::phaseReady;a.identity[0]=3;a.content[0]=7;
    for(unsigned i=1;i<8;i++)a.auxiliary[i]=i;
    CHECK(equal(a,a));CHECK(agree(a,a)==coop::CampaignAgreement::ready);
    auto b=a;b.auxiliary[0]=1u<<10;CHECK(!equal(a,b));CHECK(equal(a,b,true));CHECK(agree(a,b)==coop::CampaignAgreement::ready);
    for(unsigned i=1;i<8;i++){b=a;++b.auxiliary[i];CHECK(!equal(a,b,true));CHECK(agree(a,b)==coop::CampaignAgreement::invalid);}
    b=a;++b.selection;CHECK(!equal(a,b));CHECK(agree(a,b)==coop::CampaignAgreement::invalid);
    b=a;++b.identity[31];CHECK(!equal(a,b));b=a;++b.content[31];CHECK(!equal(a,b));CHECK(agree(a,b)==coop::CampaignAgreement::invalid);
    b=a;++b.id;CHECK(agree(a,b)==coop::CampaignAgreement::invalid);
    b=a;b.flags=0;CHECK(agree(a,b)==coop::CampaignAgreement::waiting);CHECK(agree(b,a)==coop::CampaignAgreement::waiting);
    b.selection=0xffffffffu;b.auxiliary[1]=0xffff;CHECK(agree(a,b)==coop::CampaignAgreement::waiting);
    b=a;b.flags=coop::phaseConfirmed;CHECK(!coop::CampaignPhaseValid(b));b.flags=4;CHECK(!coop::CampaignPhaseValid(b));
    b=a;b.auxiliary[0]=1u<<28;CHECK(!coop::CampaignPhaseValid(b));b=a;b.id=0;CHECK(!coop::CampaignPhaseValid(b));
    b=a;b.kind=99;CHECK(!coop::CampaignPhaseValid(b));b=a;b.kind=coop::phaseLoad;CHECK(coop::CampaignPhaseValid(b));CHECK(agree(a,b)==coop::CampaignAgreement::invalid);
    b.auxiliary[0]=1;CHECK(!coop::CampaignPhaseValid(b));b.auxiliary[0]=0;b.flags=3;CHECK(coop::CampaignPhaseValid(b));CHECK(!equal(a,b));
    a.flags=3;b=a;CHECK(equal(a,b));++b.selection;CHECK(!equal(a,b));
    coop::CampaignSaveAckFacts facts={};facts.readable=facts.profileInitialized=facts.active=facts.exactSceneType=facts.exactTaskType=facts.handlerIsNativeTask=true;
    facts.close=facts.row=facts.slot=facts.operation=facts.storageToken=0;facts.state=9;facts.uiWord=1;CHECK(ack(facts));
    // The real observation has nonzero bytes above +3C; these are not the UI
    // success byte and must not block a legitimate acknowledgement.
    facts.uiWord=0x3d3d3d01;CHECK(ack(facts));
    bool coop::CampaignSaveAckFacts::* guards[]={&coop::CampaignSaveAckFacts::readable,&coop::CampaignSaveAckFacts::profileInitialized,&coop::CampaignSaveAckFacts::active,&coop::CampaignSaveAckFacts::exactSceneType,&coop::CampaignSaveAckFacts::exactTaskType,&coop::CampaignSaveAckFacts::handlerIsNativeTask};
    for(auto guard:guards){auto wrong=facts;wrong.*guard=false;CHECK(!ack(wrong));}
    std::uint32_t coop::CampaignSaveAckFacts::* zeroFields[]={&coop::CampaignSaveAckFacts::close,&coop::CampaignSaveAckFacts::row,&coop::CampaignSaveAckFacts::slot,&coop::CampaignSaveAckFacts::operation,&coop::CampaignSaveAckFacts::storageToken};
    for(auto field:zeroFields){auto wrong=facts;wrong.*field=1;CHECK(!ack(wrong));}
    auto wrong=facts;wrong.state=8;CHECK(!ack(wrong));wrong=facts;wrong.uiWord=0x3d3d3d00;CHECK(!ack(wrong));wrong.uiWord=0x3d3d3d02;CHECK(!ack(wrong));
    a.kind=coop::phaseMenu;a.id=5;a.flags=ack(facts)?coop::phaseReady:0;b=a;b.flags=0;CHECK(agree(a,b)==coop::CampaignAgreement::waiting);
    coop::CampaignAutosaveNoticeFacts n={};n.readable=n.sourceAttested=n.captionMatches=n.profileInitialized=n.exactSceneType=n.exactProfileType=n.active=true;
    n.row=n.slot=n.operation=n.label0=n.forcedClose=0;n.label1=n.label2=0xffffffffu;n.count=n.styleFlag=1;n.profileState=5;CHECK(notice(n));
    bool coop::CampaignAutosaveNoticeFacts::* noticeGuards[]={&coop::CampaignAutosaveNoticeFacts::readable,&coop::CampaignAutosaveNoticeFacts::sourceAttested,&coop::CampaignAutosaveNoticeFacts::captionMatches,&coop::CampaignAutosaveNoticeFacts::profileInitialized,&coop::CampaignAutosaveNoticeFacts::exactSceneType,&coop::CampaignAutosaveNoticeFacts::exactProfileType,&coop::CampaignAutosaveNoticeFacts::active};
    for(auto guard:noticeGuards){auto other=n;other.*guard=false;CHECK(!notice(other));}
    std::uint32_t coop::CampaignAutosaveNoticeFacts::* noticeFields[]={&coop::CampaignAutosaveNoticeFacts::row,&coop::CampaignAutosaveNoticeFacts::count,&coop::CampaignAutosaveNoticeFacts::slot,&coop::CampaignAutosaveNoticeFacts::operation,&coop::CampaignAutosaveNoticeFacts::profileState,&coop::CampaignAutosaveNoticeFacts::label0,&coop::CampaignAutosaveNoticeFacts::label1,&coop::CampaignAutosaveNoticeFacts::label2,&coop::CampaignAutosaveNoticeFacts::forcedClose,&coop::CampaignAutosaveNoticeFacts::styleFlag};
    for(auto field:noticeFields){auto other=n;other.*field^=1;CHECK(!notice(other));}
    a.id=6;a.flags=coop::phaseReady;b=a;b.flags=0;b.selection=0xffffffffu;CHECK(agree(a,b)==coop::CampaignAgreement::waiting);
    std::printf("CAMPAIGN_PHASE_SELFTEST_PASS checks=%u\n",checks);return 0;
}
