#include "progress_sample.hpp"
#include <cstdio>
#include <cstring>
#include <initializer_list>
int main(){
    unsigned char state[0x1fc]={0};auto baseline=coop::ProgressSample(state);
    // Currency, purchase/unlock and collectible fields must be observed.
    const unsigned shared[]={0,0x1a,0xc0,0xc4,0xdb,0xea,0x127,0x128,0x12d,0x130,0x19c,0x1a0,0x1b4,0x1c8,0x1cc,0x1cf};
    for(unsigned offset:shared){state[offset]=1;if(coop::ProgressSample(state)==baseline)return 1;state[offset]=0;}
    // Only these explicitly omitted regions are tested as local/uninitialized.
    for(unsigned offset:{0xbfu,0x12eu,0x12fu,0x19du,0x19eu,0x19fu,0x1bcu,0x1c0u,0x1c4u,0x1e2u,0x1e4u,0x1fbu}){
        state[offset]=123;if(coop::ProgressSample(state)!=baseline)return 2;state[offset]=0;
    }
    std::puts("PROGRESS_SAMPLE_SELFTEST_PASS shared_mutations=16 omitted_bytes=12");return 0;
}
