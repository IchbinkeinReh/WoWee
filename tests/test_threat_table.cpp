#include <catch_amalgamated.hpp>

#include "game/threat_table.hpp"

using wowee::game::scaledThreatPercent;
using wowee::game::threatStatusColor;
using wowee::game::ThreatTable;
using wowee::game::worstThreatStatus;

namespace {
constexpr uint64_t kMe = 1, kTank = 2, kOther = 3;
using Guids = std::vector<uint64_t>;
using Floaters = std::vector<std::pair<uint8_t, uint8_t>>;
using Sounds = std::vector<uint32_t>;
}  // namespace

TEST_CASE("the victim is whom the server names, not who has the most", "[threat]") {
    ThreatTable t;
    auto c = t.update(kTank, {{kTank, 1000}, {kMe, 500}}, kMe);
    CHECK(c.list);
    CHECK(c.joined == Guids{kTank, kMe});
    CHECK(t.standing(kTank).status == 4);
    CHECK(t.standing(kMe).status == 1);
    CHECK(t.standing(kMe).percent == 50);
    CHECK(c.situations == Guids{kTank});

    // Past the victim: high threat, the victim no longer safe.
    c = t.update(std::nullopt, {{kMe, 1050}}, kMe);
    CHECK(t.standing(kMe).status == 2);
    CHECK(t.standing(kMe).percent == 105);
    CHECK_FALSE(t.standing(kMe).victim);
    CHECK(t.standing(kTank).status == 3);
    CHECK(t.standing(kTank).victim);
    CHECK(c.situations == Guids{kMe, kTank});
    CHECK(c.sounds == Sounds{ThreatTable::kSoundReached});
    CHECK(c.floaters == Floaters{{uint8_t{1}, uint8_t{2}}});

    // Named: the sound of it, and the other is reckoned against the player.
    c = t.update(kMe, {{kMe, 1200}, {kTank, 1000}}, kMe);
    CHECK(t.standing(kMe).status == 4);
    CHECK(t.standing(kMe).victim);
    CHECK(t.standing(kTank).status == 1);
    CHECK(t.standing(kTank).percent == 83);
    CHECK(c.sounds == Sounds{ThreatTable::kSoundVictim});
    CHECK(c.floaters == Floaters{{uint8_t{2}, uint8_t{4}}});

    // And back.
    c = t.update(kTank, {{kTank, 2000}}, kMe);
    CHECK(t.standing(kMe).status == 1);
    CHECK(t.standing(kMe).percent == 60);
    CHECK(c.floaters == Floaters{{uint8_t{4}, uint8_t{1}}});
    CHECK(c.sounds.empty());
}

TEST_CASE("one new to a list gets no floater and no sound of being its victim", "[threat]") {
    ThreatTable t;
    auto c = t.update(kMe, {{kMe, 10}}, kMe);
    CHECK(t.standing(kMe).status == 4);
    CHECK(c.floaters.empty());
    CHECK(c.sounds.empty());
    c = t.update(kOther, {{kOther, 10}, {kMe, 10}}, kMe);
    CHECK(c.floaters == Floaters{{uint8_t{4}, uint8_t{2}}});
    CHECK(c.sounds.empty());   // of 1 to 2 alone
}

TEST_CASE("an update leaves who is not in it, and reckons all by the victim's", "[threat]") {
    ThreatTable t;
    (void)t.update(kTank, {{kTank, 1000}, {kMe, 500}, {kOther, 100}}, kMe);
    auto c = t.update(std::nullopt, {{kOther, 200}}, kMe);
    CHECK(t.entries().size() == 3);
    CHECK(c.situations.empty());
    CHECK(c.list);
    CHECK(t.standing(kOther).percent == 20);
    (void)t.update(std::nullopt, {{kTank, 2000}}, kMe);
    CHECK(t.standing(kMe).percent == 25);
    CHECK(t.standing(kOther).percent == 10);
    // A list without a victim tells its members apart from strangers alone.
    ThreatTable u;
    c = u.update(std::nullopt, {{kMe, 10}}, kMe);
    CHECK(c.situations.empty());
    CHECK(u.standing(kMe).status == 1);
    CHECK_FALSE(u.standing(kMe).known);
    CHECK(u.standing(kMe).threat == 0);
    CHECK(u.standing(kTank).status == 0);
}

TEST_CASE("the share stops at 250, and at none or a victim without threat", "[threat]") {
    ThreatTable t;
    (void)t.update(kTank, {{kTank, 10}, {kMe, 1000}, {kOther, -5}}, kMe);
    CHECK(t.standing(kMe).percent == 250);
    CHECK(t.standing(kOther).percent == 0);
    CHECK(t.standing(kOther).status == 1);
    CHECK(t.standing(kTank).percent == 255);
    (void)t.update(kTank, {{kTank, 0}}, kMe);
    CHECK(t.standing(kOther).percent == 100);
    CHECK(t.standing(kOther).status == 2);
}

TEST_CASE("one taken off the list, and the list cleared", "[threat]") {
    ThreatTable t;
    (void)t.update(kTank, {{kTank, 100}, {kMe, 100}, {kOther, 50}}, kMe);
    REQUIRE(t.standing(kTank).status == 3);
    auto c = t.remove(kMe, kMe);
    CHECK(c.situations == Guids{kMe, kTank});
    CHECK(c.list);
    CHECK(t.standing(kTank).status == 4);
    CHECK(t.standing(kMe).status == 0);
    c = t.remove(kMe, kMe);
    CHECK(c.situations.empty());
    CHECK_FALSE(c.list);
    c = t.remove(kTank, kMe);
    CHECK(c.situations == Guids{kTank});
    CHECK(t.victim() == 0);
    c = t.clear();
    CHECK(c.situations == Guids{kOther});
    CHECK(c.list);
    CHECK_FALSE(t.clear().list);
}

TEST_CASE("the percentage towards the victim is of 110, or of 130 from afar", "[threat]") {
    ThreatTable t;
    (void)t.update(kTank, {{kTank, 100}, {kMe, 104}}, kMe);
    CHECK(scaledThreatPercent(t.standing(kMe), true) == Catch::Approx(94.545).epsilon(0.001));
    CHECK(scaledThreatPercent(t.standing(kMe), false) == Catch::Approx(80.0).epsilon(0.001));
    CHECK(scaledThreatPercent(t.standing(kTank), false) == 100.0f);
    CHECK(scaledThreatPercent(t.standing(kOther), true) == 0.0f);
}

TEST_CASE("one's status over several lists", "[threat]") {
    const auto over = [](std::initializer_list<uint8_t> s) {
        uint8_t w = 0;
        for (const uint8_t v : s) w = worstThreatStatus(w, v);
        return w;
    };
    CHECK(over({}) == 0);
    CHECK(over({1, 2}) == 2);
    CHECK(over({2, 1}) == 2);
    CHECK(over({1, 4, 2}) == 4);
    CHECK(over({4, 3}) == 3);
    CHECK(over({3, 4}) == 3);
}

TEST_CASE("the threat colours", "[threat]") {
    CHECK(threatStatusColor(0) == 0xffffffffu);
    CHECK(threatStatusColor(1) == 0xffb0b0b0u);
    CHECK(threatStatusColor(2) == 0xffffff77u);
    CHECK(threatStatusColor(3) == 0xffff9900u);
    CHECK(threatStatusColor(4) == 0xffff0000u);
    CHECK(threatStatusColor(5) == 0xffb0b0b0u);
}
