// The tabard designer as CGTabardModelFrame keeps its design: the counts it
// cycles through (0x00a14908, 0x00599470), the guild's design or a random one
// to start from (0x00599670), what refuses a save (0x006db320), what the
// answer says (0x006d9230), and the files the frame names (0x004e81c0,
// 0x004e8210).
#include <catch_amalgamated.hpp>

#include "game/tabard_design.hpp"

using namespace wowee::game;
namespace tb = wowee::game::tabard;

TEST_CASE("CycleVariation wraps each of the five round its count", "[tabard]") {
    tb::Design d;
    tb::cycle(d, 0, -1);
    CHECK(d.values[0] == 169);
    tb::cycle(d, 0, 1);
    CHECK(d.values[0] == 0);
    tb::cycle(d, 2, -1);
    CHECK(d.values[2] == 5);
    tb::cycle(d, 4, 3);
    CHECK(d.values[4] == 3);
    // A step as large as the count moves nothing; nor does a sixth index.
    tb::cycle(d, 1, 17);
    CHECK(d.values[1] == 0);
    tb::cycle(d, 5, 1);
    CHECK(d == tb::Design{{0, 0, 5, 0, 3}});
}

TEST_CASE("The design starts from the guild's, or from chance", "[tabard]") {
    const GuildEmblem guild{.emblemStyle = 12, .emblemColor = 3, .borderStyle = 2, .borderColor = 7,
                            .backgroundColor = 40};
    int asked = 0;
    auto pick = [&](int32_t n) { ++asked; return n - 1; };
    CHECK(tb::initial(guild, pick) == tb::Design{{12, 3, 2, 7, 40}});
    CHECK(asked == 0);
    GuildEmblem unknown = guild;
    unknown.borderColor = 0xFFFFFFFFu;
    CHECK(tb::initial(unknown, pick) == tb::Design{{169, 16, 5, 16, 50}});
    CHECK(tb::initial(std::nullopt, pick) == tb::Design{{169, 16, 5, 16, 50}});
    CHECK(asked == 10);
}

TEST_CASE("A save is refused in 0x006db320's order", "[tabard]") {
    const GuildEmblem guild{.emblemStyle = 1, .emblemColor = 1, .borderStyle = 1, .borderColor = 1,
                            .backgroundColor = 1};
    tb::SaveCheck c{.guild = guild, .guildMaster = true, .money = 200000};
    const tb::Design fresh{{2, 1, 1, 1, 1}};
    CHECK_FALSE(tb::saveRefusal(fresh, c));
    CHECK(tb::saveRefusal(tb::Design{{170, 1, 1, 1, 1}}, c) == tb::kInvalidColors);
    CHECK(tb::saveRefusal(tb::Design{{1, 1, 6, 1, 1}}, c) == tb::kInvalidColors);
    CHECK(tb::saveRefusal(tb::Design{{-1, 1, 1, 1, 1}}, c) == tb::kInvalidColors);
    CHECK(tb::saveRefusal(tb::Design::of(guild), c) == tb::kSame);
    c.guildMaster = false;
    CHECK(tb::saveRefusal(fresh, c) == tb::kNotGuildMaster);
    c.guildMaster = true;
    c.money = 99999;
    CHECK(tb::saveRefusal(fresh, c) == tb::kNotEnoughMoney);
    c.guild.reset();
    CHECK(tb::saveRefusal(fresh, c) == tb::kNoGuild);
}

TEST_CASE("The save's answer and the frame's files", "[tabard]") {
    CHECK(tb::saveResultError(0) == tb::kSaved);
    CHECK(tb::saveResultError(4) == tb::kCannotAfford);
    CHECK_FALSE(tb::saveResultError(5));
    CHECK(tb::knownSaveResult(5));
    CHECK_FALSE(tb::knownSaveResult(6));
    CHECK(tb::backgroundFile(7, true) == "Textures\\GuildEmblems\\Background_07_TU_U");
    CHECK(tb::emblemFile(123, 4, false) == "Textures\\GuildEmblems\\Emblem_123_04_TL_U");
}
