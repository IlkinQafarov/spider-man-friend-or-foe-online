// Research execution proof for the live-join timeline. Player builds reject these flags.
static DWORD liveJoinReplayArea(){
    const auto& meta=liveJournal.GetMetadata();
    for(unsigned i=0;i<103;i++){DWORD descriptor=readWord(0x584928+i*4),id=0;char path[33]={};
        if(campaignArea(descriptor,path,id)&&!strcmp(path,meta.areaPath.data()))return descriptor;}
    return 0;
}
static bool liveJoinResearchStartAtHub(){
    DWORD flow=readWord(0x69e72c),manager=readWord(0x6a9988);
    if(campaignMission||!flow||readWord(flow+8)!=16||readWord(flow+0xc)!=16||!manager||readWord(manager+0x2c)!=0)return false;
    if(liveReplayPairedBootstrap()&&!campaignHubPlayer2){
        campaignHubPairedCalls=60;campaignHubPartnerUpdate();if(!campaignHubPlayer2)return false;
    }
    const char* wanted=liveReplayRequested?liveJournal.GetMetadata().areaPath.data():"Tokyo/1_Level/1_Area";
    DWORD area=0,id=0;char path[33]={};
    for(unsigned i=0;i<103;i++){DWORD candidate=readWord(0x584928+i*4),candidateId=0;char candidatePath[33]={};
        if(campaignArea(candidate,candidatePath,candidateId)&&!strcmp(candidatePath,wanted)){area=candidate;id=candidateId;strcpy(path,candidatePath);break;}}
    if(!area)campaignReject("journal bootstrap mission identity");
    *(DWORD*)(readWord(0x6a9974)+0x1c)=area;*(DWORD*)0x69ecec=2;*(DWORD*)(flow+0xc)=0x20;
    campaignMission=true;campaignHubPartnerReset();strcpy(campaignMissionPath,path);campaignMissionArea=id;
    log("LIVE_JOURNAL NATIVE_MISSION_START path=%s area=%lu",path,id);return true;
}
static bool liveJoinLoadJournal(){
    HANDLE file=CreateFileW(liveReplaySource,GENERIC_READ,FILE_SHARE_READ,0,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,0);
    if(file==INVALID_HANDLE_VALUE)return false;
    LARGE_INTEGER size={};bool ok=GetFileSizeEx(file,&size)&&size.QuadPart>0&&
        size.QuadPart<=(LONGLONG)(coop::live_join::kMetadataRecordBytes+4+coop::live_join::kMaxFrames*coop::live_join::kFrameRecordBytes);
    std::vector<std::uint8_t> bytes;
    if(ok){try{bytes.resize((size_t)size.QuadPart);}catch(...){ok=false;}}
    DWORD got=0;if(ok)ok=ReadFile(file,bytes.data(),(DWORD)bytes.size(),&got,0)&&got==bytes.size();CloseHandle(file);
    if(!ok||!liveJournal.Decode(bytes.data(),bytes.size())||!liveJournal.Size()||
       liveJournal.GetMetadata().partyMode>coop::live_join::kPartyModeTwoHumans)return false;
    DWORD sidekick=0;memcpy(&sidekick,liveJournal.GetMetadata().missionStartProgress.data()+0x1b4,4);
    if(sidekick!=liveJournal.GetMetadata().sidekick)return false;
    log("LIVE_REPLAY LOADED frames=%u base=%u area=%s",(unsigned)liveJournal.Size(),liveJournal.GetMetadata().baseTick,liveJournal.GetMetadata().areaPath.data());return true;
}
static void liveJoinRecordStart(const char* path,DWORD id){
    const bool solo=liveRecordSolo||liveRecordSoloActive;
    if((!liveRecordTest&&!solo&&!liveJoinTest)||(!solo&&networkRole!=1)||(solo&&networkRole!=0&&!liveJoinTest)||
       id==97||id!=campaignMissionArea||strcmp(path,campaignMissionPath))return;
    coop::live_join::Metadata meta={};strcpy_s(meta.areaPath.data(),meta.areaPath.size(),path);
    meta.baseTick=networkTick;meta.seed=readWord(0x58cf24);meta.sidekick=readWord(0x69c624);
    meta.partyMode=solo?coop::live_join::kPartyModeNativeAi:coop::live_join::kPartyModeTwoHumans;
    if(!readMemory((void*)0x69c470,meta.missionStartProgress.data(),meta.missionStartProgress.size()))campaignReject("journal initial progress read");
    liveJournal=coop::live_join::Journal(meta);liveRecordFinished=false;liveRecordPending=false;
    if(!liveJournal.IsValid())campaignReject("journal initial metadata invalid");
    log("LIVE_RECORD BEGIN base=%u area=%s seed=%u",meta.baseTick,path,meta.seed);
}
static bool liveJoinSoloBeforeLoad(const char* path,DWORD id){
    const bool timelineSolo=liveJoinTest&&(networkDeferred||liveRecordSoloActive)&&networkRole!=2&&!liveReplayRequested;
    if(!liveRecordSolo&&!timelineSolo)return false;
    if(id==97){
        if(timelineSolo&&liveRecordSoloActive){
            liveRecordSoloActive=false;networkInputClock=false;networkTick=0;networkState=0;liveWireBaseTick=0;
            campaignPrepared=campaignMission=campaignGameplay=false;liveJournal=coop::live_join::Journal();
            liveRecordPending=liveRecordFinished=false;livePartyMode=0;networkSwapped=networkSwapLatched=false;networkSwapHeld=0;
        }
        log("LIVE_RECORD SOLO native hub load id=97");return true;
    }
    if(!path||!path[0]||strlen(path)>=sizeof(campaignMissionPath))campaignReject("solo mission path invalid");
    if(liveRecordSoloActive){
        if(!liveJournal.IsValid()||!campaignPrepared||!networkInputClock||liveRecordFinished)
            campaignReject("solo area load lost prepared journal state");
        if(liveRecordPending)campaignReject("solo area load before prior state observation");
        log("LIVE_RECORD SOLO area load path=%s id=%lu tick=%u keep_clock_rng_journal=1",path,id,networkTick);
        return true;
    }
    DWORD flow=readWord(0x69e72c);
    if(!campaignMode||(!timelineSolo&&(!liveRecordTest||!liveRecordSolo||networkWire))||networkRole||networkReady||networkInputClock||
       networkTick||networkFailed||networkComplete||campaignPrepared||liveReplayRequested||!flow||readWord(flow+8)!=64)
        campaignReject("solo first load lifecycle guards");
    if(liveJournal.IsValid()||liveRecordPending||liveRecordFinished)
        campaignReject("solo journal already initialized");
    if(campaignMission&&(campaignMissionArea!=id||strcmp(path,campaignMissionPath)))
        campaignReject("solo selected mission identity changed before load");
    if(!prepareTaskTiming())campaignReject("solo native task timing preparation");
    networkTick=0;networkState=0;networkInputClock=true;
    for(unsigned slot=0;slot<4;slot++)*(DWORD*)(0x691170+slot*0x11c+0x110)=10000;
    *(DWORD*)0x6a98dc=10000;*(DWORD*)0x58cf24=8466;networkAmbientRng=0x84f19a21;
    campaignPrepared=true;campaignMission=true;campaignMissionArea=id;
    if(strcpy_s(campaignMissionPath,sizeof(campaignMissionPath),path))campaignReject("solo mission path copy");
    livePartyMode=0;liveRecordSoloActive=true;liveJoinRecordStart(path,id);
    if(!liveJournal.IsValid())campaignReject("solo journal did not start");
    liveRecordSoloActive=true;
    log("LIVE_RECORD SOLO READY tick=%u clock=10000 seed=%u ambient=%08x path=%s id=%lu native_ai=1",
        networkTick,readWord(0x58cf24),networkAmbientRng,path,id);
    return true;
}
static void liveJoinRecordInput(const coop::Actions& host,const coop::Actions& partner,unsigned status,unsigned slot){
    const bool solo=liveRecordSoloActive;
    if((!liveRecordTest&&!solo&&!liveJoinTest)||(!solo&&networkRole!=1)||(solo&&(!liveRecordSolo&&!liveJoinTest))||
       liveRecordFinished||!liveJournal.IsValid())return;
    if(solo&&livePartyMode==coop::live_join::kPartyModeNativeAi){
        if(slot!=0)campaignReject("solo journal host slot changed");
        for(float value:partner)if(value!=0.0f)campaignReject("solo journal contains partner actions");
    }
    if(liveRecordPending)campaignReject("journal input advanced without native simulation");
    livePendingFrame={};livePendingFrame.tick=networkTick;livePendingFrame.host=host;livePendingFrame.partner=partner;
    livePendingFrame.agreedStatus=status;livePendingFrame.hostSlot=slot;livePendingFrame.pointer=appliedMenuPointer;
    livePendingFrame.partyMode=solo?livePartyMode:coop::live_join::kPartyModeTwoHumans;
    liveRecordPending=true;
}
static void liveJoinSoloInput(){
    if(!liveRecordSoloActive)return;
    if((!liveRecordSolo&&!liveJoinTest)||(!liveJoinTest&&(networkRole||networkWire))||!campaignMode||!campaignPrepared||
       !networkInputClock||!liveJournal.IsValid())
        campaignReject("solo input lifecycle guards");
    if(networkFailed||networkComplete||liveRecordFinished){applyNetworkActions(0,{});applyNetworkActions(1,{});
        if(networkFailed&&gameWindow&&!networkCloseRequested){networkCloseRequested=true;log("NET failure requests normal close of owned solo game window");PostMessageW(gameWindow,WM_CLOSE,0,0);}return;}
    DWORD flow=readWord(0x69e72c);
    if(!flow||readWord(flow+8)!=128||!campaignGameplay)
        campaignReject("solo input outside prepared gameplay");
    if(livePartyTransitionTest){
        const unsigned target=livePartyTransitionStage==0?900u:1200u;
        if(livePartyTransitionStage<2&&networkTick>=target){
            const unsigned mode=livePartyTransitionStage==0?1u:0u;
            if(liveJoinSetPartyMode(mode)){livePartyMode=mode;++livePartyTransitionStage;}
            else if(networkTick>=target+300)campaignReject("research native party handoff readiness timeout");
        }
    }
    if(liveJoinTest&&livePartyMode==1&&!networkSwapped&&liveJoinSetPartyMode(0))livePartyMode=0;
    coop::Actions host=campaignFixture?physicalFixtureActions(false):captureLocalActions(),partner={};
    if(livePartyTransitionTest&&livePartyMode==coop::live_join::kPartyModeTwoHumans)partner=networkBotActions(1,networkTick);
    bool nis=localNisLoaded();static bool previousNis=true;
    if(previousNis&&!nis)nisLocalLoadStart=networkTick;previousNis=nis;
    host[networkNisAction]=readyBits(nis,localExitUnlocked(),localNextAreaLoaded(),
                                     areaChangePending!=0,localStreamDone());
    const unsigned status=readyBitsOf(host[networkNisAction]);host[networkNisAction]=0;
    networkNisAgreed=(status&4)!=0;if(!networkNisAgreed)nisAgreedNotReadyAt=networkTick;
    networkAreaChangeAgreed=(status&8)!=0;networkStreamDoneAgreed=(status&16)!=0;networkExitAgreed=status&3;
    DWORD mode[3]={};if(!captureGameMode(mode))campaignReject("solo native menu mode unreadable");
    DWORD sceneManager=readWord(0x6a9940);if(!sceneManager)campaignReject("solo native scene manager unreadable");
    DWORD scene=readWord(sceneManager+0x4e4),movie=readWord(0x6a9980);
    const bool menu=coop::MissionMenuActive(128,mode[0],scene,movie&&readWord(movie+4)==1);
    appliedMenuPointer=localMenuPointer;localMenuPointer.flags&=~coop::menuPointerClick;applyMissionPointer();
    if(menu&&(appliedMenuPointer.flags&coop::menuPointerClick))host[10]=1.0f;
    DWORD bindings=readWord(0x69e56c);if(!bindings)campaignReject("solo native bindings unavailable");
    DWORD pauseAction=readWord(bindings+14*4);
    if(pauseAction>=28)campaignReject("solo native pause mapping unavailable");
    coop::ApplyMissionMenuOwnership(host,partner,menu,pauseAction);
    const unsigned hostSlot=livePartyMode==0?0u:networkSwapped?1u:0u;
    applyNetworkActions(hostSlot,host);applyNetworkActions(1-hostSlot,partner);
    liveJoinRecordInput(host,partner,status,hostSlot);++networkTick;
}
static void liveJoinResearchClose(){
    networkComplete=true;applyNetworkActions(0,{});applyNetworkActions(1,{});
    if(!gameWindow||!PostMessageW(gameWindow,WM_CLOSE,0,0)){networkFailed=true;log("NET FAIL journal normal close request");}
}
static void liveJoinAfterState(unsigned tick){
    if(liveReplayRunning&&!networkComplete){
        const auto* frame=liveJournal.At(liveReplayIndex);
        if(!frame||frame->tick!=tick||frame->postState!=networkState){
            log("LIVE_REPLAY MISMATCH frame=%u tick=%u expected=%016llx actual=%016llx",liveReplayIndex,tick,frame?frame->postState:0,networkState);liveJoinResearchClose();return;
        }
        ++liveReplayIndex;
        if(liveReplayIndex==liveJournal.Size()){log("LIVE_REPLAY MATCH frames=%u",liveReplayIndex);if(liveAdmissionClient)liveReplayFinished=true;else liveJoinResearchClose();}
    }
    if(liveRecordPending){
        if(tick!=livePendingFrame.tick)campaignReject("journal native simulation tick mismatch");
        livePendingFrame.postState=networkState;if(!liveJournal.Append(livePendingFrame))campaignReject("journal append rejected");liveRecordPending=false;
        if(liveRecordTest&&liveJournal.Size()==1800){
            std::vector<std::uint8_t> encoded;if(!liveJournal.Encode(encoded))campaignReject("journal serialization failed");
            wchar_t path[32768];_snwprintf(path,32768,L"%s\\live-join-journal.bin",sandbox);
            HANDLE file=CreateFileW(path,GENERIC_WRITE,0,0,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,0);DWORD written=0;
            bool ok=file!=INVALID_HANDLE_VALUE&&WriteFile(file,encoded.data(),(DWORD)encoded.size(),&written,0)&&written==encoded.size();
            if(file!=INVALID_HANDLE_VALUE){if(!FlushFileBuffers(file))ok=false;if(!CloseHandle(file))ok=false;}
            if(!ok)campaignReject("journal sandbox write failed");liveRecordFinished=true;
            log("LIVE_RECORD SAVED frames=%u bytes=%u",(unsigned)liveJournal.Size(),(unsigned)encoded.size());
            if(liveRecordSolo&&liveRecordSoloActive)liveJoinResearchClose();
        }
    }
}
static bool liveJoinReplayBeforeLoad(const char* path,DWORD id){
    const auto& meta=liveJournal.GetMetadata();BYTE* progress=(BYTE*)0x69c470;
    if(id==97||!liveReplayRunning){
        memcpy(progress,meta.missionStartProgress.data(),0x1bc);memcpy(progress+0x1c8,meta.missionStartProgress.data()+0x1c8,0x1e4-0x1c8);
    }
    if(id==97){
        if(meta.partyMode==coop::live_join::kPartyModeTwoHumans&&(!prepareNetworkPlayers()||!prepareTaskTiming()))
            campaignReject("paired replay native hub preparation");
        return true;
    }
    if(!liveReplayRunning){
        if(strcmp(path,meta.areaPath.data())||readWord(readWord(0x69e72c)+8)!=64)campaignReject("replay initial native load identity");
        bool playersReady=meta.partyMode==coop::live_join::kPartyModeNativeAi||prepareNetworkPlayers();
        bool timingReady=prepareTaskTiming();
        if(!playersReady||!timingReady)campaignReject("replay native preparation");
        networkTick=meta.baseTick;networkState=0;networkInputClock=true;networkReady=true;campaignPrepared=true;livePartyMode=meta.partyMode;
        campaignMission=true;campaignMissionArea=id;strcpy(campaignMissionPath,path);
        DWORD clock=10000+(DWORD)((ULONGLONG)networkTick*1000/60);
        for(unsigned slot=0;slot<4;slot++)*(DWORD*)(0x691170+slot*0x11c+0x110)=clock;*(DWORD*)0x6a98dc=clock;
        *(DWORD*)0x58cf24=meta.seed;networkAmbientRng=0x84f19a21;liveReplayRunning=true;
        log("LIVE_REPLAY BEGIN base=%u area=%s seed=%u",networkTick,path,meta.seed);
    }
    return true;
}
static void liveJoinReplayInput(){
    if(networkComplete||networkFailed)return;
    const auto* frame=liveJournal.At(liveReplayIndex);
    if(!frame||frame->tick!=networkTick){log("LIVE_REPLAY MISMATCH frame=%u reason=input_tick",liveReplayIndex);liveJoinResearchClose();return;}
    if(frame->partyMode!=livePartyMode){
        if(liveJoinTest)liveTraceFrom=networkTick>2?networkTick-2:0;
        if(!liveJoinSetPartyMode(frame->partyMode))campaignReject("replay native party handoff readiness mismatch");
        livePartyMode=frame->partyMode;
    }
    unsigned status=frame->agreedStatus;networkNisAgreed=(status&4)!=0;
    if(!networkNisAgreed)nisAgreedNotReadyAt=networkTick;
    networkAreaChangeAgreed=(status&8)!=0;networkStreamDoneAgreed=(status&16)!=0;networkExitAgreed=status&3;
    appliedMenuPointer=frame->pointer;applyMissionPointer();networkSwapped=frame->hostSlot!=0;
    applyNetworkActions(frame->hostSlot,frame->host);applyNetworkActions(1-frame->hostSlot,frame->partner);++networkTick;
}
