#pragma once
#include <cstdint>
#include <cstddef>
namespace coop {
// Exact-build serialized progress fields. Native init4D775A and purchase/unlock
// routines4D79CD/4D7A16/4D7AC4 initialize/read these spans. Do not hash the
// enclosing struct raw: it includes padding (including byte0xBF after the
// 0xA5-byte upgrade array), local settings and save date/time.
inline std::uint32_t ProgressSample(const unsigned char* state) {
    // +1CC is the campaign flags DWORD initialized at4D7858 and read at507C19.
    const std::size_t spans[][2]={{0,0xbf},{0xc0,0x12e},{0x130,0x19d},{0x1a0,0x1b8},{0x1c8,0x1d0}};
    std::uint32_t hash=2166136261u;
    for(const auto& span:spans)for(std::size_t i=span[0];i<span[1];++i)hash=(hash^state[i])*16777619u;
    return hash;
}
}
