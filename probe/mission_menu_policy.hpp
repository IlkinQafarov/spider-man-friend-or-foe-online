#pragma once
#include "input_wire.hpp"
namespace coop {
inline bool MissionMenuActive(unsigned flow,unsigned mode,unsigned scene,bool movie) {
    // Scene10 is the normal mission HUD, not an input-owning popup.
    return flow==128 && !(movie && scene==16) && (mode!=0 || (scene!=0 && scene!=10));
}
inline void ApplyMissionMenuOwnership(Actions& host,Actions& partner,bool menu,unsigned pauseAction=5) {
    // Pause belongs to the session host, even when the characters have swapped.
    if(pauseAction<partner.size())partner[pauseAction]=0;
    if(menu) {
#ifndef FOF_TEST_ALLOW_PARTNER_MENU
        partner.fill(0);
#endif
        host[13]=0; // character switching has no meaning while operating a dialog
    }
}
}
