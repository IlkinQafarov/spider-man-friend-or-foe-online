#pragma once

#include "input_wire.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace coop::live_join {

// The checksum detects accidental corruption; it is not authentication.
constexpr std::uint32_t kSchemaVersion = 2;
constexpr std::uint32_t kPartyModeNativeAi = 0;
constexpr std::uint32_t kPartyModeTwoHumans = 1;
constexpr std::size_t kAreaPathBytes = 96;
constexpr std::size_t kMissionStartProgressBytes = 508;
constexpr std::size_t kMaxFrames = 216000;
constexpr std::size_t kMetadataBodyBytes = kAreaPathBytes + 5u * 4u + kMissionStartProgressBytes;
constexpr std::size_t kMetadataRecordBytes = kMetadataBodyBytes + 8u;
constexpr std::size_t kFrameBodyBytes = 4u + 2u * 28u * 4u + 12u + 4u + 4u + 4u + 8u;
constexpr std::size_t kFrameRecordBytes = kFrameBodyBytes + 8u;
constexpr std::uint32_t kAllowedStatusBits = 0x1fu;

static_assert(sizeof(float) == 4, "journal actions require binary32 float storage");
static_assert(std::numeric_limits<float>::is_iec559, "journal actions require IEEE-754 floats");
static_assert(kMetadataBodyBytes == 624 && kMetadataRecordBytes == 632, "metadata wire size");
static_assert(kFrameBodyBytes == 260 && kFrameRecordBytes == 268, "frame wire size");

struct Metadata {
    std::array<char, kAreaPathBytes> areaPath{};
    std::uint32_t baseTick = 0;
    std::uint32_t seed = 0;
    std::uint32_t sidekick = 0;
    std::uint32_t partyMode = kPartyModeTwoHumans;
    std::uint32_t schemaVersion = kSchemaVersion;
    std::array<std::uint8_t, kMissionStartProgressBytes> missionStartProgress{};
};

struct Frame {
    std::uint32_t tick = 0;
    Actions host{};
    Actions partner{};
    MenuPointer pointer{};
    std::uint32_t agreedStatus = 0;
    std::uint32_t hostSlot = 0;
    std::uint32_t partyMode = kPartyModeTwoHumans;
    std::uint64_t postState = 0;
};

namespace detail {
inline void Put32(std::uint8_t* out, std::uint32_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value);
    out[1] = static_cast<std::uint8_t>(value >> 8);
    out[2] = static_cast<std::uint8_t>(value >> 16);
    out[3] = static_cast<std::uint8_t>(value >> 24);
}
inline void Put64(std::uint8_t* out, std::uint64_t value) noexcept {
    for (unsigned i = 0; i != 8; ++i) out[i] = static_cast<std::uint8_t>(value >> (8u * i));
}
inline std::uint32_t Get32(const std::uint8_t* in) noexcept {
    return std::uint32_t(in[0]) | (std::uint32_t(in[1]) << 8) |
           (std::uint32_t(in[2]) << 16) | (std::uint32_t(in[3]) << 24);
}
inline std::uint64_t Get64(const std::uint8_t* in) noexcept {
    std::uint64_t value = 0;
    for (unsigned i = 0; i != 8; ++i) value |= std::uint64_t(in[i]) << (8u * i);
    return value;
}
inline std::uint64_t Checksum(const std::uint8_t* bytes, std::size_t size) noexcept {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (std::size_t i = 0; i != size; ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}
inline bool ValidPath(const std::array<char, kAreaPathBytes>& path) noexcept {
    std::size_t length = 0;
    while (length < path.size() && path[length] != '\0') ++length;
    if (length == 0 || length == path.size()) return false;
    for (std::size_t i = length + 1; i < path.size(); ++i)
        if (path[i] != '\0') return false;

    std::size_t componentStart = 0;
    for (std::size_t i = 0; i <= length; ++i) {
        if (i == length || path[i] == '/') {
            const std::size_t componentLength = i - componentStart;
            if (componentLength == 0 || (componentLength == 1 && path[componentStart] == '.') ||
                (componentLength == 2 && path[componentStart] == '.' && path[componentStart + 1] == '.'))
                return false;
            componentStart = i + 1;
            continue;
        }
        const unsigned char ch = static_cast<unsigned char>(path[i]);
        const bool alnum = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                           (ch >= '0' && ch <= '9');
        if (!alnum && ch != '_' && ch != '-') return false;
    }
    return true;
}
inline bool ValidActions(const Actions& actions) noexcept {
    for (float value : actions)
        if (!std::isfinite(value) || value < 0.0f || value > 1.0f) return false;
    return true;
}
inline bool ValidFrame(const Frame& frame) noexcept {
    if (!ValidActions(frame.host) || !ValidActions(frame.partner)) return false;
    if ((frame.agreedStatus & ~kAllowedStatusBits) != 0 || frame.hostSlot > 1 ||
        frame.partyMode > kPartyModeTwoHumans) return false;
    if (frame.partyMode == kPartyModeNativeAi) {
        for (float value : frame.partner) if (value != 0.0f) return false;
        if (frame.hostSlot != 0) return false;
    }
    if ((frame.pointer.flags & ~(menuPointerActive | menuPointerClick)) != 0 ||
        frame.pointer.x > 65535u || frame.pointer.y > 65535u) return false;
    if ((frame.pointer.flags & menuPointerClick) && !(frame.pointer.flags & menuPointerActive)) return false;
    if (!(frame.pointer.flags & menuPointerActive) &&
        (frame.pointer.x != 0 || frame.pointer.y != 0 || frame.pointer.flags != 0)) return false;
    return true;
}
inline void PutFloat(std::uint8_t* out, float value) noexcept {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    Put32(out, bits);
}
inline float GetFloat(const std::uint8_t* in) noexcept {
    const std::uint32_t bits = Get32(in);
    float value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
inline bool CheckRecord(const std::uint8_t* data, std::size_t bodyBytes) noexcept {
    return Get64(data + bodyBytes) == Checksum(data, bodyBytes);
}
inline void SealRecord(std::uint8_t* data, std::size_t bodyBytes) noexcept {
    Put64(data + bodyBytes, Checksum(data, bodyBytes));
}
} // namespace detail

inline bool EncodeMetadata(const Metadata& metadata,
                           std::array<std::uint8_t, kMetadataRecordBytes>& out) noexcept {
    if (metadata.schemaVersion != kSchemaVersion || metadata.partyMode > kPartyModeTwoHumans ||
        !detail::ValidPath(metadata.areaPath)) return false;
    std::size_t offset = 0;
    for (char ch : metadata.areaPath) out[offset++] = static_cast<std::uint8_t>(ch);
    detail::Put32(out.data() + offset, metadata.baseTick); offset += 4;
    detail::Put32(out.data() + offset, metadata.seed); offset += 4;
    detail::Put32(out.data() + offset, metadata.sidekick); offset += 4;
    detail::Put32(out.data() + offset, metadata.partyMode); offset += 4;
    detail::Put32(out.data() + offset, metadata.schemaVersion); offset += 4;
    for (std::uint8_t value : metadata.missionStartProgress) out[offset++] = value;
    detail::SealRecord(out.data(), kMetadataBodyBytes);
    return true;
}

inline bool DecodeMetadata(const std::uint8_t* data, std::size_t size, Metadata& out) noexcept {
    if (!data || size != kMetadataRecordBytes || !detail::CheckRecord(data, kMetadataBodyBytes)) return false;
    Metadata decoded{};
    std::size_t offset = 0;
    for (char& ch : decoded.areaPath) ch = static_cast<char>(data[offset++]);
    decoded.baseTick = detail::Get32(data + offset); offset += 4;
    decoded.seed = detail::Get32(data + offset); offset += 4;
    decoded.sidekick = detail::Get32(data + offset); offset += 4;
    decoded.partyMode = detail::Get32(data + offset); offset += 4;
    decoded.schemaVersion = detail::Get32(data + offset); offset += 4;
    for (std::uint8_t& value : decoded.missionStartProgress) value = data[offset++];
    if (decoded.schemaVersion != kSchemaVersion || decoded.partyMode > kPartyModeTwoHumans ||
        !detail::ValidPath(decoded.areaPath)) return false;
    out = decoded;
    return true;
}

inline bool EncodeFrame(const Frame& frame,
                        std::array<std::uint8_t, kFrameRecordBytes>& out) noexcept {
    if (!detail::ValidFrame(frame)) return false;
    std::size_t offset = 0;
    detail::Put32(out.data() + offset, frame.tick); offset += 4;
    for (float value : frame.host) { detail::PutFloat(out.data() + offset, value); offset += 4; }
    for (float value : frame.partner) { detail::PutFloat(out.data() + offset, value); offset += 4; }
    detail::Put32(out.data() + offset, frame.pointer.x); offset += 4;
    detail::Put32(out.data() + offset, frame.pointer.y); offset += 4;
    detail::Put32(out.data() + offset, frame.pointer.flags); offset += 4;
    detail::Put32(out.data() + offset, frame.agreedStatus); offset += 4;
    detail::Put32(out.data() + offset, frame.hostSlot); offset += 4;
    detail::Put32(out.data() + offset, frame.partyMode); offset += 4;
    detail::Put64(out.data() + offset, frame.postState); offset += 8;
    detail::SealRecord(out.data(), kFrameBodyBytes);
    return offset == kFrameBodyBytes;
}

inline bool DecodeFrame(const std::uint8_t* data, std::size_t size, Frame& out) noexcept {
    if (!data || size != kFrameRecordBytes || !detail::CheckRecord(data, kFrameBodyBytes)) return false;
    Frame decoded{};
    std::size_t offset = 0;
    decoded.tick = detail::Get32(data + offset); offset += 4;
    for (float& value : decoded.host) { value = detail::GetFloat(data + offset); offset += 4; }
    for (float& value : decoded.partner) { value = detail::GetFloat(data + offset); offset += 4; }
    decoded.pointer.x = detail::Get32(data + offset); offset += 4;
    decoded.pointer.y = detail::Get32(data + offset); offset += 4;
    decoded.pointer.flags = detail::Get32(data + offset); offset += 4;
    decoded.agreedStatus = detail::Get32(data + offset); offset += 4;
    decoded.hostSlot = detail::Get32(data + offset); offset += 4;
    decoded.partyMode = detail::Get32(data + offset); offset += 4;
    decoded.postState = detail::Get64(data + offset); offset += 8;
    if (offset != kFrameBodyBytes || !detail::ValidFrame(decoded)) return false;
    out = decoded;
    return true;
}

class Journal {
public:
    Journal() noexcept = default;
    explicit Journal(const Metadata& metadata, std::size_t maximumFrames = kMaxFrames) noexcept
        : metadata_(metadata), maximumFrames_(maximumFrames), valid_(
              maximumFrames > 0 && maximumFrames <= kMaxFrames &&
              metadata.schemaVersion == kSchemaVersion && metadata.partyMode <= kPartyModeTwoHumans &&
              detail::ValidPath(metadata.areaPath)) {}

    bool IsValid() const noexcept { return valid_; }
    std::size_t Size() const noexcept { return frames_.size(); }
    std::size_t MaximumFrames() const noexcept { return maximumFrames_; }
    const Metadata& GetMetadata() const noexcept { return metadata_; }
    const Frame* At(std::size_t index) const noexcept {
        return index < frames_.size() ? &frames_[index] : nullptr;
    }

    bool Append(const Frame& frame) noexcept {
        if (!valid_ || frames_.size() >= maximumFrames_ || !detail::ValidFrame(frame) ||
            (frames_.empty() && frame.partyMode != metadata_.partyMode)) return false;
        const std::uint64_t expectedTick = std::uint64_t(metadata_.baseTick) + frames_.size();
        if (expectedTick > (std::numeric_limits<std::uint32_t>::max)() || frame.tick != expectedTick) return false;
        try { frames_.push_back(frame); }
        catch (...) { return false; }
        return true;
    }

    // Format: fixed metadata record, LE32 frame count, then fixed frame records.
    bool Encode(std::vector<std::uint8_t>& out) const noexcept {
        if (!valid_ || frames_.size() > maximumFrames_) return false;
        std::array<std::uint8_t, kMetadataRecordBytes> metadataRecord{};
        if (!EncodeMetadata(metadata_, metadataRecord)) return false;
        std::vector<std::uint8_t> encoded;
        try {
            encoded.resize(kMetadataRecordBytes + 4u + frames_.size() * kFrameRecordBytes);
        } catch (...) { return false; }
        std::memcpy(encoded.data(), metadataRecord.data(), metadataRecord.size());
        detail::Put32(encoded.data() + kMetadataRecordBytes, static_cast<std::uint32_t>(frames_.size()));
        std::size_t offset = kMetadataRecordBytes + 4u;
        for (const Frame& frame : frames_) {
            std::array<std::uint8_t, kFrameRecordBytes> record{};
            if (!EncodeFrame(frame, record)) return false;
            std::memcpy(encoded.data() + offset, record.data(), record.size());
            offset += record.size();
        }
        out.swap(encoded);
        return true;
    }

    // Decode is transactional: any invalid or truncated input leaves this journal intact.
    bool Decode(const std::uint8_t* data, std::size_t size) noexcept {
        if (!data || size < kMetadataRecordBytes + 4u) return false;
        Metadata metadata{};
        if (!DecodeMetadata(data, kMetadataRecordBytes, metadata)) return false;
        const std::uint32_t count = detail::Get32(data + kMetadataRecordBytes);
        if (maximumFrames_ == 0 || maximumFrames_ > kMaxFrames || count > maximumFrames_) return false;
        const std::size_t expectedSize = kMetadataRecordBytes + 4u +
                                         std::size_t(count) * kFrameRecordBytes;
        if (size != expectedSize) return false;
        Journal replacement(metadata, maximumFrames_);
        if (!replacement.IsValid()) return false;
        try { replacement.frames_.reserve(count); }
        catch (...) { return false; }
        std::size_t offset = kMetadataRecordBytes + 4u;
        for (std::uint32_t i = 0; i < count; ++i) {
            Frame frame{};
            if (!DecodeFrame(data + offset, kFrameRecordBytes, frame) || !replacement.Append(frame)) return false;
            offset += kFrameRecordBytes;
        }
        *this = std::move(replacement);
        return true;
    }

private:
    Metadata metadata_{};
    // A default journal is an uninitialized decode target, not a zero-capacity journal.
    std::size_t maximumFrames_ = kMaxFrames;
    bool valid_ = false;
    std::vector<Frame> frames_;
};

} // namespace coop::live_join
