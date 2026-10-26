#include "live_join_journal.hpp"
#include "live_join_phase.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

using namespace coop::live_join;

static Metadata SampleMetadata(std::uint32_t baseTick = 400) {
    Metadata metadata{};
    const char path[] = "Tokyo/1_Level/1_Area";
    std::memcpy(metadata.areaPath.data(), path, sizeof(path));
    metadata.baseTick = baseTick;
    metadata.seed = 0x12345678u;
    metadata.sidekick = 3;
    metadata.partyMode = kPartyModeTwoHumans;
    for (std::size_t i = 0; i < metadata.missionStartProgress.size(); ++i)
        metadata.missionStartProgress[i] = static_cast<std::uint8_t>(i * 17u);
    return metadata;
}

static Frame SampleFrame(std::uint32_t tick) {
    Frame frame{};
    frame.tick = tick;
    for (std::size_t i = 0; i < frame.host.size(); ++i) {
        frame.host[i] = static_cast<float>(i % 11u) / 10.0f;
        frame.partner[i] = static_cast<float>((i + 4u) % 11u) / 10.0f;
    }
    frame.pointer = {65535u, 317u, coop::menuPointerActive | coop::menuPointerClick};
    frame.agreedStatus = 0x15u;
    frame.hostSlot = 1;
    frame.postState = UINT64_C(0x0123456789abcdef) + tick;
    return frame;
}

static bool SameFrame(const Frame& a, const Frame& b) {
    return a.tick == b.tick && std::memcmp(a.host.data(), b.host.data(), sizeof(a.host)) == 0 &&
           std::memcmp(a.partner.data(), b.partner.data(), sizeof(a.partner)) == 0 &&
           a.pointer.x == b.pointer.x && a.pointer.y == b.pointer.y && a.pointer.flags == b.pointer.flags &&
           a.agreedStatus == b.agreedStatus && a.hostSlot == b.hostSlot &&
           a.partyMode == b.partyMode && a.postState == b.postState;
}

static bool SameMetadata(const Metadata& a, const Metadata& b) {
    return a.areaPath == b.areaPath && a.baseTick == b.baseTick && a.seed == b.seed &&
           a.sidekick == b.sidekick && a.partyMode == b.partyMode && a.schemaVersion == b.schemaVersion &&
           a.missionStartProgress == b.missionStartProgress;
}

static bool Expect(bool condition, const char* label) {
    if (!condition) std::printf("FAIL %s\n", label);
    return condition;
}

static bool RoundTrip() {
    Journal source(SampleMetadata(), 3);
    if (!Expect(source.IsValid(), "valid journal metadata") ||
        !Expect(source.Append(SampleFrame(400)), "append frame 400") ||
        !Expect(source.Append(SampleFrame(401)), "append frame 401")) return false;
    std::vector<std::uint8_t> bytes;
    if (!Expect(source.Encode(bytes), "encode journal") ||
        !Expect(bytes.size() == kMetadataRecordBytes + 4u + 2u * kFrameRecordBytes, "wire length")) return false;

    Journal decoded(SampleMetadata(1), 3);
    if (!Expect(decoded.Decode(bytes.data(), bytes.size()), "decode journal") ||
        !Expect(decoded.Size() == 2, "decoded count") ||
        !Expect(SameMetadata(decoded.GetMetadata(), source.GetMetadata()), "metadata round trip") ||
        !Expect(decoded.At(0) && SameFrame(*decoded.At(0), *source.At(0)), "frame 0 round trip") ||
        !Expect(decoded.At(1) && SameFrame(*decoded.At(1), *source.At(1)), "frame 1 round trip") ||
        !Expect(decoded.At(2) == nullptr, "indexed bounds")) return false;

    std::array<std::uint8_t, kMetadataRecordBytes> metadataRecord{};
    Metadata metadataOut{};
    if (!Expect(EncodeMetadata(source.GetMetadata(), metadataRecord), "metadata encode") ||
        !Expect(DecodeMetadata(metadataRecord.data(), metadataRecord.size(), metadataOut), "metadata decode") ||
        !Expect(SameMetadata(source.GetMetadata(), metadataOut), "standalone metadata round trip")) return false;

    Metadata aiMetadata = SampleMetadata(700);
    aiMetadata.partyMode = kPartyModeNativeAi;
    Journal transitions(aiMetadata, 3);
    Frame aiStart = SampleFrame(700);
    aiStart.partyMode = kPartyModeNativeAi;
    aiStart.partner = {};
    aiStart.hostSlot = 0;
    Frame humans = SampleFrame(701);
    humans.partyMode = kPartyModeTwoHumans;
    humans.hostSlot = 1;
    Frame aiReturn = aiStart;
    aiReturn.tick = 702;
    if (!Expect(transitions.Append(aiStart), "AI initial frame") ||
        !Expect(transitions.Append(humans), "AI to two-human transition") ||
        !Expect(transitions.Append(aiReturn), "two-human to AI transition")) return false;
    std::vector<std::uint8_t> transitionBytes;
    Journal transitionDecoded;
    if (!Expect(transitions.Encode(transitionBytes), "encode party-mode transitions") ||
        !Expect(transitionDecoded.Decode(transitionBytes.data(), transitionBytes.size()), "decode party-mode transitions") ||
        !Expect(transitionDecoded.Size() == 3 && transitionDecoded.GetMetadata().partyMode == kPartyModeNativeAi,
                "initial mode retained across transitions")) return false;
    for (std::size_t i = 0; i < 3; ++i)
        if (!Expect(SameFrame(*transitionDecoded.At(i), *transitions.At(i)), "party-mode transition frame round trip"))
            return false;
    std::vector<std::uint8_t> badInitialMode = transitionBytes;
    const std::size_t firstFrame = kMetadataRecordBytes + 4u;
    detail::Put32(badInitialMode.data() + firstFrame + 248u, kPartyModeTwoHumans);
    detail::Put64(badInitialMode.data() + firstFrame + kFrameBodyBytes,
                  detail::Checksum(badInitialMode.data() + firstFrame, kFrameBodyBytes));
    Journal rejectedInitialMode;
    if (!Expect(!rejectedInitialMode.Decode(badInitialMode.data(), badInitialMode.size()),
                "decode rejects initial mode differing from metadata") ||
        !Expect(!rejectedInitialMode.IsValid() && rejectedInitialMode.Size() == 0,
                "initial-mode decode rejection preserves empty target")) return false;
    return true;
}

static bool AppendFailuresPreserve() {
    Journal journal(SampleMetadata(), 2);
    const Frame original = SampleFrame(400);
    if (!journal.Append(original)) return false;
    Frame invalid = SampleFrame(400);
    bool ok = true;
    ok &= Expect(!journal.Append(invalid), "duplicate tick rejected");
    invalid.tick = 402;
    ok &= Expect(!journal.Append(invalid), "gap rejected");
    invalid = SampleFrame(401); invalid.host[4] = std::numeric_limits<float>::quiet_NaN();
    ok &= Expect(!journal.Append(invalid), "NaN action rejected");
    invalid = SampleFrame(401); invalid.partner[7] = 1.01f;
    ok &= Expect(!journal.Append(invalid), "out of range action rejected");
    invalid = SampleFrame(401); invalid.pointer.flags = 4;
    ok &= Expect(!journal.Append(invalid), "unknown pointer flag rejected");
    invalid = SampleFrame(401); invalid.pointer.x = 65536;
    ok &= Expect(!journal.Append(invalid), "pointer coordinate rejected");
    invalid = SampleFrame(401); invalid.pointer.flags = coop::menuPointerClick;
    ok &= Expect(!journal.Append(invalid), "inactive pointer click rejected");
    invalid = SampleFrame(401); invalid.agreedStatus = 0x20;
    ok &= Expect(!journal.Append(invalid), "unknown status rejected");
    invalid = SampleFrame(401); invalid.hostSlot = 2;
    ok &= Expect(!journal.Append(invalid), "invalid host slot rejected");
    invalid = SampleFrame(401); invalid.partyMode = 2;
    ok &= Expect(!journal.Append(invalid), "unknown frame party mode rejected");
    ok &= Expect(journal.Size() == 1 && journal.At(0) && SameFrame(*journal.At(0), original),
                 "append failures preserve prior frame");

    Journal full(SampleMetadata(), 1);
    ok &= Expect(full.Append(original), "capacity fixture append");
    ok &= Expect(!full.Append(SampleFrame(401)), "capacity failure rejected");
    ok &= Expect(full.Size() == 1 && SameFrame(*full.At(0), original), "capacity preserves prior frame");

    Journal nearOverflow(SampleMetadata(std::numeric_limits<std::uint32_t>::max()), 2);
    ok &= Expect(nearOverflow.Append(SampleFrame(std::numeric_limits<std::uint32_t>::max())), "last tick append");
    ok &= Expect(!nearOverflow.Append(SampleFrame(0)), "tick overflow rejected");
    ok &= Expect(nearOverflow.Size() == 1, "tick overflow preserves prior frame");

    Metadata aiMetadata = SampleMetadata(700);
    aiMetadata.partyMode = kPartyModeNativeAi;
    Journal ai(aiMetadata, 3);
    Frame aiFrame = SampleFrame(700);
    aiFrame.partyMode = kPartyModeNativeAi;
    aiFrame.partner = {};
    aiFrame.hostSlot = 0;
    Frame wrongInitial = SampleFrame(700);
    ok &= Expect(!ai.Append(wrongInitial), "first frame must match initial metadata party mode");
    ok &= Expect(ai.Size() == 0, "initial mode mismatch preserves empty journal");
    ok &= Expect(ai.Append(aiFrame), "native AI frame accepted");
    Frame fakePartner = aiFrame;
    fakePartner.tick = 701; fakePartner.partner[3] = 0.25f;
    ok &= Expect(!ai.Append(fakePartner), "native AI partner actions rejected");
    Frame fakeSlot = aiFrame;
    fakeSlot.tick = 701; fakeSlot.hostSlot = 1;
    ok &= Expect(!ai.Append(fakeSlot), "native AI second host slot rejected");
    Frame humanFrame = SampleFrame(701);
    humanFrame.partyMode = kPartyModeTwoHumans;
    ok &= Expect(ai.Append(humanFrame), "AI to two-human transition accepted");
    Frame aiReturn = aiFrame;
    aiReturn.tick = 702;
    ok &= Expect(ai.Append(aiReturn), "two-human to AI transition accepted");
    ok &= Expect(ai.Size() == 3, "invalid AI inputs preserve journal across transitions");
    return ok;
}

static bool MetadataFailures() {
    bool ok = true;
    Metadata bad = SampleMetadata();
    bad.schemaVersion = kSchemaVersion + 1;
    Journal unknownVersion(bad, 2);
    ok &= Expect(!unknownVersion.IsValid(), "unknown version rejected at construction");
    bad = SampleMetadata(); bad.partyMode = 2;
    Journal unknownPartyMode(bad, 2);
    ok &= Expect(!unknownPartyMode.IsValid(), "unknown metadata party mode rejected");
    bad = SampleMetadata(); bad.areaPath.fill('A');
    Journal unterminated(bad, 2);
    ok &= Expect(!unterminated.IsValid(), "unterminated path rejected");
    bad = SampleMetadata(); bad.areaPath[5] = '\\';
    Journal backslash(bad, 2);
    ok &= Expect(!backslash.IsValid(), "noncanonical separator rejected");
    bad = SampleMetadata(); bad.areaPath[6] = '.'; bad.areaPath[7] = '.';
    Journal traversal(bad, 2);
    ok &= Expect(!traversal.IsValid(), "dot path component rejected");
    Journal zeroCapacity(SampleMetadata(), 0);
    Journal excessiveCapacity(SampleMetadata(), kMaxFrames + 1);
    ok &= Expect(!zeroCapacity.IsValid() && !excessiveCapacity.IsValid(), "invalid capacity rejected");
    return ok;
}

static bool DecodeFailuresPreserve() {
    Journal good(SampleMetadata(), 3);
    if (!good.Append(SampleFrame(400)) || !good.Append(SampleFrame(401))) return false;
    std::vector<std::uint8_t> bytes;
    if (!good.Encode(bytes)) return false;

    Journal defaultTarget;
    if (!Expect(!defaultTarget.IsValid(), "default target starts uninitialized") ||
        !Expect(defaultTarget.MaximumFrames() == kMaxFrames, "default target has bounded default capacity") ||
        !Expect(defaultTarget.Decode(bytes.data(), bytes.size()), "default target decodes valid journal") ||
        !Expect(defaultTarget.IsValid() && defaultTarget.Size() == good.Size(), "default target becomes valid") ||
        !Expect(SameMetadata(defaultTarget.GetMetadata(), good.GetMetadata()), "default target metadata")) return false;
    std::vector<std::uint8_t> corruptDefault = bytes;
    corruptDefault[kMetadataRecordBytes + 4u + 4u] ^= 1u;
    if (!Expect(!defaultTarget.Decode(corruptDefault.data(), corruptDefault.size()),
                "default target rejects corrupt replacement") ||
        !Expect(defaultTarget.IsValid() && defaultTarget.Size() == good.Size() &&
                SameMetadata(defaultTarget.GetMetadata(), good.GetMetadata()) &&
                SameFrame(*defaultTarget.At(0), *good.At(0)) && SameFrame(*defaultTarget.At(1), *good.At(1)),
                "failed default-target decode preserves prior journal")) return false;

    Journal target(SampleMetadata(900), 3);
    if (!Expect(target.MaximumFrames() == 3, "explicit journal capacity retained")) return false;
    const Frame sentinel = SampleFrame(900);
    if (!target.Append(sentinel)) return false;
    auto unchanged = [&] {
        return target.Size() == 1 && target.At(0) && SameFrame(*target.At(0), sentinel) &&
               target.GetMetadata().baseTick == 900;
    };
    bool ok = true;
    ok &= Expect(!target.Decode(bytes.data(), bytes.size() - 1), "truncated journal rejected");
    ok &= Expect(unchanged(), "truncated decode preserves prior journal");
    ok &= Expect(!target.Decode(bytes.data(), bytes.size() + 1), "trailing bytes rejected");
    ok &= Expect(unchanged(), "length decode preserves prior journal");

    Journal boundedTarget(SampleMetadata(), 1);
    ok &= Expect(!boundedTarget.Decode(bytes.data(), bytes.size()), "explicit decode capacity enforced");
    ok &= Expect(boundedTarget.IsValid() && boundedTarget.Size() == 0 && boundedTarget.MaximumFrames() == 1,
                 "explicit capacity and prior state preserved after rejected decode");

    std::vector<std::uint8_t> malformed = bytes;
    detail::Put32(malformed.data() + kMetadataRecordBytes, 4);
    ok &= Expect(!target.Decode(malformed.data(), malformed.size()), "over capacity count rejected");
    ok &= Expect(unchanged(), "bounds decode preserves prior journal");

    malformed = bytes;
    malformed[kMetadataRecordBytes + 4u + 4u] ^= 1u; // Mutate host action bytes, leave its checksum stale.
    ok &= Expect(!target.Decode(malformed.data(), malformed.size()), "altered frame checksum rejected");
    ok &= Expect(unchanged(), "checksum decode preserves prior journal");

    malformed = bytes;
    detail::Put32(malformed.data() + kMetadataRecordBytes + 4u, 401); // First tick becomes a gap.
    detail::Put64(malformed.data() + kMetadataRecordBytes + 4u + kFrameBodyBytes,
                  detail::Checksum(malformed.data() + kMetadataRecordBytes + 4u, kFrameBodyBytes));
    ok &= Expect(!target.Decode(malformed.data(), malformed.size()), "rechecksummed tick gap rejected");
    ok &= Expect(unchanged(), "gap decode preserves prior journal");

    malformed = bytes;
    detail::Put32(malformed.data() + 112, kSchemaVersion + 1);
    detail::Put64(malformed.data() + kMetadataBodyBytes,
                  detail::Checksum(malformed.data(), kMetadataBodyBytes));
    ok &= Expect(!target.Decode(malformed.data(), malformed.size()), "unknown encoded version rejected");
    ok &= Expect(unchanged(), "version decode preserves prior journal");

    malformed = bytes;
    malformed[3] = static_cast<std::uint8_t>('.');
    malformed[4] = static_cast<std::uint8_t>('.');
    detail::Put64(malformed.data() + kMetadataBodyBytes,
                  detail::Checksum(malformed.data(), kMetadataBodyBytes));
    ok &= Expect(!target.Decode(malformed.data(), malformed.size()), "noncanonical encoded path rejected");
    ok &= Expect(unchanged(), "path decode preserves prior journal");

    malformed = bytes;
    detail::Put32(malformed.data() + 108, 2);
    detail::Put64(malformed.data() + kMetadataBodyBytes,
                  detail::Checksum(malformed.data(), kMetadataBodyBytes));
    ok &= Expect(!target.Decode(malformed.data(), malformed.size()), "unknown encoded party mode rejected");
    ok &= Expect(unchanged(), "party mode decode preserves prior journal");

    malformed = bytes;
    malformed[kMetadataRecordBytes + 4u + 4u] = 0;
    malformed[kMetadataRecordBytes + 4u + 5u] = 0;
    malformed[kMetadataRecordBytes + 4u + 6u] = 0xc0;
    malformed[kMetadataRecordBytes + 4u + 7u] = 0x7f; // IEEE-754 quiet NaN.
    detail::Put64(malformed.data() + kMetadataRecordBytes + 4u + kFrameBodyBytes,
                  detail::Checksum(malformed.data() + kMetadataRecordBytes + 4u, kFrameBodyBytes));
    ok &= Expect(!target.Decode(malformed.data(), malformed.size()), "rechecksummed NaN action rejected");
    ok &= Expect(unchanged(), "NaN decode preserves prior journal");
    return ok;
}

static bool mainTest() {
    Offer offer{};offer.tick=643;offer.frames=643;offer.checkpoint=0x12345678abcdef12ULL;
    offer.loadSequence=2;offer.cycle=3;offer.cycleStartTick=99;
    coop::Digest content{};content[0]=42;Offer decoded{};
    auto phase=MakeOffer(offer,content);
    bool phaseOk=Expect(coop::CampaignPhaseValid(phase)&&phase.auxiliary[0]==0&&ReadOffer(phase,decoded)&&
        decoded.tick==643&&decoded.frames==643&&decoded.checkpoint==offer.checkpoint&&decoded.loadSequence==2&&
        decoded.cycle==3&&decoded.cycleStartTick==99,"live offer preserves reserved word and load timeline");
    auto checkpoint=MakeCheckpoint(kCatchupPhase,643,offer.checkpoint,1,content);
    phaseOk &= Expect(coop::CampaignPhaseValid(checkpoint)&&checkpoint.auxiliary[0]==0&&
        coop::CampaignPhaseEqual(checkpoint,checkpoint),"live checkpoint is a valid strict load phase");
    phase.auxiliary[0]=2;phaseOk &= Expect(!ReadOffer(phase,decoded),"offer rejects reserved auxiliary mutation");
    phase=MakeOffer(offer,content);phase.auxiliary[2]=644;
    phaseOk &= Expect(!ReadOffer(phase,decoded),"offer rejects inconsistent timeline bounds");
    const bool ok = phaseOk && RoundTrip() && AppendFailuresPreserve() && MetadataFailures() && DecodeFailuresPreserve();
    if (ok) std::printf("LIVE_JOIN_JOURNAL_SELFTEST_PASS metadata=%zu frame=%zu\n",
                        kMetadataRecordBytes, kFrameRecordBytes);
    return ok;
}

int main() { return mainTest() ? 0 : 1; }
