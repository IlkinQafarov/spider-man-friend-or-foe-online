#include "mission_menu_policy.hpp"
#include <cstdio>

int main() {
    struct Case { unsigned flow,mode,scene; bool movie,active; };
    const Case cases[] = {
        {128,0,14,false,true}, {128,1,0,false,true},
        {128,0,0,false,false}, {128,0,10,false,false}, {128,1,10,false,true}, {128,5,16,true,false},
        {128,1,16,false,true}, {128,0,8,false,true},
        {16,1,14,false,false}, {32,1,14,false,false},
        {128,0,16,true,false}, {128,5,0,true,true}
    };
    for(const auto& c:cases) if(coop::MissionMenuActive(c.flow,c.mode,c.scene,c.movie)!=c.active) {
        std::puts("MISSION_MENU_POLICY_FAIL native phase/movie classification"); return 1;
    }
    // Same session host on either endpoint, even after the avatars swap.
    for(unsigned role=1;role<=2;++role) for(unsigned avatar=0;avatar<=1;++avatar) for(bool menu:{false,true}) {
        coop::Actions hostSource={},peerSource={};
        for(unsigned i=0;i<hostSource.size();++i) {hostSource[i]=float(i)+0.25f;peerSource[i]=float(i)+0.75f;}
        coop::Actions local=role==1?hostSource:peerSource,remote=role==1?peerSource:hostSource;
        coop::Actions& host=role==1?local:remote;
        coop::Actions& peer=role==1?remote:local;
        coop::ApplyMissionMenuOwnership(host,peer,menu);
        coop::Actions slots[2]; slots[avatar]=host;slots[1-avatar]=peer;
        for(unsigned i=0;i<host.size();++i) {
            const float wantHost=menu&&i==13?0.0f:hostSource[i];
            const float wantPeer=menu||i==5?0.0f:peerSource[i];
            if(slots[avatar][i]!=wantHost||slots[1-avatar][i]!=wantPeer) {
                std::printf("MISSION_MENU_POLICY_FAIL role=%u host_avatar=%u menu=%u action=%u\n",role,avatar,menu?1u:0u,i);return 1;
            }
        }
    }
    coop::Actions host={},peer={};peer.fill(1);
    coop::ApplyMissionMenuOwnership(host,peer,false,8);
    if(peer[8]!=0||peer[5]!=1){std::puts("MISSION_MENU_POLICY_FAIL native pause remap");return 1;}
    std::puts("MISSION_MENU_POLICY_PASS phases=12 endpoints/avatar/menu=8 actions=28 native-pause-remap");return 0;
}
