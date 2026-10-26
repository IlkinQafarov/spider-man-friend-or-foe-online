// In-place native conversion of the existing sidekick between AI and logical P2.
// The caller updates livePartyMode only after this function returns true.
struct LiveJoinPartyActorState {
    DWORD actor, serial, vtable, controller, aiController, activeController;
    DWORD slot, provider, transform, position[4], health[2];
};
struct LiveJoinPartySnapshot {
    DWORD manager, area, areaId, weak[2][3], partyCount, flow, flowPending, primaryIndex;
    DWORD actorListCount, actorList[128][3];
    LiveJoinPartyActorState actors[2];
};

static bool liveJoinPartyReadActor(DWORD actor, DWORD serial, LiveJoinPartyActorState& out) {
    out={}; out.actor=actor; out.serial=serial;
    DWORD header=0;
    if(!actor||serial==~0u||!readMemory((void*)(actor+8),&header,4)||!header||
       !readMemory((void*)(header+8),&out.serial,4)||out.serial!=serial||
       !readMemory((void*)actor,&out.vtable,4)||out.vtable!=0x564528||
       !readMemory((void*)(actor+0x2dc),&out.controller,4)||!out.controller||
       !readMemory((void*)(actor+0x2e0),&out.aiController,4)||!out.aiController||
       !readMemory((void*)(actor+0x2d8),&out.activeController,4)||
       !readMemory((void*)(out.controller+0xc0),&out.slot,4)||
       !readMemory((void*)(out.controller+0xc4),&out.provider,4)||
       !readMemory((void*)(actor+0x1d0),&out.transform,4)||!out.transform||
       readWord(out.vtable+0x2f4)!=0x45a6c9||readWord(out.vtable+0x21c)!=0x45119b||
       !readMemory((void*)out.transform,out.position,sizeof(out.position))||
       !readMemory((void*)(actor+0x398),&out.health[0],4)||
       !readMemory((void*)(actor+0x39c),&out.health[1],4))return false;
    return true;
}

static bool liveJoinPartyCapture(LiveJoinPartySnapshot& out) {
    out={};
    out.manager=readWord(0x6a9900);
    DWORD flowAddress=readWord(0x69e72c), areas=readWord(0x6a9974);
    char areaPath[33]={};
    if(!out.manager||!flowAddress||!areas||
       !readMemory((void*)(flowAddress+8),&out.flow,4)||
       !readMemory((void*)(flowAddress+0xc),&out.flowPending,4)||
       !readMemory((void*)(areas+0x18),&out.area,4)||!out.area||
       !campaignArea(out.area,areaPath,out.areaId))return false;
    if(!readMemory((void*)(out.manager+0x48),&out.partyCount,4))return false;
    for(unsigned p=0;p<2;p++){
        if(!readMemory((void*)(out.manager+0x128+p*12),out.weak[p],sizeof(out.weak[p]))||
           !out.weak[p][1]||out.weak[p][2]==~0u||
           readWord(out.manager+0x12c+p*12)!=out.weak[p][1]||
           !liveJoinPartyReadActor(out.weak[p][1],out.weak[p][2],out.actors[p]))return false;
    }
    out.primaryIndex=~0u;
    for(unsigned p=0;p<2;p++)if(out.actors[p].slot==0){
        if(out.primaryIndex!=~0u)return false;
        out.primaryIndex=p;
    }
    if(out.primaryIndex==~0u)return false;
    const LiveJoinPartyActorState& other=out.actors[1u-out.primaryIndex];
    if(other.slot!=~0u&&other.slot!=1)return false;
    DWORD node=0,visitedNodes[128]={};
    if(!readMemory((void*)(out.manager+0x34),&node,4))return false;
    while(node){
        if(out.actorListCount>=128)return false;
        for(DWORD i=0;i<out.actorListCount;i++)if(visitedNodes[i]==node)return false;
        visitedNodes[out.actorListCount]=node;
        if(!readMemory((void*)node,out.actorList[out.actorListCount],sizeof(out.actorList[0])))return false;
        ++out.actorListCount;node=out.actorList[out.actorListCount-1][0];
    }
    return out.actorListCount!=0;
}

static bool liveJoinPartySnapshotEqual(const LiveJoinPartySnapshot& before,
                                       const LiveJoinPartySnapshot& after,
                                       unsigned primaryIndex,
                                       unsigned from, unsigned to) {
    if(before.manager!=after.manager||before.area!=after.area||before.areaId!=after.areaId||
       before.flow!=after.flow||before.flowPending!=after.flowPending||
       before.primaryIndex!=primaryIndex||after.primaryIndex!=primaryIndex||
       before.actorListCount!=after.actorListCount||
       memcmp(before.actorList,after.actorList,sizeof(before.actorList))||
       memcmp(before.weak,after.weak,sizeof(before.weak)))return false;
    if(after.partyCount!=(to?2u:1u))return false;
    for(unsigned p=0;p<2;p++){
        const LiveJoinPartyActorState& a=before.actors[p];
        const LiveJoinPartyActorState& b=after.actors[p];
        if(a.actor!=b.actor||a.serial!=b.serial||a.vtable!=b.vtable||
           a.controller!=b.controller||a.aiController!=b.aiController||
           a.transform!=b.transform||memcmp(a.position,b.position,sizeof(a.position))||
           memcmp(a.health,b.health,sizeof(a.health)))return false;
        if(p==primaryIndex){
            if(memcmp(&a,&b,sizeof(a)))return false;
        }else if(from==0&&to==1){
            if(b.slot!=1||!b.provider||b.activeController!=b.controller)return false;
        }else if(from==1&&to==0){
            if(b.slot!=~0u||b.provider||b.activeController!=b.aiController)return false;
        }else return false;
    }
    return true;
}

static bool liveJoinSetPartyMode(unsigned mode) {
    if(mode>1){log("LIVE_PARTY MODE_REJECT tick=%u reason=invalid_mode mode=%u",networkTick,mode);return false;}
    LiveJoinPartySnapshot before={};
    DWORD flow=readWord(0x69e72c),transition=readWord(0x6a9988),nativeMode[3]={};
    DWORD movie=readWord(0x6a9980);
    if(!flow||!transition||!captureGameMode(nativeMode)||nativeMode[0]!=0||
       readWord(flow+8)!=128||readWord(flow+0xc)!=128||readWord(transition+0x2c)!=0||
       !localStreamDone()||(movie&&readWord(movie+4)==1)||
       !liveJoinPartyCapture(before)){
        log("LIVE_PARTY MODE_BUSY tick=%u reason=mission_not_idle_or_party_unreadable flow=%lu/%lu mode=%lu stage=%lu nis_loaded=%u stream_done=%u movie=%lu actors=%lu",networkTick,
            readWord(flow+8),readWord(flow+0xc),nativeMode[0],readWord(transition+0x2c),localNisLoaded(),localStreamDone(),readWord(movie+4),before.actorListCount);return false;
    }
    const unsigned primaryIndex=before.primaryIndex,otherIndex=1u-primaryIndex;
    const LiveJoinPartyActorState& primary=before.actors[primaryIndex];
    const LiveJoinPartyActorState& other=before.actors[otherIndex];
    unsigned from=0;
    if(primary.slot!=0||primary.provider==0||primary.activeController!=primary.controller||
       (other.slot==~0u?(other.provider!=0||other.activeController!=other.aiController):
                        (other.provider==0||other.activeController!=other.controller))||
       before.partyCount!=(other.slot==1?2u:1u)){
        log("LIVE_PARTY MODE_BUSY tick=%u reason=unexpected_native_slots",networkTick);return false;
    }
    from=other.slot==1?1u:0u;
    if(from==mode){log("LIVE_PARTY MODE_BUSY tick=%u reason=already_mode mode=%u",networkTick,mode);return false;}
    // 45666D obtains the provider from the current row of the 6A98F4 array,
    // rather than the unrelated adjacent global at 6A98F8.
    DWORD providers=readWord(0x6a98f4),stride=readWord(providers+4),row=readWord(providers+0xc),items=readWord(providers+0x10);
    if(mode==1&&(!providers||stride<2||stride>32||row>32||!items||!readWord(items+(row*stride+1)*4))){
        log("LIVE_PARTY MODE_BUSY tick=%u reason=slot1_provider_unavailable",networkTick);return false;
    }
    DWORD manager=before.manager,slot=1;
    if(mode==1){
        // Selecting a previously unused device runs the game's own drop-in path.
        // If it was already selected, 40182D returns early, so invoke the native
        // party operation explicitly in that case.
        if(!prepareNetworkPlayers())campaignReject("native in-place party device preparation");
        if(readWord(other.controller+0xc0)!=1){
            __asm {
                push esi
                mov esi,manager
                push slot
                mov eax,0x45d02e
                call eax
                pop esi
            }
            ((void(__cdecl*)())0x4e3e69)();
        }
    }else{
        __asm {
            mov eax,manager
            push slot
            mov ecx,0x45d0f9
            call ecx
        }
    }
    LiveJoinPartySnapshot after={};
    if(!liveJoinPartyCapture(after)||!liveJoinPartySnapshotEqual(before,after,primaryIndex,from,mode))
        campaignReject("native in-place party handoff changed actor/world invariants");
    log("LIVE_PARTY MODE tick=%u from=%u to=%u preserved=1",networkTick,from,mode);
    return true;
}
