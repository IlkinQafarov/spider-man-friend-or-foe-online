// Research-gated live admission. Only the incoming game loads/replays a mission.
// The host waits at the next input boundary with its existing world intact.
static ULONGLONG liveJoinDeadline=0;
static coop::live_join::Offer liveJoinOffer;
static bool liveJoinAdmissionChangedPartyMode=false;
static unsigned liveJoinAgreedDelay=6;

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
    if(liveJoinAdmissionChangedPartyMode&&!campaignStopping()){
        if(liveJoinSetPartyMode(coop::live_join::kPartyModeNativeAi)){livePartyMode=coop::live_join::kPartyModeNativeAi;
            log("LIVE_JOIN CANCELED restored_native_ai=1");}
        else log("LIVE_JOIN CANCELED restore_native_ai_failed=1 host_gameplay_continues=1");
        liveJoinAdmissionChangedPartyMode=false;
    }else liveJoinAdmissionChangedPartyMode=false;
    campaignWaitNotice(false);log("LIVE_JOIN CANCELED tick=%u reason=%s preserved=1",networkTick,reason);
    networkWire->RequestStop();networkWire->Close();networkRole=0;networkReady=false;networkDeferred=true;
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
static bool liveJoinAdaptiveDelay(unsigned& agreed){
    const auto ping=liveJoinCheckpoint(0x31003);coop::PhaseState peer={};double samples[6]={};
    for(unsigned i=0;i<7;i++){
        const double began=milliseconds();
        if(!liveJoinPhase(ping,peer)||!liveJoinCheckpointEqual(ping,peer))return false;
        const double elapsed=milliseconds()-began;
        if(i)samples[i-1]=elapsed;
    }
    // The 90th percentile of six samples uses the nearest-rank maximum.
    double p90=samples[0];for(unsigned i=1;i<6;i++)if(samples[i]>p90)p90=samples[i];
    unsigned local=(unsigned)ceil(p90/(2.0*16.6666667))+2u;
    if(local<6)local=6;if(local>14)local=14;
    coop::PhaseState choice={};choice.kind=coop::phaseLoad;choice.id=0x31004;choice.flags=coop::phaseReady;
    choice.selection=local;choice.content=networkContent;
    if(!liveJoinPhase(choice,peer)||peer.kind!=coop::phaseLoad||peer.id!=0x31004||peer.flags!=coop::phaseReady||
       peer.selection<6||peer.selection>14||peer.identity!=choice.identity||peer.auxiliary[0]!=0||
       peer.auxiliary[1]!=0||peer.auxiliary[2]!=0||peer.auxiliary[3]!=0||peer.auxiliary[4]!=0||
       peer.auxiliary[5]!=0||peer.auxiliary[6]!=0||peer.auxiliary[7]!=0)return false;
    agreed=local>peer.selection?local:peer.selection;
    if(!networkWire->SetDelay(agreed))return false;
    log("LIVE_JOIN INPUT_DELAY frames=%u local=%u p90_rtt_ms=%.2f",agreed,local,p90);
    return true;
}
static void liveJoinFinishAdmission(){
    if(liveAdmissionClient){campaignLoadSequence=liveJoinOffer.loadSequence;campaignCycle=liveJoinOffer.cycle;campaignCycleStartTick=liveJoinOffer.cycleStartTick;}
    liveWireBaseTick=networkTick;networkAgreedDelay=liveJoinAgreedDelay;
    networkSwapHeld=0;networkSwapLatched=false;
    campaignJoinPending=false;campaignJoinSent=campaignJoinReceived=campaignJoined=true;
    liveRecordSoloActive=liveReplayRequested=liveReplayRunning=liveReplayFinished=liveAdmissionClient=false;
    liveJoinAdmissionChangedPartyMode=false;
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
    if(!liveJoinEnabled())return;
    if(liveJoinTest&&networkRole==1&&campaignJoinPending&&networkTick%120==0){
        DWORD flow=readWord(0x69e72c),mode[3]={};captureGameMode(mode);
        log("LIVE_JOIN WAIT_DIAGNOSTIC tick=%u ready=%u sent=%u solo=%u gameplay=%u mission=%u pending_frame=%u valid=%u frames=%u flow=%lu/%lu mode=%lu stream_done=%u",
            networkTick,networkReady?1u:0u,campaignJoinSent?1u:0u,liveRecordSoloActive?1u:0u,
            campaignGameplay?1u:0u,campaignMission?1u:0u,liveRecordPending?1u:0u,liveJournal.IsValid()?1u:0u,
            (unsigned)liveJournal.Size(),readWord(flow+8),readWord(flow+0xc),mode[0],localStreamDone()?1u:0u);
    }
    if(liveAdmissionClient&&liveReplayFinished){
        auto state=liveJoinCheckpoint(0x31001);coop::PhaseState peer={};
        if(networkTick!=liveJoinOffer.tick||networkState!=liveJoinOffer.checkpoint||livePartyMode!=liveJoinOffer.partyMode||
           !liveJoinPhase(state,peer)||!liveJoinCheckpointEqual(state,peer)){
            log("LIVE_JOIN REJECT reason=catchup_checkpoint");campaignWaitNotice(false);liveJoinResearchClose();return;
        }
        if(!liveJoinAdaptiveDelay(liveJoinAgreedDelay)){
            log("LIVE_JOIN REJECT reason=input_delay_negotiation");campaignWaitNotice(false);liveJoinResearchClose();return;
        }
        if(livePartyMode==0){if(!liveJoinSetPartyMode(1)){log("LIVE_JOIN REJECT reason=client_handoff");liveJoinResearchClose();return;}livePartyMode=1;}
        state=liveJoinCheckpoint(0x31002);
        if(!liveJoinPhase(state,peer)||!liveJoinCheckpointEqual(state,peer)){
            log("LIVE_JOIN REJECT reason=handoff_checkpoint");campaignWaitNotice(false);liveJoinResearchClose();return;}
        liveJoinFinishAdmission();return;
    }
    if(networkRole==1&&networkReady&&campaignJoinPending&&!campaignJoinSent&&liveRecordSoloActive&&
       (!liveJoinHistoryUsable||!liveJournal.IsValid()||!liveJournal.Size())){
        liveJoinAbandonHost("history_unavailable");return;
    }
    if(networkRole!=1||!networkReady||!campaignJoinPending||campaignJoinSent||!liveRecordSoloActive||
       !campaignGameplay||!campaignMission||liveRecordPending||!liveJournal.IsValid()||!liveJournal.Size())return;
    DWORD flow=readWord(0x69e72c),mode[3]={},transition=readWord(0x6a9988),movie=readWord(0x6a9980);
    // An ended NIS can leave stream state 3 behind during ordinary boss play.
    // Mode/transition zero establishes the idle boundary; only an active movie blocks it.
    if(readWord(flow+8)!=128||readWord(flow+0xc)!=128||!captureGameMode(mode)||mode[0]!=0||
       !transition||readWord(transition+0x2c)!=0||(movie&&readWord(movie+4)==1))return;
    liveJoinDeadline=GetTickCount64()+300000;
    auto checkpoint=liveJoinCheckpoint(0x31001);
    networkWire->SetTimeout(300000);
    coop::live_join::Offer details={};details.tick=networkTick;details.frames=(unsigned)liveJournal.Size();
    details.checkpoint=networkState;details.baseTick=liveJournal.GetMetadata().baseTick;details.partyMode=livePartyMode;
    details.loadSequence=campaignLoadSequence;details.cycle=campaignCycle;details.cycleStartTick=campaignCycleStartTick;
    auto offer=coop::live_join::MakeOffer(details,networkContent);
    std::vector<std::uint8_t> bytes;coop::PhaseState peer={};
    log("LIVE_JOIN FROZEN tick=%u history=%u preserved=1",networkTick,(unsigned)liveJournal.Size());
    if(!liveJournal.Encode(bytes)||(liveJoinTest&&!liveJoinSaveCapture(bytes))||!networkWire->SetDelay(6)||!liveJoinPhase(offer,peer)||peer.id!=0x30000||
       !liveJoinSendBlob(bytes)||!liveJoinPhase(checkpoint,peer)||!liveJoinCheckpointEqual(checkpoint,peer)){
        liveJoinAbandonHost("history_or_checkpoint");return;
    }
    const auto preserved=liveJoinCheckpoint(0x31001);
    if(!liveJoinCheckpointEqual(checkpoint,preserved)){liveJoinAbandonHost("host_changed_while_waiting");return;}
    if(!liveJoinAdaptiveDelay(liveJoinAgreedDelay)){liveJoinAbandonHost("input_delay_negotiation");return;}
    if(livePartyMode==0){if(!liveJoinSetPartyMode(1)){liveJoinAbandonHost("host_handoff");return;}
        livePartyMode=1;liveJoinAdmissionChangedPartyMode=true;}
    checkpoint=liveJoinCheckpoint(0x31002);
    if(!liveJoinPhase(checkpoint,peer)||!liveJoinCheckpointEqual(checkpoint,peer)){
        liveJoinAbandonHost("handoff_checkpoint");return;}
    liveJoinFinishAdmission();
}
