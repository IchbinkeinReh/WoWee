// The 3.3.5 aura and cast-failure packets as TrinityCore and AzerothCore
// build them, read as wow.exe 12340 reads them.
//
// SMSG_AURA_UPDATE and SMSG_AURA_UPDATE_ALL are one reader (0x007300a0 ->
// 0x0072f5d0): the unit's packed guid, then slots to the end of the packet,
// each 0x00716510's - slot, spell, and for a spell its flags, level and
// stack, the caster's packed guid unless flags 0x08, the durations under
// 0x20. SMSG_CAST_FAILED is 0x00809af0: up to two u32s follow the result,
// whatever it is, while the packet has them.
#include <catch_amalgamated.hpp>

#include <cstring>
#include <vector>

#include "core/application.hpp"
#include "game/packet_parsers.hpp"
#include "game/world_packets.hpp"

namespace wowee {
namespace core {
Application* Application::instance = nullptr;
}
}  // namespace wowee

using namespace wowee::game;

namespace {

constexpr uint64_t kPlayer = 0x0000000000000042ull;
constexpr uint64_t kCreature = 0xF130000ABC000123ull;

struct Writer {
    std::vector<uint8_t> bytes;
    void u8(uint8_t v) { bytes.push_back(v); }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>(v >> (i * 8)));
    }
    void packedGuid(uint64_t guid) {
        uint8_t mask = 0;
        uint8_t out[8];
        int n = 0;
        for (int i = 0; i < 8; ++i) {
            const uint8_t b = static_cast<uint8_t>((guid >> (i * 8)) & 0xFF);
            if (b) { mask |= static_cast<uint8_t>(1u << i); out[n++] = b; }
        }
        u8(mask);
        for (int i = 0; i < n; ++i) u8(out[i]);
    }
    wowee::network::Packet packet() const { return wowee::network::Packet(0, bytes); }
};

/// AuraApplication::BuildUpdatePacket, as both cores write one slot.
void writeAura(Writer& w, uint8_t slot, uint32_t spellId, uint8_t flags, uint8_t level, uint8_t stack,
               uint64_t caster, int32_t maxDuration, int32_t duration) {
    w.u8(slot);
    w.u32(spellId);
    if (spellId == 0) return;
    w.u8(flags);
    w.u8(level);
    w.u8(stack);
    if (!(flags & 0x08)) w.packedGuid(caster);
    if (flags & 0x20) {
        w.u32(static_cast<uint32_t>(maxDuration));
        w.u32(static_cast<uint32_t>(duration));
    }
}

}  // namespace

TEST_CASE("aura update: a self-cast aura names the unit as its caster", "[aura]") {
    Writer w;
    w.packedGuid(kPlayer);
    // Power Word: Fortitude the player cast on itself: AFLAG_CASTER, no guid.
    writeAura(w, 3, 1243, 0x01 | 0x08 | 0x10 | 0x20, 80, 0, kPlayer, 1800000, 1799000);
    auto packet = w.packet();
    AuraUpdateData data;
    REQUIRE(AuraUpdateParser::parse(packet, data, false));
    CHECK(packet.getRemainingSize() == 0);
    CHECK(data.guid == kPlayer);
    REQUIRE(data.updates.size() == 1);
    CHECK(data.updates[0].first == 3);
    const AuraSlot& a = data.updates[0].second;
    CHECK(a.spellId == 1243u);
    CHECK(a.level == 80);
    // 0x00716510 takes the unit's own guid; this was left 0, so the
    // player's own buffs had no caster and UnitAura returned none.
    CHECK(a.casterGuid == kPlayer);
    CHECK(a.maxDurationMs == 1800000);
    CHECK(a.durationMs == 1799000);
}

TEST_CASE("aura update: flags 0x40 adds nothing to the wire in 3.3.5", "[aura]") {
    Writer w;
    w.packedGuid(kCreature);
    // A debuff with the effect-amount flag set, then a second slot. There are
    // no amounts on the wire; reading them swallowed the next slot.
    writeAura(w, 0, 589, 0x01 | 0x40 | 0x20, 80, 0, kPlayer, 18000, 18000);
    writeAura(w, 1, 0, 0, 0, 0, 0, 0, 0);
    auto packet = w.packet();
    AuraUpdateData data;
    REQUIRE(AuraUpdateParser::parse(packet, data, true));
    CHECK(packet.getRemainingSize() == 0);
    REQUIRE(data.updates.size() == 2);
    CHECK(data.updates[0].second.casterGuid == kPlayer);
    CHECK(data.updates[0].second.durationMs == 18000);
    CHECK(data.updates[1].first == 1);
    CHECK(data.updates[1].second.isEmpty());
}

TEST_CASE("aura update: a single update is read to its end like the full one", "[aura]") {
    Writer w;
    w.packedGuid(kCreature);
    writeAura(w, 5, 0, 0, 0, 0, 0, 0, 0);
    writeAura(w, 6, 774, 0x01 | 0x10, 80, 2, kPlayer, 0, 0);
    auto packet = w.packet();
    AuraUpdateData data;
    REQUIRE(AuraUpdateParser::parse(packet, data, false));
    CHECK(packet.getRemainingSize() == 0);
    REQUIRE(data.updates.size() == 2);
    CHECK(data.updates[1].second.spellId == 774u);
    CHECK(data.updates[1].second.charges == 2);
    CHECK(data.updates[1].second.casterGuid == kPlayer);
    // No 0x20: no durations, the aura does not run out.
    CHECK(data.updates[1].second.durationMs == -1);
}

TEST_CASE("cast failed: the reagent's item id follows the result", "[cast]") {
    // Spell::SendCastResult, SPELL_FAILED_REAGENTS: the missing item.
    Writer w;
    w.u8(1);
    w.u32(20484);
    w.u8(0x64);
    w.u32(17030);
    auto packet = w.packet();
    CastFailedData data;
    WotlkPacketParsers parsers;
    REQUIRE(parsers.parseCastFailed(packet, data));
    CHECK(packet.getRemainingSize() == 0);
    CHECK(data.result == 0x64);
    CHECK(data.miscArg == 17030u);
    CHECK(data.miscArg2 == 0u);
}

TEST_CASE("cast failed: a skill and a rank, or an item class and subclass mask", "[cast]") {
    Writer w;
    w.u8(0);
    w.u32(2575);
    w.u8(0x96);  // MIN_SKILL
    w.u32(186);  // Mining
    w.u32(175);
    auto packet = w.packet();
    CastFailedData data;
    WotlkPacketParsers parsers;
    REQUIRE(parsers.parseCastFailed(packet, data));
    CHECK(packet.getRemainingSize() == 0);
    CHECK(data.miscArg == 186u);
    CHECK(data.miscArg2 == 175u);

    Writer bare;
    bare.u8(2);
    bare.u32(133);
    bare.u8(0x0c);
    auto barePacket = bare.packet();
    CastFailedData bareData;
    REQUIRE(parsers.parseCastFailed(barePacket, bareData));
    CHECK(bareData.result == 0x0c);
    CHECK(bareData.miscArg == 0u);
}

TEST_CASE("SMSG_TOTEM_CREATED carries the totem's guid whole", "[totem]") {
    // Totem::InitStats writes slot, uint64 guid, duration and spell
    // (0x00526530 reads the guid with the 8-byte reader). Read as a packed
    // guid, the mask byte and the guid's bytes became duration and spell.
    Writer w;
    w.u8(0);                                  // fire
    w.u32(static_cast<uint32_t>(kCreature));
    w.u32(static_cast<uint32_t>(kCreature >> 32));
    w.u32(120000);                            // two minutes
    w.u32(3599);                              // Searing Totem
    auto packet = w.packet();
    TotemCreatedData data;
    REQUIRE(TotemCreatedParser::parse(packet, data));
    CHECK(packet.getRemainingSize() == 0);
    CHECK(data.slot == 0);
    CHECK(data.guid == kCreature);
    CHECK(data.durationMs == 120000u);
    CHECK(data.spellId == 3599u);
}
