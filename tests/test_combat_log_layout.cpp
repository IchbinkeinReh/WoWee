// The 3.3.5 combat log packets as TrinityCore and AzerothCore build them,
// read as wow.exe 12340 reads them.
//
// SMSG_ATTACKERSTATEUPDATE is 0x00755630: the absorbs and resists are on the
// wire only where the hit flags name them (0x60, 0x180), the victim state is
// a byte, and the blocked amount (0x2000), the rage (0x800000) and a debug
// block (0x1) follow by flag. The rest are the readers 0x006b0b80 registers:
// full eight-byte guids where the server writes ObjectGuid as a uint64, and
// SMSG_SPELLLOGEXECUTE's entries by 0x006d368c's table.
#include <catch_amalgamated.hpp>

#include <cstring>
#include <vector>

#include "core/application.hpp"
#include "game/packet_parsers.hpp"
#include "game/power_prediction.hpp"
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
    void u64(uint64_t v) {
        u32(static_cast<uint32_t>(v));
        u32(static_cast<uint32_t>(v >> 32));
    }
    void f32(float v) {
        uint32_t raw;
        std::memcpy(&raw, &v, 4);
        u32(raw);
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

/// Unit::SendAttackStateUpdate, one sub-damage as both cores send it.
struct Swing {
    uint32_t hitInfo = 0x2;  // HITINFO_AFFECTS_VICTIM
    uint32_t damage = 0;
    uint32_t overkill = 0;
    uint32_t school = 1;
    uint32_t absorb = 0;
    uint32_t resist = 0;
    uint8_t targetState = 1;  // VICTIMSTATE_HIT
    uint32_t blocked = 0;
    uint32_t rage = 0;
};

Writer writeSwing(const Swing& s) {
    Writer w;
    w.u32(s.hitInfo);
    w.packedGuid(kPlayer);
    w.packedGuid(kCreature);
    w.u32(s.damage);
    w.u32(s.overkill);
    w.u8(1);
    w.u32(s.school);
    w.f32(static_cast<float>(s.damage));
    w.u32(s.damage);
    if (s.hitInfo & 0x60) w.u32(s.absorb);
    if (s.hitInfo & 0x180) w.u32(s.resist);
    w.u8(s.targetState);
    w.u32(0);  // unknown attacker state
    w.u32(0);  // melee spell id
    if (s.hitInfo & 0x2000) w.u32(s.blocked);
    if (s.hitInfo & 0x800000) w.u32(s.rage);
    return w;
}

AttackerStateUpdateData readSwing(const Swing& s) {
    auto packet = writeSwing(s).packet();
    WotlkPacketParsers parsers;
    AttackerStateUpdateData data;
    REQUIRE(parsers.parseAttackerStateUpdate(packet, data));
    // Every byte the server wrote, and none past it.
    CHECK(packet.getRemainingSize() == 0);
    CHECK(data.attackerGuid == kPlayer);
    CHECK(data.targetGuid == kCreature);
    return data;
}

}  // namespace

TEST_CASE("attacker state: a plain hit has no absorb or resist on the wire", "[combatlog]") {
    Swing s;
    s.damage = 87;
    const auto d = readSwing(s);
    CHECK(d.totalDamage == 87);
    REQUIRE(d.subDamages.size() == 1);
    CHECK(d.subDamages[0].intDamage == 87u);
    CHECK(d.subDamages[0].absorbed == 0u);
    CHECK(d.subDamages[0].resisted == 0u);
    // A byte: read as a uint32 with the absorb and resist before it, this
    // came out 0 for every swing.
    CHECK(d.victimState == 1u);
}

TEST_CASE("attacker state: a critical hit", "[combatlog]") {
    Swing s;
    s.hitInfo = 0x2 | 0x200;
    s.damage = 190;
    s.overkill = 12;
    const auto d = readSwing(s);
    CHECK(d.isCrit());
    CHECK(d.overkill == 12);
    CHECK(d.victimState == 1u);
}

TEST_CASE("attacker state: a miss and a dodge", "[combatlog]") {
    Swing miss;
    miss.hitInfo = 0x2 | 0x10;
    miss.targetState = 0;  // VICTIMSTATE_INTACT
    const auto m = readSwing(miss);
    CHECK(m.isMiss());
    CHECK(m.victimState == 0u);

    Swing dodge;
    dodge.targetState = 2;  // VICTIMSTATE_DODGE
    CHECK(readSwing(dodge).victimState == 2u);
}

TEST_CASE("attacker state: absorbs and resists only under their flags", "[combatlog]") {
    Swing absorb;
    absorb.hitInfo = 0x2 | 0x40;  // HITINFO_PARTIAL_ABSORB
    absorb.damage = 50;
    absorb.absorb = 30;
    const auto a = readSwing(absorb);
    CHECK(a.subDamages[0].absorbed == 30u);
    CHECK(a.subDamages[0].resisted == 0u);
    CHECK(a.victimState == 1u);

    Swing full;
    full.hitInfo = 0x2 | 0x20;  // HITINFO_FULL_ABSORB
    full.absorb = 80;
    CHECK(readSwing(full).subDamages[0].absorbed == 80u);

    Swing resist;
    resist.hitInfo = 0x2 | 0x100;  // HITINFO_PARTIAL_RESIST
    resist.school = 4;
    resist.damage = 40;
    resist.resist = 10;
    const auto r = readSwing(resist);
    CHECK(r.subDamages[0].absorbed == 0u);
    CHECK(r.subDamages[0].resisted == 10u);

    Swing both;
    both.hitInfo = 0x2 | 0x40 | 0x100;
    both.damage = 20;
    both.absorb = 7;
    both.resist = 9;
    const auto b = readSwing(both);
    CHECK(b.subDamages[0].absorbed == 7u);
    CHECK(b.subDamages[0].resisted == 9u);
}

TEST_CASE("attacker state: a block names its amount", "[combatlog]") {
    Swing s;
    s.hitInfo = 0x2 | 0x2000;  // HITINFO_BLOCK
    s.targetState = 5;         // VICTIMSTATE_BLOCKS
    s.damage = 60;
    s.blocked = 25;
    const auto d = readSwing(s);
    CHECK(d.victimState == 5u);
    CHECK(d.blocked == 25u);
}

TEST_CASE("attacker state: glancing and crushing are 0x10000 and 0x20000", "[combatlog]") {
    Swing glance;
    glance.hitInfo = 0x2 | 0x10000;
    glance.damage = 30;
    const auto g = readSwing(glance);
    CHECK(g.isGlancing());
    CHECK_FALSE(g.isCrushing());

    Swing crush;
    crush.hitInfo = 0x2 | 0x20000;
    crush.damage = 300;
    const auto c = readSwing(crush);
    CHECK(c.isCrushing());
    CHECK_FALSE(c.isGlancing());
}

TEST_CASE("attacker state: the rage under 0x800000, after the block", "[combatlog]") {
    Swing s;
    s.hitInfo = 0x2 | 0x2000 | 0x800000;
    s.targetState = 5;
    s.damage = 10;
    s.blocked = 40;
    s.rage = 125;
    const auto d = readSwing(s);
    CHECK(d.blocked == 40u);
    CHECK(d.rageGained == 125);

    Swing plain;
    plain.hitInfo = 0x2 | 0x800000;
    plain.damage = 10;
    plain.rage = 51;
    CHECK(readSwing(plain).rageGained == 51);

    // No rage on the wire for a crushing blow: 0x20000 is not RAGE_GAIN.
    Swing crush;
    crush.hitInfo = 0x2 | 0x20000;
    crush.damage = 300;
    CHECK(readSwing(crush).rageGained == 0);
}

TEST_CASE("attacker state: the swing's rage moves the attacker's prediction", "[combatlog]") {
    // 0x007512b0 -> 0x00722cf0(1, +0x64): a warrior (rage, power type 1) at
    // 20.0 rage of 100.0 gains the swing's 12.5.
    Swing s;
    s.hitInfo = 0x2 | 0x800000;
    s.damage = 100;
    s.rage = 125;
    const auto d = readSwing(s);
    power_prediction::Predicted p;
    p.reset({0, 200, 0, 0, 0, 0, 0});
    CHECK(power_prediction::add(p, 1, d.rageGained, 1, 0u, 1000));
    CHECK(p.value[1] == 325);
    // Only on the unit's own power type: a druid in cat form gains none.
    power_prediction::Predicted cat;
    cat.reset({0, 200, 0, 0, 0, 0, 0});
    CHECK_FALSE(power_prediction::add(cat, 1, d.rageGained, 3, 0u, 1000));
    CHECK(cat.value[1] == 200);
    // No more than the maximum.
    CHECK(power_prediction::add(p, 1, 900, 1, 0u, 1000));
    CHECK(p.value[1] == 1000);
}

TEST_CASE("attacker state: the debug block under 0x1 is passed over", "[combatlog]") {
    Swing s;
    s.hitInfo = 0x2 | 0x1;
    s.damage = 5;
    auto w = writeSwing(s);
    w.u32(0);
    for (int i = 0; i < 8 + 4; ++i) w.f32(0.0f);
    w.u32(0);
    auto packet = w.packet();
    WotlkPacketParsers parsers;
    AttackerStateUpdateData d;
    REQUIRE(parsers.parseAttackerStateUpdate(packet, d));
    CHECK(packet.getRemainingSize() == 0);
    CHECK(d.victimState == 1u);
}

TEST_CASE("attacker state: a packet short of a flagged field is refused", "[combatlog]") {
    Swing s;
    s.hitInfo = 0x2 | 0x800000;
    s.damage = 10;
    auto w = writeSwing(s);
    w.bytes.resize(w.bytes.size() - 2);
    auto packet = w.packet();
    WotlkPacketParsers parsers;
    AttackerStateUpdateData d;
    CHECK_FALSE(parsers.parseAttackerStateUpdate(packet, d));
    CHECK(packet.getReadPos() == 0);
}

TEST_CASE("damage shield: full guids, then spell, damage, overkill, school", "[combatlog]") {
    Writer w;
    w.u64(kCreature);  // the one the shield struck
    w.u64(kPlayer);    // its owner
    w.u32(467);        // Thorns
    w.u32(9);
    w.u32(0);
    w.u32(8);
    auto packet = w.packet();
    SpellDamageShieldData d;
    REQUIRE(SpellDamageShieldParser::parse(packet, d));
    CHECK(packet.getRemainingSize() == 0);
    CHECK(d.victimGuid == kCreature);
    CHECK(d.casterGuid == kPlayer);
    CHECK(d.spellId == 467u);
    CHECK(d.damage == 9u);
    CHECK(d.schoolMask == 8u);
}

TEST_CASE("proc resist, immune and instakill: full guids", "[combatlog]") {
    Writer w;
    w.u64(kPlayer);
    w.u64(kCreature);
    w.u32(12345);
    w.u8(0);  // debug log format
    auto packet = w.packet();
    SpellLogGuidsData d;
    REQUIRE(SpellLogGuidsParser::parse(packet, d, true));
    CHECK(packet.getRemainingSize() == 0);
    CHECK(d.casterGuid == kPlayer);
    CHECK(d.victimGuid == kCreature);
    CHECK(d.spellId == 12345u);

    Writer k;
    k.u64(kCreature);
    k.u64(kPlayer);
    k.u32(5);  // Death Touch
    auto kill = k.packet();
    SpellLogGuidsData ik;
    REQUIRE(SpellLogGuidsParser::parse(kill, ik, false));
    CHECK(kill.getRemainingSize() == 0);
    CHECK(ik.casterGuid == kCreature);
    CHECK(ik.victimGuid == kPlayer);
}

TEST_CASE("spell log miss: full guids, no reflect tail", "[combatlog]") {
    Writer w;
    w.u32(133);  // Fireball
    w.u64(kPlayer);
    w.u8(0);
    w.u32(2);
    w.u64(kCreature);
    w.u8(11);  // SPELL_MISS_REFLECT, and nothing after it
    w.u64(kCreature + 1);
    w.u8(2);   // SPELL_MISS_RESIST
    auto packet = w.packet();
    SpellLogMissData d;
    REQUIRE(SpellLogMissParser::parse(packet, d));
    CHECK(packet.getRemainingSize() == 0);
    CHECK(d.spellId == 133u);
    CHECK(d.casterGuid == kPlayer);
    REQUIRE(d.misses.size() == 2);
    CHECK(d.misses[0].victimGuid == kCreature);
    CHECK(d.misses[0].missInfo == 11);
    CHECK(d.misses[1].victimGuid == kCreature + 1);
    CHECK(d.misses[1].missInfo == 2);
}

TEST_CASE("spell log miss: two floats a miss under the debug byte", "[combatlog]") {
    Writer w;
    w.u32(133);
    w.u64(kPlayer);
    w.u8(1);
    w.u32(1);
    w.u64(kCreature);
    w.u8(2);
    w.f32(0.5f);
    w.f32(0.25f);
    auto packet = w.packet();
    SpellLogMissData d;
    REQUIRE(SpellLogMissParser::parse(packet, d));
    CHECK(packet.getRemainingSize() == 0);
    CHECK(d.debug);
    REQUIRE(d.misses.size() == 1);
}

TEST_CASE("dispel failed: full guids, the spell, the auras that held", "[combatlog]") {
    Writer w;
    w.u64(kPlayer);
    w.u64(kCreature);
    w.u32(527);  // Dispel Magic
    w.u32(1000);
    w.u32(2000);
    auto packet = w.packet();
    DispelFailedData d;
    REQUIRE(DispelFailedParser::parse(packet, d));
    CHECK(d.casterGuid == kPlayer);
    CHECK(d.victimGuid == kCreature);
    CHECK(d.spellId == 527u);
    CHECK(packet.getRemainingSize() == 0);
}

TEST_CASE("dispel log: the victim first", "[combatlog]") {
    Writer w;
    w.packedGuid(kCreature);  // victim
    w.packedGuid(kPlayer);    // caster
    w.u32(527);
    w.u8(0);  // not used
    w.u32(2);
    w.u32(1243);
    w.u8(0);
    w.u32(139);
    w.u8(1);
    auto packet = w.packet();
    SpellDispelLogData d;
    REQUIRE(SpellDispelLogParser::parse(packet, d));
    CHECK(packet.getRemainingSize() == 0);
    CHECK(d.victimGuid == kCreature);
    CHECK(d.casterGuid == kPlayer);
    CHECK(d.spellId == 527u);
    REQUIRE(d.auras.size() == 2);
    CHECK(d.auras[0].spellId == 1243u);
    CHECK(d.auras[1].spellId == 139u);
    CHECK(d.auras[1].flag == 1);
}

TEST_CASE("spell log execute: entries by the client's table", "[combatlog]") {
    Writer w;
    w.packedGuid(kPlayer);
    w.u32(2139);  // Counterspell
    w.u32(4);
    // SPELL_EFFECT_INTERRUPT_CAST (68): target, interrupted spell.
    w.u32(68);
    w.u32(1);
    w.packedGuid(kCreature);
    w.u32(133);
    // SPELL_EFFECT_POWER_DRAIN (8): target, amount, power, multiplier.
    w.u32(8);
    w.u32(1);
    w.packedGuid(kCreature);
    w.u32(250);
    w.u32(0);
    w.f32(1.0f);
    // SPELL_EFFECT_HEAL (10): nothing logged per entry.
    w.u32(10);
    w.u32(3);
    // SPELL_EFFECT_CREATE_ITEM_2 (157): the item.
    w.u32(157);
    w.u32(1);
    w.u32(5512);
    auto packet = w.packet();
    SpellLogExecuteData d;
    REQUIRE(SpellLogExecuteParser::parse(packet, d, false));
    CHECK(packet.getRemainingSize() == 0);
    CHECK(d.casterGuid == kPlayer);
    CHECK(d.spellId == 2139u);
    REQUIRE(d.entries.size() == 3);
    CHECK(d.entries[0].effect == 68u);
    CHECK(d.entries[0].targetGuid == kCreature);
    CHECK(d.entries[0].value1 == 133u);
    CHECK(d.entries[1].effect == 8u);
    CHECK(d.entries[1].value1 == 250u);
    CHECK(d.entries[1].value2 == 0u);
    CHECK(d.entries[1].multiplier == 1.0f);
    CHECK(d.entries[2].effect == 157u);
    CHECK(d.entries[2].value1 == 5512u);
}

TEST_CASE("spell log execute: durability damage and a summon", "[combatlog]") {
    Writer w;
    w.packedGuid(kCreature);
    w.u32(1);
    w.u32(2);
    w.u32(111);  // SPELL_EFFECT_DURABILITY_DAMAGE: target, item, slot
    w.u32(1);
    w.packedGuid(kPlayer);
    w.u32(0xFFFFFFFF);
    w.u32(0xFFFFFFFF);
    w.u32(28);  // SPELL_EFFECT_SUMMON: the summoned
    w.u32(1);
    w.packedGuid(kCreature + 7);
    auto packet = w.packet();
    SpellLogExecuteData d;
    REQUIRE(SpellLogExecuteParser::parse(packet, d, false));
    CHECK(packet.getRemainingSize() == 0);
    REQUIRE(d.entries.size() == 2);
    CHECK(d.entries[0].targetGuid == kPlayer);
    CHECK(d.entries[1].targetGuid == kCreature + 7);
}
