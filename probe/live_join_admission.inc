// Research-gated live admission. Only the incoming game loads/replays a mission.
// The host waits at the next input boundary with its existing world intact.
static ULONGLONG liveJoinDeadline=0;
static coop::live_join::Offer liveJoinOffer;

static bool liveJoinPump(){
    MSG message;while(PeekMessageW(&message,0,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
    if(campaignStopping()||!networkWire||networkWire->PeerStopped()||GetTickCount64()>=liveJoinDeadline)return false;
    campaignWaitNotice(true);Sleep(2);return true;
}
static bool liveJoinPhase(const coop::PhaseState& state,coop::PhaseState& peer){
    const unsigned sequence=campaignPhaseSequence++;
    if(!networkWire->SendPhase(sequence,state))return false;
    for(;;){int got=networkWire->TryReceivePhase(sequence,peer);
        if(got)return got>0&&coop::CampaignPhaseValid(peer)&&peer.content==state.content;
        if(!liveJoinPump())return false;
    }
}
struct LiveJoinBlobSend {coop::InputWire* wire;const std::vector<std::uint8_t>* bytes;bool result;};
static DWORD WINAPI liveJoinSendBlobThread(void* opaque){
    auto& task=*(LiveJoinBlobSend*)opaque;task.result=task.wire->SendBlob(0,*task.bytes);return 0;
}
static bool liveJoinSaveCapture(const std::vector<std::uint8_t>& bytes){
    wchar_t path[32768];_snwprintf(path,32768,L"%s\\live-join-capture-%u.bin",sandbox,networkTick);
    HANDLE file=CreateFileW(path,GENERIC_WRITE,0,0,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,0);
    if(file==INVALID_HANDLE_VALUE)return false;DWORD written=0;
    bool ok=WriteFile(file,bytes.data(),(DWORD)bytes.size(),&written,0)&&written==bytes.size();
    if(!FlushFileBuffers(file))ok=false;if(!CloseHandle(file))ok=false;return ok;
}
static bool liveJoinSendBlob(const std::vector<std::uint8_t>& bytes){
    LiveJoinBlobSend task={networkWire,&bytes,false};
    HANDLE thread=CreateThread(0,0,liveJoinSendBlobThread,&task,0,0);if(!thread)return false;
    bool canceled=false;
    while(WaitForSingleObject(thread,0)==WAIT_TIMEOUT){
        if(!canceled&&!liveJoinPump()){canceled=true;networkWire->RequestStop();}
        else if(canceled)Sleep(2);
    }
    CloseHandle(thread);return !canceled&&task.result;
}
static void liveJoinAbandonHost(const char* reason){
    campaignWaitNotice(false);log("LIVE_JOIN CANCELED tick=%u reason=%s preserved=1",networkTick,reason);
    networkWire->Close();networkRole=0;networkReady=false;networkDeferred=true;
    campaignJoinPending=campaignJoinSent=campaignJoinReceived=campaignJoined=false;
    campaignPhaseSequence=0;networkAgreedDelay=0;liveRecordSoloActive=true;
}
static coop::PhaseState liveJoinCheckpoint(unsigned id){
    observeNetworkState(networkTick-1,true);
    return coop::live_join::MakeCheckpoint(id,networkTick,networkState,livePartyMode,networkContent);
}
static bool liveJoinCheckpointEqual(const coop::PhaseState& own,const coop::PhaseState& peer){
    return coop::CampaignPhaseEqual(own,peer);
}
static void liveJoinFinishAdmission(){
    if(liveAdmissionClient){campaignLoadSequence=liveJoinOffer.loadSequence;campaignCycle=liveJoinOffer.cycle;campaignCycleStartTick=liveJoinOffer.cycleStartTick;}
    liveWireBaseTick=networkTick;networkAgreedDelay=6;
    campaignJoinPending=false;campaignJoinSent=campaignJoinReceived=campaignJoined=true;
    liveRecordSoloActive=liveReplayRequested=liveReplayRunning=liveReplayFinished=liveAdmissionClient=false;
    campaignGameplay=campaignMission=campaignPrepared=true;networkWire->SetTimeout(30000);
    campaignWaitNotice(false);log("LIVE_JOIN COMPLETE tick=%u preserved=1",networkTick);
}
static bool liveJoinReceiveOffer(const coop::PhaseState& offer){
    liveJoinDeadline=GetTickCount64()+300000;
    if(!coop::live_join::ReadOffer(offer,liveJoinOffer))
        campaignReject("live join offer bounds or version");
    std::vector<std::uint8_t> bytes;
    for(;;){int got=networkWire->TryReceiveBlob(0,bytes);if(got>0)break;
        if(got<0||!liveJoinPump()){campaignWaitNotice(false);campaignCloseFromPeer("live_join_blob",campaignPhaseSequence);return false;}}
    if(!liveJournal.Decode(bytes.data(),bytes.size())||liveJournal.Size()!=liveJoinOffer.frames||
       liveJournal.GetMetadata().baseTick!=liveJoinOffer.baseTick||
       (ULONGLONG)liveJournal.GetMetadata().baseTick+liveJournal.Size()!=offer.selection||
       liveJournal.At(liveJournal.Size()-1)->partyMode!=liveJoinOffer.partyMode)
        campaignReject("live join bounded journal identity");
    const auto& meta=liveJournal.GetMetadata();
    DWORD selectedSidekick=0;memcpy(&selectedSidekick,meta.missionStartProgress.data()+0x1b4,4);
    if(selectedSidekick!=meta.sidekick)campaignReject("live join selected sidekick metadata mismatch");
    memcpy(campaignJoinProgress,meta.missionStartProgress.data(),sizeof(campaignJoinProgress));
    campaignJoinSelected=coop::ProgressSample(campaignJoinProgress);campaignJoinReceived=campaignJoined=true;
    campaignResumeArea=0;liveReplayRequested=liveAdmissionClient=true;liveReplayFinished=false;
    if(!liveJoinReplayArea()||!networkWire->SetDelay(6))campaignReject("live join native mission or delay");
    networkWire->SetTimeout(300000);campaignWaitNotice(false);
    log("LIVE_JOIN CATCHUP frames=%u target_tick=%lu host_area_preserved=1",(unsigned)liveJournal.Size(),offer.selection);
    return true;
}
static void liveJoinBeforeInput(){
    if(!liveJoinTest)return;
    if(liveAdmissionClient&&liveReplayFinished){
        auto state=liveJoinCheckpoint(0x31001);coop::PhaseState peer={};
        if(networkTick!=liveJoinOffer.tick||networkState!=liveJoinOffer.checkpoint||livePartyMode!=liveJoinOffer.partyMode||
           !liveJoinPhase(state,peer)||!liveJoinCheckpointEqual(state,peer)){
            log("LIVE_JOIN REJECT reason=catchup_checkpoint");campaignWaitNotice(false);liveJoinResearchClose();return;
        }
        if(livePartyMode==0){if(!liveJoinSetPartyMode(1)){log("LIVE_JOIN REJECT reason=client_handoff");liveJoinResearchClose();return;}livePartyMode=1;}
        state=liveJoinCheckpoint(0x31002);
        if(!liveJoinPhase(state,peer)||!liveJoinCheckpointEqual(state,peer)){
            log("LIVE_JOIN REJECT reason=handoff_checkpoint");campaignWaitNotice(false);liveJoinResearchClose();return;}
        liveJoinFinishAdmission();return;
    }
    if(networkRole!=1||!networkReady||!campaignJoinPending||campaignJoinSent||!liveRecordSoloActive||
       !campaignGameplay||!campaignMission||liveRecordPending||!liveJournal.IsValid()||!liveJournal.Size())return;
    DWORD flow=readWord(0x69e72c),mode[3]={},transition=readWord(0x6a9988);
    if(readWord(flow+8)!=128||readWord(flow+0xc)!=128||!captureGameMode(mode)||mode[0]!=0||
       !transition||readWord(transition+0x2c)!=0||!localStreamDone())return;
    liveJoinDeadline=GetTickCount64()+300000;
    auto checkpoint=liveJoinCheckpoint(0x31001);
    networkWire->SetTimeout(300000);
    coop::live_join::Offer details={};details.tick=networkTick;details.frames=(unsigned)liveJournal.Size();
    details.checkpoint=networkState;details.baseTick=liveJournal.GetMetadata().baseTick;details.partyMode=livePartyMode;
    details.loadSequence=campaignLoadSequence;details.cycle=campaignCycle;details.cycleStartTick=campaignCycleStartTick;
    auto offer=coop::live_join::MakeOffer(details,networkContent);
    std::vector<std::uint8_t> bytes;coop::PhaseState peer={};
    log("LIVE_JOIN FROZEN tick=%u history=%u preserved=1",networkTick,(unsigned)liveJournal.Size());
    if(!liveJournal.Encode(bytes)||!liveJoinSaveCapture(bytes)||!networkWire->SetDelay(6)||!liveJoinPhase(offer,peer)||peer.id!=0x30000||
       !liveJoinSendBlob(bytes)||!liveJoinPhase(checkpoint,peer)||!liveJoinCheckpointEqual(checkpoint,peer)){
        liveJoinAbandonHost("history_or_checkpoint");return;
    }
    const auto preserved=liveJoinCheckpoint(0x31001);
    if(!liveJoinCheckpointEqual(checkpoint,preserved)){liveJoinAbandonHost("host_changed_while_waiting");return;}
    if(livePartyMode==0){if(!liveJoinSetPartyMode(1)){liveJoinAbandonHost("host_handoff");return;}livePartyMode=1;}
    checkpoint=liveJoinCheckpoint(0x31002);
    if(!liveJoinPhase(checkpoint,peer)||!liveJoinCheckpointEqual(checkpoint,peer)){
        liveJoinAbandonHost("handoff_checkpoint");return;}
    liveJoinFinishAdmission();
}
