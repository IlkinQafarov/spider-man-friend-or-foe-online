#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <algorithm>

namespace coop {
constexpr std::size_t KeyNameLimit = 256;
constexpr std::uint32_t KeyCountLimit = 1024;
struct KeyName {
    std::array<char16_t, KeyNameLimit> text{};
    std::size_t length = 0; // Excludes the terminal NUL.
};
enum class KeyLookup { Found, Missing, Malformed };

// Exact-build native wide string: length including NUL, capacity, data, ref.
// Read through a caller-provided safe reader; never dereference game pointers.
template<class Reader>
bool ReadNativeKeyName(std::uint32_t address, Reader read, KeyName& out) {
    std::uint32_t header[4]{};
    if (!address || address > 0xfffffff0u || !read(address, header, sizeof(header))) return false;
    const auto length = header[0], capacity = header[1], data = header[2];
    if (!length || length > KeyNameLimit || capacity < length || !data ||
        std::uint64_t(data) + std::uint64_t(length) * 2 > 0x100000000ull) return false;
    KeyName value;
    if (!read(data, value.text.data(), length * 2) || value.text[length - 1] != 0) return false;
    for (std::uint32_t i = 0; i + 1 < length; ++i) if (value.text[i] == 0) return false;
    value.length = length - 1;
    out = value;
    return true;
}

// Native527B30/527640 compares UTF16 code units exactly. The original525C30
// discards provider count and searches forever; this returns an explicit miss.
template<class Getter>
KeyLookup FindKeyName(std::uint32_t count, const KeyName& target, Getter get,
                      std::uint32_t& found) {
    if (count > KeyCountLimit || !target.length || target.length >= KeyNameLimit)
        return KeyLookup::Malformed;
    for (std::uint32_t i = 0; i < count; ++i) {
        KeyName candidate;
        if (!get(i, candidate) || candidate.length >= KeyNameLimit) return KeyLookup::Malformed;
        if (candidate.length == target.length &&
            std::equal(target.text.begin(), target.text.begin() + target.length, candidate.text.begin())) {
            found = i;
            return KeyLookup::Found;
        }
    }
    return KeyLookup::Missing;
}

// US language alone is insufficient: Dvorak/custom0409 variants are different.
// Runtime additionally validates physical-key translations on this loaded HKL.
inline bool SelectLoadedUSLayout(const std::uint32_t* handles, std::size_t count,
                                 std::uint32_t& out) {
    if (!handles || count == 0 || count > 256) return false;
    for (std::size_t i = 0; i < count; ++i) if (handles[i] == 0x04090409u) {
        out = handles[i];
        return true;
    }
    return false;
}
}
