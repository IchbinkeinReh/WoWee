// The stance bar's list, which is the spellbook's and not a class's: 0x00542030
// puts a learned spell on it, 0x0053fad0 takes it off, 0x0053c680 sorts it.
// Every row below is 3.3.5a's Spell.dbc (patch-deDE-3.MPQ).
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"

#include <algorithm>
#include <string>
#include <vector>

using replay::Session;
using replay::UpdateObject;
using wowee::game::Opcode;
using wowee::game::UF;

namespace {

struct SpellRow {
    uint32_t id;
    uint32_t attrEx2;
    uint32_t auras[3];
    int32_t order;
    uint32_t activeIcon;
};

constexpr uint32_t kDevotion9 = 48941, kDevotion10 = 48942, kRetribution = 54043, kConcentration = 19746;
constexpr uint32_t kShadow = 48943, kFrost = 48945, kFire = 48947, kCrusader = 32223;
constexpr uint32_t kMetamorphosis = 47241, kGhostWolf = 2645, kHolyLight = 635;
constexpr uint32_t kBlood = 48266, kFrostPresence = 48263, kUnholy = 48265;
constexpr uint32_t kStealth = 1784, kShadowDance = 51713;
constexpr uint32_t kBear = 5487, kDireBear = 9634, kAquatic = 1066, kCat = 768, kTravel = 783, kMoonkin = 24858;
constexpr uint32_t kFlight = 33943, kSwiftFlight = 40120;
constexpr uint32_t kBattle = 2457, kDefensive = 71, kBerserker = 2458, kShadowform = 15473;

const SpellRow kSpells[] = {
    {kDevotion9, 0x11, {22, 0, 0}, 0, 122},      {kDevotion10, 0x11, {22, 0, 0}, 0, 122},
    {kRetribution, 0x11, {15, 79, 193}, 1, 122}, {kConcentration, 0x11, {149, 0, 0}, 2, 122},
    {kShadow, 0x11, {143, 0, 0}, 3, 122},        {kFrost, 0x11, {143, 0, 0}, 4, 122},
    {kFire, 0x11, {143, 0, 0}, 5, 122},          {kCrusader, 0x11, {172, 211, 210}, 7, 122},
    {kMetamorphosis, 0x0, {36, 142, 79}, 0, 0},  {kGhostWolf, 0x2, {36, 31, 20}, 0, 122},
    {kBlood, 0x11, {79, 118, 10}, 0, 2636},      {kFrostPresence, 0x11, {142, 10, 87}, 1, 2632},
    {kUnholy, 0x11, {138, 107, 10}, 2, 2633},    {kStealth, 0x200000, {36, 16, 33}, 0, 30},
    {kShadowDance, 0x0, {275, 0, 36}, 1, 0},     {kBear, 0x0, {36, 77, 0}, 0, 122},
    {kDireBear, 0x0, {36, 77, 0}, 0, 122},       {kAquatic, 0x0, {36, 77, 0}, 1, 122},
    {kCat, 0x0, {36, 77, 23}, 2, 122},           {kTravel, 0x0, {36, 77, 0}, 3, 122},
    {kMoonkin, 0x0, {36, 77, 0}, 4, 122},        {kFlight, 0x0, {36, 77, 201}, 5, 122},
    {kSwiftFlight, 0x0, {36, 77, 201}, 5, 122},  {kBattle, 0x1, {36, 280, 0}, 0, 279},
    {kDefensive, 0x0, {36, 0, 0}, 1, 276},       {kBerserker, 0x1, {36, 0, 0}, 2, 84},
    {kShadowform, 0x0, {36, 79, 87}, 0, 55},     {kHolyLight, 0x0, {0, 0, 0}, 0, 0},
};

// A character of one class in the world, with Spell.dbc's rows and the events
// it raises.
struct Book {
    Session s;
    std::vector<std::string> events;

    explicit Book(uint32_t classId) {
        s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}, {UF::UNIT_FIELD_LEVEL, 80},
                       {UF::UNIT_FIELD_BYTES_0, 1u | (classId << 8)}});
        for (const SpellRow& r : kSpells) {
            auto& e = s.handler().spellNameCacheRef()[r.id];
            e.name = "spell " + std::to_string(r.id);
            e.attrEx2 = r.attrEx2;
            for (int k = 0; k < 3; ++k) {
                e.effectIds[k] = r.auras[k] != 0 ? 6u : 0u;
                e.effectAuraIds[k] = r.auras[k];
            }
            e.stanceBarOrder = r.order;
            e.activeIconId = r.activeIcon;
        }
        s.handler().setAddonEventCallback(
            [this](const std::string& event, const std::vector<std::string>&) { events.push_back(event); });
    }
    ~Book() { s.handler().setAddonEventCallback({}); }

    void initialSpells(const std::vector<uint32_t>& ids) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_INITIAL_SPELLS));
        p.writeUInt8(0);
        p.writeUInt16(static_cast<uint16_t>(ids.size()));
        for (uint32_t id : ids) {
            p.writeUInt32(id);
            p.writeUInt16(0);
        }
        p.writeUInt16(0);
        s.receive(p);
    }
    void packet(Opcode op, uint32_t a, uint32_t b = 0) {
        wowee::network::Packet p(wowee::game::wireOpcode(op));
        p.writeUInt32(a);
        if (op == Opcode::SMSG_SUPERCEDED_SPELL) p.writeUInt32(b);
        else if (op == Opcode::SMSG_LEARNED_SPELL) p.writeUInt16(0);
        s.receive(p);
    }
    // AuraApplication::BuildUpdatePacket for the player's own aura, or the
    // slot emptied.
    void aura(uint8_t slot, uint32_t spell) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_AURA_UPDATE));
        p.writePackedGuid(Session::kPlayerGuid);
        p.writeUInt8(slot);
        p.writeUInt32(spell);
        if (spell != 0) {
            p.writeUInt8(0x10 | 0x08 | 0x01);
            p.writeUInt8(80);
            p.writeUInt8(0);
        }
        s.receive(p);
    }
    std::vector<uint32_t> bar() { return s.handler().stanceBarSpells(); }
    size_t heard(const std::string& event) {
        const auto n = static_cast<size_t>(std::count(events.begin(), events.end(), event));
        events.clear();
        return n;
    }
};

using Bar = std::vector<uint32_t>;

}  // namespace

// Class 2 had no list: ShapeshiftBarFrame stayed hidden (playtest P17).
TEST_CASE("a paladin's auras are the stance bar", "[replay][stancebar]") {
    Book book(2);
    // As the server sends them: in no order, among the rest.
    book.initialSpells({kCrusader, kHolyLight, kFire, kRetribution, kFrost, kDevotion10, kShadow, kConcentration});
    CHECK(book.bar() == Bar{kDevotion10, kRetribution, kConcentration, kShadow, kFrost, kFire, kCrusader});
    CHECK(book.heard("UPDATE_SHAPESHIFT_FORMS") == 1);
}

TEST_CASE("every class's stance bar is what its spells say", "[replay][stancebar]") {
    struct Case {
        uint32_t classId;
        Bar known, bar;
    };
    const Case cases[] = {
        {1, {kBerserker, kBattle, kDefensive}, {kBattle, kDefensive, kBerserker}},
        // Shadow Dance shapeshifts by its third effect.
        {4, {kShadowDance, kStealth}, {kStealth, kShadowDance}},
        {5, {kShadowform}, {kShadowform}},
        {6, {kUnholy, kBlood, kFrostPresence}, {kBlood, kFrostPresence, kUnholy}},
        // Ghost Wolf shapeshifts and is AttributesEx2 0x2.
        {7, {kGhostWolf}, {}},
        {9, {kMetamorphosis}, {kMetamorphosis}},
        {11, {kSwiftFlight, kMoonkin, kCat, kTravel, kAquatic, kDireBear},
             {kDireBear, kAquatic, kCat, kTravel, kMoonkin, kSwiftFlight}},
    };
    for (const Case& c : cases) {
        INFO("class " << c.classId);
        Book book(c.classId);
        book.initialSpells(c.known);
        CHECK(book.bar() == c.bar);
    }
}

TEST_CASE("the stance bar changes as spells are learned and forgotten", "[replay][stancebar]") {
    Book book(2);
    book.initialSpells({kRetribution, kDevotion9});
    book.events.clear();

    book.packet(Opcode::SMSG_LEARNED_SPELL, kConcentration);
    CHECK(book.bar() == Bar{kDevotion9, kRetribution, kConcentration});
    CHECK(book.heard("UPDATE_SHAPESHIFT_FORMS") == 1);

    // A spell that is none of the bar's says nothing of it.
    book.packet(Opcode::SMSG_LEARNED_SPELL, kHolyLight);
    CHECK(book.heard("UPDATE_SHAPESHIFT_FORMS") == 0);

    // A higher rank: on the old one's button (0x005424c5).
    book.packet(Opcode::SMSG_SUPERCEDED_SPELL, kDevotion9, kDevotion10);
    CHECK(book.bar() == Bar{kDevotion10, kRetribution, kConcentration});
    CHECK(book.heard("UPDATE_SHAPESHIFT_FORMS") == 1);
    // The learned packet that may follow finds it there.
    book.packet(Opcode::SMSG_LEARNED_SPELL, kDevotion10);
    CHECK(book.bar().size() == 3);
    CHECK(book.heard("UPDATE_SHAPESHIFT_FORMS") == 0);

    book.packet(Opcode::SMSG_REMOVED_SPELL, kRetribution);
    CHECK(book.bar() == Bar{kDevotion10, kConcentration});
    CHECK(book.heard("UPDATE_SHAPESHIFT_FORMS") == 1);

    // The next login's book replaces it.
    book.initialSpells({kCrusader});
    CHECK(book.bar() == Bar{kCrusader});
}

TEST_CASE("Dire Bear and swift flight take their forms' buttons", "[replay][stancebar]") {
    Book book(11);
    book.initialSpells({kBear, kCat, kFlight});
    book.packet(Opcode::SMSG_SUPERCEDED_SPELL, kBear, kDireBear);
    book.packet(Opcode::SMSG_SUPERCEDED_SPELL, kFlight, kSwiftFlight);
    CHECK(book.bar() == Bar{kDireBear, kCat, kSwiftFlight});
}

TEST_CASE("an aura's button is active while its aura is, and pages nothing", "[replay][stancebar]") {
    Book book(2);
    book.initialSpells({kDevotion10, kRetribution, kConcentration});
    auto& gh = book.s.handler();
    book.aura(0, kRetribution);
    CHECK(gh.formSpellInEffect(kRetribution));
    CHECK_FALSE(gh.formSpellInEffect(kDevotion10));
    CHECK(book.heard("UPDATE_SHAPESHIFT_FORM") == 1);
    // No form: the action bar keeps its page (0x005a83c0).
    CHECK(gh.getShapeshiftFormId() == 0);
    CHECK(gh.getBonusActionBarOffset() == 0);

    // Another aura takes its slot.
    book.aura(0, kDevotion10);
    CHECK(gh.formSpellInEffect(kDevotion10));
    CHECK_FALSE(gh.formSpellInEffect(kRetribution));
    CHECK(book.heard("UPDATE_SHAPESHIFT_FORM") == 1);
    CHECK(book.heard("UPDATE_BONUS_ACTIONBAR") == 0);

    // Pressed while it is on, it is taken off (0x0080da80 -> 0x00802f80).
    gh.castStanceBarSpell(0);
    const auto cancels = book.s.sent().of(Opcode::CMSG_CANCEL_AURA);
    REQUIRE(cancels.size() == 1);
    CHECK(book.s.sent().of(Opcode::CMSG_CAST_SPELL).empty());
    book.aura(0, 0);
    CHECK_FALSE(gh.formSpellInEffect(kDevotion10));
    CHECK(book.heard("UPDATE_SHAPESHIFT_FORM") == 1);
    // Past the bar's end nothing is cast.
    gh.castStanceBarSpell(3);
    CHECK(book.s.sent().of(Opcode::CMSG_CAST_SPELL).empty());
}

// 0x0080da80, which the action bar, the spellbook and CastSpellByName press a
// spell through: one in effect is taken off, "!" and CastSpellByID cast it.
TEST_CASE("a spell pressed while it is in effect is taken off", "[replay][stancebar]") {
    Book book(2);
    book.initialSpells({kDevotion10, kRetribution, kHolyLight});
    auto& gh = book.s.handler();
    const auto cancels = [&] { return book.s.sent().of(Opcode::CMSG_CANCEL_AURA).size(); };
    const auto casts = [&] { return book.s.sent().of(Opcode::CMSG_CAST_SPELL).size(); };
    book.aura(0, kRetribution);
    SECTION("pressed") {
        gh.useSpell(kRetribution, 0);
        REQUIRE(cancels() == 1);
        auto p = book.s.sent().of(Opcode::CMSG_CANCEL_AURA).front();
        CHECK(p.readUInt32() == kRetribution);
        CHECK(casts() == 0);
    }
    SECTION("cast outright") {
        gh.castSpell(kRetribution, 0);
        CHECK(cancels() == 0);
        CHECK(casts() == 1);
    }
    SECTION("another is cast") {
        gh.useSpell(kDevotion10, 0);
        CHECK(cancels() == 0);
        CHECK(casts() == 1);
    }
    SECTION("an aura with no icon for being on is cast again") {
        book.aura(1, kHolyLight);
        gh.useSpell(kHolyLight, Session::kPlayerGuid);
        CHECK(cancels() == 0);
        CHECK(casts() == 1);
    }
    SECTION("not while charmed") {
        book.s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_CHARMEDBY, 0x99}}));
        gh.useSpell(kRetribution, 0);
        CHECK(cancels() == 0);
        CHECK(casts() == 0);
    }
}

// 0x00802cb0: a spell with no ActiveIconID is never in effect.
TEST_CASE("Metamorphosis is never the active button", "[replay][stancebar]") {
    Book book(9);
    book.initialSpells({kMetamorphosis});
    book.aura(0, kMetamorphosis);
    CHECK_FALSE(book.s.handler().formSpellInEffect(kMetamorphosis));
}

TEST_CASE("before WotLK the stance bar is the class's list", "[replay][stancebar]") {
    Session s("tbc");
    CHECK(s.handler().stanceBarSpells().empty());
}

// The player as its own target or focus showed no auras (playtest P37).
TEST_CASE("the player's auras are the target's where the player is the target", "[replay][auras]") {
    Book book(2);
    std::vector<std::string> units;
    book.s.handler().setAddonEventCallback([&](const std::string& event, const std::vector<std::string>& args) {
        if (event == "UNIT_AURA" && !args.empty()) units.push_back(args[0]);
    });
    book.s.handler().setTarget(Session::kPlayerGuid);
    book.s.handler().setFocus(Session::kPlayerGuid);
    units.clear();
    book.aura(0, kDevotion10);
    REQUIRE(book.s.handler().getTargetAuras().size() == 1);
    CHECK(book.s.handler().getTargetAuras()[0].spellId == kDevotion10);
    // By guid too: focus, mouseover, targettarget.
    const auto* byGuid = book.s.handler().getUnitAuras(Session::kPlayerGuid);
    REQUIRE(byGuid != nullptr);
    CHECK((*byGuid)[0].spellId == kDevotion10);
    // UNIT_AURA for every token of the unit (0x006143f0).
    CHECK(units == std::vector<std::string>{"player", "target", "focus"});
    book.s.handler().clearTarget();
    CHECK(book.s.handler().getTargetAuras().empty());
}
