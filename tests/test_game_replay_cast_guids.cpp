// The caster guid of SMSG_SPELL_FAILURE, SMSG_SPELL_FAILED_OTHER and
// SMSG_SPELL_DELAYED through GameHandler's own dispatch: packed on TBC and
// WotLK, as cmangos-tbc and OregonCore write it, and whole on classic. See
// game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"

#include <string>
#include <utility>
#include <vector>

using replay::Session;

namespace {

using wowee::game::Opcode;
using wowee::game::wireOpcode;
using wowee::network::Packet;

constexpr uint64_t kMob = 0xF130000000000123ull;

// The names of the events the session fired whose first argument is "target".
struct TargetEvents {
    std::vector<std::string> names;

    Session& session;

    // Unhooked before the session is torn down, which still fires events.
    ~TargetEvents() { session.handler().setAddonEventCallback({}); }

    explicit TargetEvents(Session& s) : session(s) {
        s.handler().setTargetGuidRaw(kMob);
        s.handler().setAddonEventCallback([this](const std::string& name, const std::vector<std::string>& args) {
            if (!args.empty() && args.front() == "target") names.push_back(name);
        });
    }
};

void writeCaster(Packet& p, bool classic) {
    if (classic) p.writeUInt64(kMob);
    else p.writePackedGuid(kMob);
}

}  // namespace

TEST_CASE("an interrupted cast names its caster as the server writes it", "[replay][castguid]") {
    for (const char* expansion : {"wotlk", "tbc", "classic"}) {
        CAPTURE(expansion);
        const std::string e = expansion;
        Session s(expansion);
        s.handler().spellNameCacheRef()[133].name = "Fireball";
        TargetEvents events(s);
        // Spell::SendInterrupted's SMSG_SPELL_FAILURE: WotLK adds the cast
        // count. Classic's whole guid is the layout this client has always
        // read; vmangos never sends the packet and cmangos-classic packs it.
        Packet p(wireOpcode(Opcode::SMSG_SPELL_FAILURE));
        writeCaster(p, e == "classic");
        if (e == "wotlk") p.writeUInt8(0);
        p.writeUInt32(133);
        p.writeUInt8(0);  // result
        s.receive(p);
        s.handler().setAddonEventCallback({});
        // The event follows the result (0x007fecc0 via 0x00809c70 in ours); what
        // this case proves is that the caster was read, so it reaches "target".
        // No stop: no cast of the mob's was shown.
        CHECK(events.names == std::vector<std::string>{"UNIT_SPELLCAST_FAILED"});
    }
}

TEST_CASE("the player's own failure is read in each expansion's layout", "[replay][castguid]") {
    // Upstream checked for a chat line here; ours says nothing for the
    // player's failure (0x00809c70 only ends the cast, the reason came with
    // SMSG_CAST_FAILED), so the layout is proved by the events instead.
    for (const char* expansion : {"wotlk", "tbc"}) {
        CAPTURE(expansion);
        Session s(expansion);
        s.handler().spellNameCacheRef()[133].name = "Fireball";
        std::vector<std::string> names;
        s.handler().setAddonEventCallback([&names](const std::string& name, const std::vector<std::string>& args) {
            if (!args.empty() && args.front() == "player") names.push_back(name);
        });
        Packet p(wireOpcode(Opcode::SMSG_SPELL_FAILURE));
        p.writePackedGuid(Session::kPlayerGuid);
        if (std::string(expansion) == "wotlk") p.writeUInt8(0);
        p.writeUInt32(133);
        p.writeUInt8(40);
        s.receive(p);
        s.handler().setAddonEventCallback({});
        CHECK(names == std::vector<std::string>{"UNIT_SPELLCAST_INTERRUPTED"});
    }
}

// The cast SMSG_SPELL_DELAYED moves is the one the unit has (0x00801b80),
// so one is started first - in each expansion's SMSG_SPELL_START layout.
static void startCast(Session& s, const std::string& expansion) {
    auto& fireball = s.handler().spellNameCacheRef()[133];
    fireball.name = "Fireball";
    Packet p(wireOpcode(Opcode::SMSG_SPELL_START));
    p.writePackedGuid(kMob);
    p.writePackedGuid(kMob);
    if (expansion == "wotlk") {
        p.writeUInt8(1);
        p.writeUInt32(133);
        p.writeUInt32(0);
        p.writeUInt32(3000);
        p.writeUInt32(0);  // target flags
    } else if (expansion == "tbc") {
        p.writeUInt32(133);
        p.writeUInt8(1);
        p.writeUInt16(0);
        p.writeUInt32(3000);
        p.writeUInt32(0);
    } else {
        p.writeUInt32(133);
        p.writeUInt16(0);
        p.writeUInt32(3000);
        p.writeUInt16(0);
    }
    s.receive(p);
}

TEST_CASE("another unit's failed cast names its caster as the server writes it", "[replay][castguid]") {
    for (const char* expansion : {"wotlk", "tbc", "classic"}) {
        CAPTURE(expansion);
        const std::string e = expansion;
        Session s(expansion);
        TargetEvents events(s);
        startCast(s, e);
        CHECK(events.names == std::vector<std::string>{"UNIT_SPELLCAST_START"});
        events.names.clear();
        Packet p(wireOpcode(Opcode::SMSG_SPELL_FAILED_OTHER));
        writeCaster(p, e == "classic");
        if (e == "wotlk") p.writeUInt8(1);
        p.writeUInt32(133);
        p.writeUInt8(0);
        s.receive(p);
        CHECK(events.names == std::vector<std::string>{"UNIT_SPELLCAST_FAILED", "UNIT_SPELLCAST_STOP"});
    }
}

TEST_CASE("a pushed-back cast names its caster as the server writes it", "[replay][castguid]") {
    for (const char* expansion : {"wotlk", "tbc", "classic"}) {
        CAPTURE(expansion);
        Session s(expansion);
        TargetEvents events(s);
        startCast(s, expansion);
        events.names.clear();
        // Spell::Delayed's SMSG_SPELL_DELAYED.
        Packet p(wireOpcode(Opcode::SMSG_SPELL_DELAYED));
        writeCaster(p, std::string(expansion) == "classic");
        p.writeUInt32(500);
        s.receive(p);
        CHECK(events.names == std::vector<std::string>{"UNIT_SPELLCAST_DELAYED"});
    }
}

TEST_CASE("a delay for a unit casting nothing raises nothing", "[replay][castguid]") {
    // 0x00801b80 looks the unit's own cast up; with none there is no record
    // and so no event.
    Session s;
    TargetEvents events(s);
    Packet p(wireOpcode(Opcode::SMSG_SPELL_DELAYED));
    writeCaster(p, false);
    p.writeUInt32(500);
    s.receive(p);
    CHECK(events.names.empty());
}
