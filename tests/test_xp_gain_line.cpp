// The experience line SMSG_LOG_XPGAIN prints, as 0x0050b300 and 0x0050b1d0
// choose it: plain, group bonus, raid penalty, rested and tired forms, and the
// Recruit-a-Friend quest form.
#include <catch_amalgamated.hpp>

#include <string>

#include "game/xp_gain_line.hpp"

namespace xl = wowee::game::xp_gain_line;

static xl::ExhaustionRow rows(int id) {
    switch (id) {
        case 1: return {true, 2.0f, "Rested"};
        case 2: return {true, 1.0f, "Normal"};
        case 4: return {true, 0.5f, "Tired"};
        case 6: return {true, 3.0f, "Recruit-A-Friend"};
        default: return {};
    }
}

static xl::Gain kill(int xp, int restless, float rate, int restState) {
    xl::Gain g;
    g.haveVictim = g.victimIsUnit = true;
    g.victimName = "Kobold";
    g.xp = xp;
    g.restlessXp = restless;
    g.groupRate = rate;
    g.restState = restState;
    return g;
}

TEST_CASE("A kill with no rest or group bonus names the victim", "[xp_gain_line]") {
    const auto l = xl::choose(kill(100, 100, 1.0f, 2), rows);
    CHECK(std::string(l.key) == "COMBATLOG_XPGAIN_FIRSTPERSON");
    CHECK(l.args == std::vector<std::string>{"Kobold", "100"});
}

TEST_CASE("The group rate turns into a bonus or a penalty", "[xp_gain_line]") {
    auto l = xl::choose(kill(125, 125, 1.25f, 2), rows);
    CHECK(std::string(l.key) == "COMBATLOG_XPGAIN_FIRSTPERSON_GROUP");
    CHECK(l.args == std::vector<std::string>{"Kobold", "125", "25"});
    l = xl::choose(kill(50, 50, 0.5f, 2), rows);
    CHECK(std::string(l.key) == "COMBATLOG_XPGAIN_FIRSTPERSON_RAID");
    CHECK(l.args == std::vector<std::string>{"Kobold", "50", "50"});
}

TEST_CASE("A rested kill gives the bonus and the rest state's name", "[xp_gain_line]") {
    auto l = xl::choose(kill(200, 100, 1.0f, 1), rows);
    CHECK(std::string(l.key) == "COMBATLOG_XPGAIN_EXHAUSTION1");
    CHECK(l.args == std::vector<std::string>{"Kobold", "200", "+100", "Rested"});
    l = xl::choose(kill(250, 125, 1.25f, 1), rows);
    CHECK(std::string(l.key) == "COMBATLOG_XPGAIN_EXHAUSTION1_GROUP");
    CHECK(l.args == std::vector<std::string>{"Kobold", "250", "+125", "Rested", "25"});
    l = xl::choose(kill(50, 100, 1.0f, 4), rows);
    CHECK(std::string(l.key) == "COMBATLOG_XPGAIN_EXHAUSTION4");
    CHECK(l.args[2] == "-50");
}

TEST_CASE("Recruit-a-Friend counts as rest state 6", "[xp_gain_line]") {
    auto g = kill(300, 100, 1.0f, 2);
    g.referAFriend = true;
    const auto l = xl::choose(g, rows);
    CHECK(std::string(l.key) == "COMBATLOG_XPGAIN_EXHAUSTION1");
    CHECK(l.args[3] == "Recruit-A-Friend");
}

TEST_CASE("Anything but a kill takes the unnamed forms", "[xp_gain_line]") {
    xl::Gain g;
    g.xp = 450;
    g.type = 1;
    auto l = xl::choose(g, rows);
    CHECK(std::string(l.key) == "COMBATLOG_XPGAIN_FIRSTPERSON_UNNAMED");
    CHECK(l.args == std::vector<std::string>{"450"});
    g.referAFriend = true;
    l = xl::choose(g, rows);
    CHECK(std::string(l.key) == "COMBATLOG_XPGAIN_QUEST");
    CHECK(l.args == std::vector<std::string>{"450", "+300", "Recruit-A-Friend", "300"});
}
