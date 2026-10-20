// The macro conditional language as the client parses it (UIMacroOptions.cpp
// 0x005f0df0, 0x005f0bb0, 0x005f0040, 0x005ef5c0).

#include <catch_amalgamated.hpp>

#include "game/macro_options.hpp"

#include <map>

using namespace wowee::game::macro_options;

namespace {

class FakeState : public State {
public:
    std::map<std::string, uint64_t> units = {{"player", 1}, {"focus", 2}, {"pet", 3}};
    uint64_t target = 0;
    bool combat = false, shift = false, ctrl = false, isOutdoors = true, grouped = false, raid = false;
    int form = 0, page = 1, bonus = 0, spec = 1;
    std::map<uint64_t, bool> alive = {{1, true}, {2, true}, {3, true}};
    std::map<uint64_t, bool> enemy;
    std::string button = "LeftButton";
    std::vector<std::string> unknown;
    std::string petName = "Fluffy", petFamily = "Wolf";
    bool hasPetOut = true;
    std::string channel;
    std::string cursor;

    uint64_t unitGuid(const std::string& unit) override {
        if (unit.empty()) return target;
        const auto it = units.find(unit);
        return it == units.end() ? 0 : it->second;
    }
    bool inCombat() override { return combat; }
    bool exists(uint64_t unit) override { return alive.count(unit) != 0; }
    bool canAssist(uint64_t unit) override { return exists(unit) && !enemy[unit]; }
    bool canAttack(uint64_t unit) override { return exists(unit) && enemy[unit]; }
    bool inPartyAsMember(uint64_t unit) override { return grouped && unit == 2; }
    bool inRaidAsMember(uint64_t unit) override { return raid && unit == 2; }
    bool dead(uint64_t unit) override { return exists(unit) && !alive[unit]; }
    bool flyable(uint64_t) override { return false; }
    bool outdoors() override { return isOutdoors; }
    bool hasPlayer() override { return true; }
    bool swimming() override { return false; }
    bool flying() override { return false; }
    bool mounted() override { return false; }
    bool stealthed() override { return false; }
    bool inParty() override { return grouped; }
    bool inRaid() override { return raid; }
    int shapeshiftForm() override { return form; }
    bool pet(std::string& name, std::string& family) override {
        name = petName;
        family = petFamily;
        return hasPetOut;
    }
    bool modifier(const std::string& name) override {
        if (name.empty()) return shift || ctrl;
        if (name == "shift") return shift;
        if (name == "ctrl") return ctrl;
        return false;
    }
    std::string clickButton() override { return button; }
    std::string macroButton() override { return {}; }
    std::string mouseButtonName(int n) override { return n == 2 ? "RightButton" : n == 1 ? "LeftButton" : ""; }
    int actionBarPage() override { return page; }
    int bonusBarOffset() override { return bonus; }
    bool equipped(const std::string& type) override { return type == "Shields"; }
    bool channeling(std::string& spell) override {
        spell = channel;
        return !channel.empty();
    }
    int activeSpec() override { return spec; }
    bool vehicleUi(uint64_t) override { return false; }
    bool playerVehicleUi() override { return false; }
    std::string cursorType() override { return cursor; }
    void unknownOption(const std::string& name) override { unknown.push_back(name); }
};

struct Answer {
    bool applies = false;
    std::string result;
    std::string target;
};
Answer run(const std::string& options, FakeState& st) {
    Answer a;
    a.applies = parse(options, st, a.result, a.target);
    return a;
}

}  // namespace

TEST_CASE("Clauses are tried in order and the first that holds answers", "[macro]") {
    FakeState st;
    CHECK(run("[combat] Charge; Heroic Strike", st).result == "Heroic Strike");
    st.combat = true;
    CHECK(run("[combat] Charge; Heroic Strike", st).result == "Charge");
    // A clause with no group always applies, and its text is trimmed.
    CHECK(run("  Fireball  ", st).result == "Fireball");
    // Nothing holds: no answer at all.
    st.combat = false;
    CHECK_FALSE(run("[combat] Charge", st).applies);
}

TEST_CASE("\"no\" negates and arguments compare with numbers", "[macro]") {
    FakeState st;
    CHECK(run("[nocombat] Eat", st).result == "Eat");
    CHECK(run("[combat:0] Eat; Fight", st).result == "Eat");
    st.combat = true;
    CHECK(run("[combat:0] Eat; Fight", st).result == "Fight");
    // "no" is matched with its case (strncmp, 0x0076e760): "NOcombat" is no
    // option at all, which is reported and holds.
    CHECK(run("[NOcombat] Eat; Fight", st).result == "Eat");
    CHECK(st.unknown == std::vector<std::string>{"NOcombat"});
}

TEST_CASE("Groups in a clause are tried until one passes, and it names the unit", "[macro]") {
    FakeState st;
    st.alive[2] = true;
    st.enemy[2] = false;
    const Answer a = run("[@focus,help][harm] Heal; Smite", st);
    CHECK(a.result == "Heal");
    CHECK(a.target == "focus");
    // target= is the other spelling; the current target when none is named.
    st.target = 2;
    const Answer b = run("[target=player,dead] Rez; [help] Heal", st);
    CHECK(b.result == "Heal");
    CHECK(b.target.empty());
    // A group of only a unit always passes.
    CHECK(run("[@pet] Mend", st).target == "pet");
}

TEST_CASE("Modifiers, buttons, bars, forms and the rest read their arguments", "[macro]") {
    FakeState st;
    CHECK(run("[mod] A; B", st).result == "B");
    st.shift = true;
    CHECK(run("[mod:ctrl/shift] A; B", st).result == "A");
    CHECK(run("[button:2] Right; Left", st).result == "Left");
    st.button = "RightButton";
    CHECK(run("[btn:2] Right; Left", st).result == "Right");
    CHECK(run("[bar:2] Two; One", st).result == "One");
    st.page = 2;
    CHECK(run("[actionbar:1/2] Two; One", st).result == "Two");
    CHECK(run("[form] Shifted; Caster", st).result == "Caster");
    st.form = 3;
    CHECK(run("[stance:1/3] Bear; Caster", st).result == "Bear");
    CHECK(run("[pet:Wolf] Bite; Growl", st).result == "Bite");
    CHECK(run("[pet:Cat] Claw; Growl", st).result == "Growl");
    CHECK(run("[equipped:Shields] Bash; Strike", st).result == "Bash");
    CHECK(run("[group] Party; Solo", st).result == "Solo");
    st.grouped = true;
    CHECK(run("[group:raid] Raid; [group:party] Party", st).result == "Party");
    CHECK(run("[spec:2] Two; One", st).result == "One");
    CHECK(run("[cursor] Drop; Pick", st).result == "Pick");
    st.cursor = "item";
    CHECK(run("[cursor:ITEM] Drop; Pick", st).result == "Drop");
    CHECK(run("[outdoors] Mount; Walk", st).result == "Mount");
    CHECK(run("[indoors] Walk; Mount", st).result == "Mount");
}

TEST_CASE("An unknown option is reported and holds", "[macro]") {
    FakeState st;
    CHECK(run("[bogus] A; B", st).result == "A");
    CHECK(st.unknown == std::vector<std::string>{"bogus"});
    // Empty groups and empty items hold too.
    CHECK(run("[] A", st).result == "A");
    CHECK(run("[,] A", st).result == "A");
}
