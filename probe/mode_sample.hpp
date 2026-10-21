#pragma once
#include <cstdint>

namespace coop {
struct ModeSample {
    std::uint32_t mode;
    std::uint32_t previous;
    std::uint32_t owner;
};

// Exact-build mode manager at *(6A9988): current +2C, previous +30,
// pause owner +5C. Previous is live only for pause restoration4E229B and
// NIS stop4E09BE. Mode4 at4E25FA..4E263B waits for the area script and
// directly writes current=0 at4E262D, without reading previous. Normal
// mode0 transitions at4E207A overwrite previous, so its boot history is
// irrelevant outside modes1/5. Never hash that inactive storage raw.
inline bool CanonicalModeSample(std::uint32_t mode, std::uint32_t previous,
                                std::uint32_t owner, ModeSample& out) {
    constexpr std::uint32_t inactive = 0xffffffffu;
    if (mode > 6 || ((mode == 1 || mode == 5) && previous > 6)) return false;
    // The owner has meaning only while paused. Other modes may retain stale
    // storage here; their canonical sample must not depend on that history.
    if (mode == 1 && owner != inactive && owner > 3) return false;
    out = {mode, (mode == 1 || mode == 5) ? previous : inactive,
           mode == 1 ? owner : inactive};
    return true;
}
}
