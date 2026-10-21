#include "mode_sample.hpp"
#include <cstdio>
#include <initializer_list>

using U32 = std::uint32_t;
using Sample = coop::ModeSample;
using Sampler = bool (*)(U32, U32, U32, Sample&);
constexpr U32 inactive = 0xffffffffu;

static bool equal(const Sample& a, const Sample& b) {
    return a.mode == b.mode && a.previous == b.previous && a.owner == b.owner;
}

// Run identical oracles against the real helper and each deliberate mutant.
static unsigned failures(Sampler sample) {
    unsigned failed = 0;
    Sample host{}, peer{}, paused{}, other{}, nis{};
    // Test030's real mode4 contradiction: boot history5 versus1 has no
    // subsequent effect because4E262D overwrites current without reading it.
    if (!sample(4, 5, inactive, host) || !sample(4, 1, inactive, peer) ||
        !equal(host, peer) || host.previous != inactive) ++failed;
    if (!sample(0, 5, 0, host) || !sample(0, 1, 3, peer) ||
        !equal(host, peer) || host.previous != inactive || host.owner != inactive) ++failed;
    if (!sample(1, 0, 0, paused) || !sample(1, 5, 0, other) ||
        equal(paused, other) || paused.previous != 0 || other.previous != 5) ++failed;
    if (!sample(1, 0, 1, other) || equal(paused, other) || other.owner != 1) ++failed;
    if (!sample(1, 0, inactive, other) || equal(paused, other)) ++failed;
    if (!sample(5, 0, 0, nis) || !sample(5, 4, 3, other) || equal(nis, other) ||
        nis.previous != 0 || other.previous != 4 || nis.owner != inactive) ++failed;
    if (!sample(5, 0, 3, other) || !equal(nis, other)) ++failed;
    // Inactive fields are intentionally not validated: they can contain old
    // history. Active state retains strict range checks and failure is atomic.
    for (U32 mode : {0u, 2u, 3u, 4u, 6u}) {
        if (!sample(mode, inactive, 999, host) || host.mode != mode ||
            host.previous != inactive || host.owner != inactive) ++failed;
    }
    for (U32 mode : {7u, inactive}) {
        host = {99, 98, 97};
        if (sample(mode, 0, inactive, host) || !equal(host, {99, 98, 97})) ++failed;
    }
    for (U32 mode : {1u, 5u}) {
        for (U32 previous : {7u, inactive}) {
            host = {99, 98, 97};
            if (sample(mode, previous, inactive, host) || !equal(host, {99, 98, 97})) ++failed;
        }
    }
    for (U32 owner : {4u, 7u, 0xfffffffeu}) {
        host = {99, 98, 97};
        if (sample(1, 0, owner, host) || !equal(host, {99, 98, 97})) ++failed;
    }
    for (U32 owner : {0u, 1u, 2u, 3u, inactive}) {
        if (!sample(1, 6, owner, host) || host.previous != 6 || host.owner != owner) ++failed;
    }
    return failed;
}

static bool retainInactive(U32 mode, U32 previous, U32 owner, Sample& out) {
    if (!coop::CanonicalModeSample(mode, previous, owner, out)) return false;
    out.previous = previous; // Wrong: includes inactive, stale boot history.
    return true;
}

static bool discardActive(U32 mode, U32 previous, U32 owner, Sample& out) {
    if (!coop::CanonicalModeSample(mode, previous, owner, out)) return false;
    out.previous = inactive; // Wrong: masks actual pause/NIS restoration state.
    return true;
}

int main() {
    const unsigned real = failures(coop::CanonicalModeSample);
    const unsigned staleTwin = failures(retainInactive);
    const unsigned activeTwin = failures(discardActive);
    if (real || !staleTwin || !activeTwin) {
        std::printf("MODE_SAMPLE_SELFTEST_FAIL actual=%u retain_inactive_twin=%u discard_active_twin=%u\n",
                    real, staleTwin, activeTwin);
        return 1;
    }
    std::puts("MODE_SAMPLE_SELFTEST_PASS canonical_history/active_pause/active_nis/ranges/atomic_failure twins_caught=2");
    return 0;
}
