#include "input_wire.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <limits>
#include <thread>

using coop::Actions;
static Actions sample(unsigned player,unsigned tick){
    Actions result={};result[16+player]=(float)(tick%4)/4.0f;result[10+player]=(tick%7==0)?1.0f:0.0f;return result;
}
static coop::MenuPointer pointerSample(unsigned player,unsigned tick){
    if(tick%5==0)return {};
    return {((tick+player)*1003u)%65536u,tick%2?65535u:0u,coop::menuPointerActive|(tick%3==0?coop::menuPointerClick:0u)};
}
static bool pointerEqual(const coop::MenuPointer& a,const coop::MenuPointer& b){return a.x==b.x&&a.y==b.y&&a.flags==b.flags;}
static bool channelTest(){
    coop::InputWire host,client;coop::Digest build={},scenario={};build[0]=0xeb;scenario[0]=0x21;
    auto accepting=std::async(std::launch::async,[&]{return host.Open(true,"127.0.0.1",49371,build,scenario,3,3000);});
    bool connected=client.Open(false,"127.0.0.1",49371,build,scenario,3,3000),accepted=accepting.get();
    if(!connected||!accepted){printf("FAIL loopback handshake host=%s client=%s\n",host.Error().c_str(),client.Error().c_str());return false;}
    std::atomic<unsigned> finished{0};
    auto peer=[&](unsigned player,coop::InputWire& wire){
        bool ok=true;
        for(unsigned tick=0;tick<180;tick++){
            Actions remote={};coop::MenuPointer localPointer=pointerSample(player,tick),remotePointer={65535,65535,3};std::uint64_t state=0,localState=((std::uint64_t)(player+1)<<32)|tick;if(!wire.Step(tick,sample(player,tick),remote,localState,&state,&localPointer,&remotePointer)){ok=false;break;}
            Actions expected=tick<3?Actions{}:sample(1-player,tick-3);
            if(remote!=expected){ok=false;printf("FAIL delayed actions player=%u tick=%u\n",player,tick);break;}
            std::uint64_t expectedState=tick<3?0:((std::uint64_t)(2-player)<<32)|(tick-3);if(state!=expectedState){ok=false;printf("FAIL delayed state player=%u tick=%u\n",player,tick);break;}
            coop::MenuPointer expectedPointer=tick<3?coop::MenuPointer{}:pointerSample(1-player,tick-3);
            if(!pointerEqual(remotePointer,expectedPointer)){ok=false;printf("FAIL delayed pointer player=%u tick=%u\n",player,tick);break;}
            if((tick+player)%9==0)std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if(ok)ok=wire.Finish();++finished;
        while(finished.load()!=2)std::this_thread::yield();
        return ok;
    };
    auto clientRun=std::async(std::launch::async,[&]{return peer(1,client);});
    bool hostOk=peer(0,host),clientOk=clientRun.get();
    if(!hostOk||!clientOk){printf("FAIL exchange host=%s client=%s\n",host.Error().c_str(),client.Error().c_str());return false;}
    puts("PASS LOOPBACK_180_TICKS_DELAY3_POINTER_AND_GRACEFUL_FINISH");return true;
}
static bool invalidInputTest(){
    coop::InputWire host,client;coop::Digest build={},scenario={};
    auto accepting=std::async(std::launch::async,[&]{return host.Open(true,"127.0.0.1",49374,build,scenario,3,3000);});
    bool joined=client.Open(false,"127.0.0.1",49374,build,scenario,3,3000),accepted=accepting.get();if(!joined||!accepted)return false;
    Actions invalid={},remote={};invalid[4]=std::numeric_limits<float>::quiet_NaN();bool rejected=!host.Step(0,invalid,remote)&&host.Error()=="Invalid local input sequence";
    printf("%s INVALID_SAMPLE_REJECTED\n",rejected?"PASS":"FAIL");return rejected;
}
static bool mismatchTest(bool scenarioMismatch){
    coop::InputWire host,client;coop::Digest build={},otherBuild={},scenario={},otherScenario={};
    if(scenarioMismatch)otherScenario[2]=1;else otherBuild[2]=1;
    unsigned short port=scenarioMismatch?49373:49372;
    auto accepting=std::async(std::launch::async,[&]{return host.Open(true,"127.0.0.1",port,build,scenario,3,3000);});
    bool clientAccepted=client.Open(false,"127.0.0.1",port,otherBuild,otherScenario,3,3000),hostAccepted=accepting.get();
    bool ok=!clientAccepted&&!hostAccepted;
    printf("%s %s_MISMATCH_REJECTED\n",ok?"PASS":"FAIL",scenarioMismatch?"SCENARIO":"BUILD");return ok;
}
int main(){
    bool exchange=channelTest(),build=mismatchTest(false),scenario=mismatchTest(true),invalid=invalidInputTest();
    if(exchange&&build&&scenario&&invalid){puts("OWN_WIRE_SELFTEST_PASS (transport only; no game synchronization tested)");return 0;}
    return 1;
}
