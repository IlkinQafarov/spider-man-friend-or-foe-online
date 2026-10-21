#include "input_key_names.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using U32 = std::uint32_t;
static coop::KeyName name(const char16_t* value) {
    coop::KeyName out;
    while (value[out.length]) { out.text[out.length] = value[out.length]; ++out.length; }
    return out;
}

// Deliberate mutation twins share the exact boundary/absence oracle.
enum class Twin { None, Unbounded, MissingAsZero };
static bool lookupChecks(Twin twin) {
    const std::vector<coop::KeyName> names{name(u"Key_W"), name(u"Key_A"), name(u"Key_Return")};
    bool valid = true;
    for (unsigned target = 0; target < names.size(); ++target) {
        U32 out = 999, calls = 0;
        auto get = [&](U32 i, coop::KeyName& n) { ++calls; if (i >= names.size()) return false; n = names[i]; return true; };
        valid &= coop::FindKeyName(U32(names.size()), names[target], get, out) == coop::KeyLookup::Found;
        valid &= out == target && calls == target + 1;
    }
    U32 out = 999, calls = 0, badIndex = 0;
    auto get = [&](U32 i, coop::KeyName& n) {
        ++calls;
        if (i >= names.size()) { ++badIndex; return false; }
        n = names[i]; return true;
    };
    auto result = coop::FindKeyName(U32(names.size()), name(u"Key_Absent"), get, out);
    if (twin == Twin::Unbounded) { coop::KeyName unused; get(U32(names.size()), unused); }
    if (twin == Twin::MissingAsZero) { result = coop::KeyLookup::Found; out = 0; }
    valid &= result == coop::KeyLookup::Missing && out == 999 && calls == names.size() && badIndex == 0;
    calls = 0;
    valid &= coop::FindKeyName(0, names[0], get, out) == coop::KeyLookup::Missing && calls == 0;
    valid &= coop::FindKeyName(coop::KeyCountLimit + 1, names[0], get, out) == coop::KeyLookup::Malformed && calls == 0;
    valid &= coop::FindKeyName(3, name(u""), get, out) == coop::KeyLookup::Malformed && calls == 0;
    auto unreadable = [](U32, coop::KeyName&) { return false; };
    valid &= coop::FindKeyName(3, names[0], unreadable, out) == coop::KeyLookup::Malformed && out == 999;
    // Exact, case-sensitive matching; localized letters cannot silently alias W.
    valid &= coop::FindKeyName(3, name(u"Key_w"), get, out) == coop::KeyLookup::Missing;
    valid &= coop::FindKeyName(3, name(u"Key_\u0426"), get, out) == coop::KeyLookup::Missing;
    return valid;
}

static bool nativeStringChecks() {
    std::array<unsigned char, 2048> memory{};
    U32 header[4]{6, 32, 512, 0};
    std::memcpy(memory.data() + 64, header, sizeof(header));
    const char16_t text[] = u"Key_W";
    std::memcpy(memory.data() + 512, text, sizeof(text));
    auto read = [&](U32 address, void* output, std::size_t count) {
        if (std::uint64_t(address) + count > memory.size()) return false;
        std::memcpy(output, memory.data() + address, count); return true;
    };
    coop::KeyName out;
    if (!coop::ReadNativeKeyName(64, read, out) || out.length != 5 || out.text[4] != u'W') return false;
    const auto checkBad = [&](U32 length, U32 cap, U32 data) {
        const U32 bad[4]{length, cap, data, 0};
        std::memcpy(memory.data() + 64, bad, sizeof(bad));
        coop::KeyName unchanged = name(u"unchanged");
        return !coop::ReadNativeKeyName(64, read, unchanged) && unchanged.length == 9;
    };
    if (!checkBad(0, 32, 512) || !checkBad(257, 300, 512) || !checkBad(6, 5, 512) ||
        !checkBad(6, 32, 0) || !checkBad(6, 32, 0xfffffffcu) || !checkBad(6, 32, 2046)) return false;
    std::memcpy(memory.data() + 64, header, sizeof(header));
    memory[512 + 10] = 1; // Missing terminating NUL.
    if (coop::ReadNativeKeyName(64, read, out)) return false;
    std::memcpy(memory.data() + 512, text, sizeof(text));
    memory[514] = memory[515] = 0; // Embedded NUL before the declared end.
    if (coop::ReadNativeKeyName(64, read, out)) return false;
    return !coop::ReadNativeKeyName(0, read, out) && !coop::ReadNativeKeyName(0xfffffff8u, read, out);
}

int main() {
    U32 selected = 999;
    const U32 loaded[]{0x04190419u, 0xf0010409u, 0x04090409u};
    const U32 noUS[]{0x04190419u, 0xf0010409u};
    bool ok = lookupChecks(Twin::None) && !lookupChecks(Twin::Unbounded) &&
              !lookupChecks(Twin::MissingAsZero) && nativeStringChecks();
    ok &= coop::SelectLoadedUSLayout(loaded, 3, selected) && selected == 0x04090409u;
    selected = 999;
    ok &= !coop::SelectLoadedUSLayout(noUS, 2, selected) && selected == 999;
    ok &= !coop::SelectLoadedUSLayout(nullptr, 0, selected) &&
          !coop::SelectLoadedUSLayout(loaded, 257, selected);
    std::puts(ok ? "INPUT_KEY_SELFTEST_PASS bounded_lookup/native_strings/exact_loaded_us twins_caught=2"
                 : "INPUT_KEY_SELFTEST_FAIL");
    return ok ? 0 : 1;
}
