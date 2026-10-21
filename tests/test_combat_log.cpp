// The combat log as wow.exe 12340 keeps it (UnitCombatLog_C.cpp): the unit
// flags 0x0074dcb0 works out, the arguments 0x0074e290 pushes for each
// subevent, and the store the interface walks - entries held behind a name
// still being asked for (0x0074f910), the oldest given up only when a new one
// is made (0x00750400), CombatLogSetCurrentEntry / AdvanceEntry /
// GetNumEntries and CombatLogAddFilter's matching (0x0074e1a0).
#include <catch_amalgamated.hpp>

#include <map>
#include <set>

#include "game/combat_log.hpp"

using namespace wowee::game::combat_log;

namespace {

constexpr uint64_t kMe = 0x10;
constexpr uint64_t kFriend = 0x11;     // a party member
constexpr uint64_t kRaider = 0x12;     // in the raid, another group
constexpr uint64_t kStranger = 0x13;   // a player nobody knows
constexpr uint64_t kWolf = 0xF130000123000045ull;
constexpr uint64_t kMyPet = 0xF140000001000046ull;
constexpr uint64_t kTotem = 0xF130000456000047ull;
constexpr uint64_t kTrap = 0xF110000789000048ull;

struct FakeWorld : UnitWorld {
    bool raid = false;
    std::set<uint64_t> party, raiders;
    std::map<uint64_t, Object> objects;
    std::map<uint64_t, int> reactions;
    std::map<uint64_t, uint32_t> roles, marks;
    uint64_t targetGuid = 0, focusGuid = 0;

    uint64_t activePlayer() const override { return kMe; }
    uint64_t target() const override { return targetGuid; }
    uint64_t focus() const override { return focusGuid; }
    bool inRaid() const override { return raid; }
    bool hasPartyMembers() const override { return !party.empty(); }
    bool partyMember(uint64_t g) const override { return g == kMe || party.count(g) != 0; }
    bool raidMember(uint64_t g) const override { return raid && (g == kMe || raiders.count(g) != 0); }
    uint32_t memberFlags(uint64_t g, bool) const override {
        auto it = roles.find(g);
        return it != roles.end() ? it->second : 0;
    }
    uint32_t raidTarget(uint64_t g) const override {
        auto it = marks.find(g);
        return it != marks.end() ? it->second : 8;
    }
    Object object(uint64_t g) const override {
        auto it = objects.find(g);
        return it != objects.end() ? it->second : Object{};
    }
    std::optional<int> reactionToPlayer(uint64_t g) const override {
        auto it = reactions.find(g);
        if (it == reactions.end()) return std::nullopt;
        return it->second;
    }
};

UnitWorld::Object unit(uint64_t charmedBy = 0, uint64_t summonedBy = 0, uint64_t createdBy = 0) {
    return {.kind = UnitWorld::Object::Kind::Unit, .charmedBy = charmedBy, .summonedBy = summonedBy,
            .createdBy = createdBy};
}

const SpellLookup kSpells = [](uint32_t id) -> std::optional<SpellInfo> {
    if (id == 133) return SpellInfo{.name = "Feuerball", .school = 4};
    if (id == 2139) return SpellInfo{.name = "Gegenzauber", .school = 64};
    return std::nullopt;
};

Entry sides(Event event, uint32_t spellId = 0) {
    Entry e;
    e.event = event;
    e.src = {.guid = kMe, .name = "Ich", .realm = {}, .flags = 0x511, .waiting = {}};
    e.dst = {.guid = kWolf, .name = "Wolf", .realm = {}, .flags = 0xa48, .waiting = {}};
    e.spellId = spellId;
    return e;
}

/// The eight every entry starts with.
void checkHead(const std::vector<Arg>& a, const char* event) {
    REQUIRE(a.size() >= 8);
    CHECK(a[0] == Arg::num(1700000000.25));
    CHECK(a[1] == Arg::str(event));
    CHECK(a[2] == Arg::str("0x0000000000000010"));
    CHECK(a[3] == Arg::str("Ich"));
    CHECK(a[4] == Arg::num(0x511));
    CHECK(a[5] == Arg::str("0xF130000123000045"));
    CHECK(a[6] == Arg::str("Wolf"));
    CHECK(a[7] == Arg::num(0xa48));
}

std::vector<Arg> args(const Entry& e) { return entryArgs(e, 1700000000.25, kSpells); }

}  // namespace

TEST_CASE("Combat log subevent names are the client's table", "[combat_log]") {
    CHECK(std::string(eventName(Event::EnvironmentalDamage)) == "ENVIRONMENTAL_DAMAGE");
    CHECK(std::string(eventName(Event::SpellAuraBrokenSpell)) == "SPELL_AURA_BROKEN_SPELL");
    CHECK(std::string(eventName(Event::PartyKill)) == "PARTY_KILL");
    CHECK(std::string(eventName(Event::UnitDissipates)) == "UNIT_DISSIPATES");
    CHECK(static_cast<size_t>(Event::UnitDissipates) + 1 == kEventCount);
}

TEST_CASE("Guids are written and read as 0x0074d0d0 / 0x0074d120 do", "[combat_log]") {
    CHECK(guidString(0) == "0x0000000000000000");
    CHECK(guidString(0xF130000ABC000123ull) == "0xF130000ABC000123");
    CHECK(parseGuid("0xF130000ABC000123") == 0xF130000ABC000123ull);
    CHECK(parseGuid("f130000abc000123") == 0xF130000ABC000123ull);
    CHECK(parseGuid("0x12zz") == 0x12);
}

TEST_CASE("CombatLog_Object_IsA wants a special bit or every category", "[combat_log]") {
    CHECK(maskComplete(0x511));             // mine, friendly, player-controlled, player
    CHECK_FALSE(maskComplete(0x111));       // no type
    CHECK_FALSE(maskComplete(0x0));
    CHECK(maskComplete(0x10000));           // the target alone
    CHECK(maskComplete(0x80000000u));       // COMBATLOG_OBJECT_NONE
}

TEST_CASE("Unit flags as 0x0074dcb0 works them out", "[combat_log]") {
    FakeWorld w;
    SECTION("nothing at all") { CHECK(unitFlags(w, 0) == object::kNone); }
    SECTION("the player") {
        w.objects[kMe] = unit();
        CHECK(unitFlags(w, kMe) == (object::kAffiliationMine | object::kReactionFriendly |
                                   object::kControlPlayer | object::kTypePlayer));
    }
    SECTION("a hostile creature is an outsider, and not asked about groups") {
        w.objects[kWolf] = unit();
        w.reactions[kWolf] = 1;
        w.party = {kFriend};
        CHECK(unitFlags(w, kWolf) == (object::kAffiliationOutsider | object::kReactionHostile |
                                     object::kControlNpc | object::kTypeNpc));
    }
    SECTION("a neutral creature, and one not in sight") {
        w.objects[kWolf] = unit();
        w.reactions[kWolf] = 3;
        CHECK(unitFlags(w, kWolf) == 0xa28);
        CHECK(unitFlags(w, 0xF130000999000001ull) == 0xa28);
    }
    SECTION("a party member, the raid and a stranger") {
        w.party = {kFriend};
        w.raid = true;
        w.raiders = {kFriend, kRaider};
        CHECK(unitFlags(w, kFriend) == 0x512);
        CHECK(unitFlags(w, kRaider) == 0x514);
        CHECK(unitFlags(w, kStranger) == 0x528);
    }
    SECTION("the player's pet and totem, through their owner") {
        w.objects[kMyPet] = unit(0, kMe, kMe);
        w.objects[kTotem] = unit(0, 0, kMe);
        CHECK(unitFlags(w, kMyPet) == (object::kAffiliationMine | object::kReactionFriendly |
                                      object::kControlPlayer | object::kTypePet));
        // Made, but neither charmed nor summoned: a guardian.
        CHECK(unitFlags(w, kTotem) == 0x2111);
    }
    SECTION("a game object is its maker's") {
        w.objects[kTrap] = {.kind = UnitWorld::Object::Kind::GameObject, .charmedBy = 0, .summonedBy = 0,
                            .createdBy = kMe};
        CHECK(unitFlags(w, kTrap) == 0x4111);
    }
    SECTION("target, focus, roles and the raid mark") {
        w.party = {kFriend};
        w.targetGuid = kFriend;
        w.focusGuid = kFriend;
        w.roles[kFriend] = 2;
        w.marks[kFriend] = 7;
        CHECK(unitFlags(w, kFriend) == (0x512u | object::kTarget | object::kFocus | object::kMainTank |
                                        (object::kRaidTarget1 << 7)));
    }
    SECTION("alone, the player's own mark and role are not looked at") {
        w.marks[kMe] = 0;
        w.roles[kMe] = 4;
        CHECK(unitFlags(w, kMe) == 0x511);
    }
}

TEST_CASE("Every subevent's arguments, in 0x0074e290's order", "[combat_log]") {
    SECTION("SWING_DAMAGE: the damage suffix, nil for what is zero") {
        Entry e = sides(Event::SwingDamage);
        e.suffix = suffix::kDamage;
        e.damage = {.amount = 120, .overkill = 0, .school = 1, .resisted = 0, .blocked = 30, .absorbed = 0,
                    .critical = true, .glancing = false, .crushing = false};
        const auto a = args(e);
        checkHead(a, "SWING_DAMAGE");
        REQUIRE(a.size() == 17);
        CHECK(a[8] == Arg::num(120));
        CHECK(a[9] == Arg::num(0));
        CHECK(a[10] == Arg::num(1));
        CHECK(a[11] == Arg::nil());
        CHECK(a[12] == Arg::num(30));
        CHECK(a[13] == Arg::nil());
        CHECK(a[14] == Arg::num(1));
        CHECK(a[15] == Arg::nil());
        CHECK(a[16] == Arg::nil());
    }
    SECTION("SPELL_DAMAGE: the spell, then the damage") {
        Entry e = sides(Event::SpellDamage, 133);
        e.suffix = suffix::kDamage;
        e.damage.amount = 500;
        e.damage.school = 4;
        e.damage.resisted = 50;
        const auto a = args(e);
        checkHead(a, "SPELL_DAMAGE");
        REQUIRE(a.size() == 20);
        CHECK(a[8] == Arg::num(133));
        CHECK(a[9] == Arg::str("Feuerball"));
        CHECK(a[10] == Arg::num(4));
        CHECK(a[11] == Arg::num(500));
        CHECK(a[14] == Arg::num(50));
    }
    SECTION("a spell the client has no row of: nil name, school 0") {
        Entry e = sides(Event::SpellInstakill, 99999);
        const auto a = args(e);
        REQUIRE(a.size() == 11);
        CHECK(a[9] == Arg::nil());
        CHECK(a[10] == Arg::num(0));
    }
    SECTION("SWING_MISSED and SPELL_MISSED: the miss, an amount where one is named") {
        Entry e = sides(Event::SwingMissed);
        e.suffix = suffix::kMissType;
        e.missType = miss::kDodge;
        auto a = args(e);
        REQUIRE(a.size() == 9);
        CHECK(a[8] == Arg::str("DODGE"));

        e = sides(Event::SpellMissed, 133);
        e.suffix = suffix::kMissType | suffix::kMissAbsorbed;
        e.missType = miss::kAbsorb;
        e.missAbsorbed = 210;
        a = args(e);
        REQUIRE(a.size() == 13);
        CHECK(a[11] == Arg::str("ABSORB"));
        CHECK(a[12] == Arg::num(210));
    }
    SECTION("ENVIRONMENTAL_DAMAGE: the kind, then the damage") {
        Entry e = sides(Event::EnvironmentalDamage);
        e.src = {};
        e.suffix = suffix::kEnvironmental | suffix::kDamage;
        e.environmentalType = 2;
        e.damage.amount = 77;
        e.damage.school = 1;
        const auto a = args(e);
        REQUIRE(a.size() == 18);
        CHECK(a[2] == Arg::str("0x0000000000000000"));
        CHECK(a[3] == Arg::nil());
        CHECK(a[4] == Arg::num(0x80000000u));
        CHECK(a[8] == Arg::str("FALLING"));
        CHECK(a[9] == Arg::num(77));
    }
    SECTION("SPELL_HEAL: amount, overhealing, absorbed, critical") {
        Entry e = sides(Event::SpellHeal, 133);
        e.suffix = suffix::kHeal;
        e.heal = {.amount = 900, .overhealing = 100, .absorbed = 0, .critical = false};
        const auto a = args(e);
        REQUIRE(a.size() == 15);
        CHECK(a[11] == Arg::num(900));
        CHECK(a[12] == Arg::num(100));
        CHECK(a[13] == Arg::num(0));
        CHECK(a[14] == Arg::nil());
    }
    SECTION("SPELL_ENERGIZE, SPELL_DRAIN and SPELL_LEECH") {
        Entry e = sides(Event::SpellEnergize, 133);
        e.suffix = suffix::kEnergize;
        e.power = {.amount = 15, .powerType = 1, .extraAmount = 0};
        auto a = args(e);
        REQUIRE(a.size() == 13);
        CHECK(a[11] == Arg::num(15));
        CHECK(a[12] == Arg::num(1));

        e = sides(Event::SpellDrain, 133);
        e.suffix = suffix::kDrain;
        e.power = {.amount = 300, .powerType = 0, .extraAmount = 0};
        a = args(e);
        REQUIRE(a.size() == 14);
        CHECK(a[13] == Arg::nil());
        e.power.extraAmount = 150;
        CHECK(args(e)[13] == Arg::num(150));
    }
    SECTION("SPELL_INTERRUPT and SPELL_DISPEL: the second spell, and the aura's kind") {
        Entry e = sides(Event::SpellInterrupt, 2139);
        e.suffix = suffix::kExtraSpell;
        e.extraSpellId = 133;
        auto a = args(e);
        REQUIRE(a.size() == 14);
        CHECK(a[11] == Arg::num(133));
        CHECK(a[12] == Arg::str("Feuerball"));
        CHECK(a[13] == Arg::num(4));

        e = sides(Event::SpellDispel, 2139);
        e.suffix = suffix::kExtraSpell | suffix::kAuraType;
        e.extraSpellId = 133;
        e.auraDebuff = true;
        a = args(e);
        REQUIRE(a.size() == 15);
        CHECK(a[14] == Arg::str("DEBUFF"));
    }
    SECTION("SPELL_AURA_APPLIED and _APPLIED_DOSE") {
        Entry e = sides(Event::SpellAuraApplied, 133);
        e.suffix = suffix::kAuraType;
        auto a = args(e);
        REQUIRE(a.size() == 12);
        CHECK(a[11] == Arg::str("BUFF"));

        e = sides(Event::SpellAuraAppliedDose, 133);
        e.suffix = suffix::kAuraType | suffix::kAuraAmount;
        e.auraAmount = 3;
        a = args(e);
        REQUIRE(a.size() == 13);
        CHECK(a[12] == Arg::num(3));
    }
    SECTION("ENCHANT_APPLIED: the enchantment's name, then the item") {
        Entry e = sides(Event::EnchantApplied);
        e.suffix = suffix::kEnchantName | suffix::kItem;
        e.enchantName = "+20 Ausdauer";
        e.itemId = 41383;
        e.itemName = "Ring";
        const auto a = args(e);
        REQUIRE(a.size() == 11);
        CHECK(a[8] == Arg::str("+20 Ausdauer"));
        CHECK(a[9] == Arg::num(41383));
        CHECK(a[10] == Arg::str("Ring"));
    }
    SECTION("SPELL_CAST_FAILED, SPELL_EXTRA_ATTACKS, SPELL_DURABILITY_DAMAGE") {
        Entry e = sides(Event::SpellCastFailed, 133);
        e.suffix = suffix::kFailedType;
        e.failedType = "Nicht genug Mana";
        CHECK(args(e).back() == Arg::str("Nicht genug Mana"));
        CHECK(args(e).size() == 12);

        e = sides(Event::SpellExtraAttacks, 133);
        e.suffix = suffix::kAmount;
        e.amount = 2;
        CHECK(args(e).size() == 12);
        CHECK(args(e).back() == Arg::num(2));

        e = sides(Event::SpellDurabilityDamage, 133);
        e.suffix = suffix::kItem;
        e.itemId = 7;
        CHECK(args(e).size() == 13);
        CHECK(args(e).back() == Arg::nil());  // a name not yet known
    }
    SECTION("PARTY_KILL and UNIT_DIED carry the two sides alone") {
        CHECK(args(sides(Event::PartyKill)).size() == 8);
        Entry e = sides(Event::UnitDied);
        e.src = {};
        CHECK(args(e).size() == 8);
    }
    SECTION("a cross-realm name is name-realm") {
        Entry e = sides(Event::PartyKill);
        e.src.realm = "Anderswo";
        CHECK(args(e)[3] == Arg::str("Ich-Anderswo"));
    }
}

namespace {

struct Harness {
    Log log;
    int64_t now = 100000;
    int retention = 300;
    std::vector<std::pair<std::string, bool>> announced;
    std::set<uint64_t> hidden;

    Harness() {
        Log::Hooks h;
        h.nowMs = [this] { return now; };
        h.retentionSeconds = [this] { return retention; };
        h.hidesGuid = [this](uint64_t g) { return hidden.count(g) != 0; };
        h.announce = [this](const Entry& e, bool filtered) {
            announced.emplace_back(e.src.name.value_or("") + ">" + e.dst.name.value_or(""), filtered);
        };
        log.setHooks(std::move(h));
        log.reset(now, 1700000000);
    }
    void add(const std::string& src, Event event = Event::SwingDamage, std::optional<NameKey> wait = {},
             uint64_t dst = kWolf) {
        Entry& e = log.create(event);
        e.src.guid = kMe;
        e.src.flags = 0x511;
        if (wait) e.src.waiting = wait;
        else e.src.name = src;
        e.dst.guid = dst;
        e.dst.name = "Wolf";
        e.dst.flags = 0xa48;
        log.commit(0);
    }
    std::vector<std::string> walkBack() {
        std::vector<std::string> out;
        bool valid = log.setCurrent(0, false);
        while (valid) {
            out.push_back(*log.current()->src.name);
            valid = log.advance(-1, false);
        }
        return out;
    }
};

}  // namespace

TEST_CASE("Entries are announced in order, held behind a name being asked for", "[combat_log]") {
    Harness h;
    h.add("A");
    const NameKey key{.kind = NameKey::Kind::Player, .id = kMe};
    h.add("", Event::SwingDamage, key);
    h.add("C");
    REQUIRE(h.announced.size() == 1);
    CHECK(h.log.numEntries(true) == 1);  // up to the one still waiting
    h.log.answer(key, "B", {});
    REQUIRE(h.announced.size() == 3);
    CHECK(h.announced[1].first == "B>Wolf");
    CHECK(h.announced[2].first == "C>Wolf");
    CHECK(h.log.numEntries(true) == 3);
}

TEST_CASE("The refilter walk: newest first, back to the oldest", "[combat_log]") {
    Harness h;
    h.add("A");
    h.add("B");
    h.add("C");
    CHECK(h.walkBack() == std::vector<std::string>{"C", "B", "A"});
    CHECK(h.log.setCurrent(1, false));
    CHECK(*h.log.current()->src.name == "A");
    CHECK(h.log.advance(2, false));
    CHECK(*h.log.current()->src.name == "C");
    CHECK_FALSE(h.log.advance(1, false));  // past the newest is no entry
    CHECK(h.log.current() == nullptr);
    CHECK_FALSE(h.log.setCurrent(4, false));
}

TEST_CASE("CombatLogAddFilter: events, masks, guids and spells", "[combat_log]") {
    Harness h;
    Filter mine;
    mine.setEvents(std::string("SPELL_DAMAGE, swing_damage"));
    mine.srcMask = object::kAffiliationMine | 0xF0 | 0x300 | 0xFC00;
    mine.dstMask = 0xFFFFFFFFu;
    h.log.addFilter(mine);
    h.add("A", Event::SwingDamage);
    h.add("B", Event::SpellHeal);
    REQUIRE(h.announced.size() == 2);
    CHECK(h.announced[0].second);
    CHECK_FALSE(h.announced[1].second);
    CHECK(h.log.numEntries(false) == 1);
    CHECK(h.log.numEntries(true) == 2);
    CHECK(h.walkBack() == std::vector<std::string>{"A"});

    SECTION("an empty list takes nothing, nil takes everything") {
        Filter none;
        none.setEvents(std::string(""));
        none.srcMask = none.dstMask = 0xFFFFFFFFu;
        Entry e;
        e.event = Event::SwingDamage;
        e.src.flags = e.dst.flags = 0x511;
        CHECK_FALSE(none.matches(e));
        none.setEvents(std::nullopt);
        CHECK(none.matches(e));
    }
    SECTION("a guid side matches that unit alone") {
        Filter byGuid;
        byGuid.setEvents(std::nullopt);
        byGuid.srcMask = 0xFFFFFFFFu;
        byGuid.dstGuid = kWolf;
        Entry e;
        e.event = Event::PartyKill;
        e.src.flags = 0x511;
        e.dst.guid = kWolf;
        CHECK(byGuid.matches(e));
        e.dst.guid = kTotem;
        CHECK_FALSE(byGuid.matches(e));
    }
    SECTION("a spell by id or by name") {
        Filter bySpell;
        bySpell.setEvents(std::nullopt);
        bySpell.srcMask = bySpell.dstMask = 0xFFFFFFFFu;
        bySpell.spellName = "feuerball";
        Entry e;
        e.event = Event::SpellDamage;
        e.src.flags = e.dst.flags = 0x511;
        e.spellId = 133;
        e.filterSpellName = "Feuerball";
        CHECK(bySpell.matches(e));
        e.filterSpellName.reset();
        CHECK_FALSE(bySpell.matches(e));
        bySpell.spellName.reset();
        bySpell.spellId = 134;
        CHECK_FALSE(bySpell.matches(e));
    }
}

TEST_CASE("The oldest goes when a new entry is made after the retention", "[combat_log]") {
    Harness h;
    h.retention = 10;
    h.add("A");
    h.add("B");
    h.now += 10000;
    h.add("C");  // A has been kept 10 s: its place is taken
    CHECK(h.log.size() == 2);
    h.add("D");  // and B too, one at a time
    CHECK(h.log.size() == 2);
    CHECK(h.walkBack() == std::vector<std::string>{"D", "C"});
    h.log.clearEntries();
    CHECK(h.log.size() == 0);
    CHECK(h.log.current() == nullptr);
}

TEST_CASE("Timestamps count from when the log began, to the millisecond", "[combat_log]") {
    Harness h;
    h.now += 1500;
    h.add("A");
    REQUIRE(h.log.setCurrent(0, true));
    CHECK(h.log.timestampOf(*h.log.current()) == Catch::Approx(1700000001.5));
}

TEST_CASE("A creature whose template hides it is logged without its spawn", "[combat_log]") {
    Harness h;
    h.hidden.insert(kWolf);
    h.add("A");
    REQUIRE(h.log.setCurrent(0, true));
    CHECK(h.log.current()->dst.guid == (kWolf & 0xFFFFFFFFFF000000ull));
}
