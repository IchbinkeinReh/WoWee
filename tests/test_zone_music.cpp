// The world's music as the client picks it (audio/zone_music.hpp): the
// slots and the one that plays (0x004c88b0), the silence between tracks
// (0x004c87d0, 0x004c8ab0), the intros' delay (0x004c8830, 0x004c9080), a
// new ZoneMusic row ending the silence (0x004c8d80) and a kit's file
// (0x004cbac0).
#include <catch_amalgamated.hpp>

#include "audio/zone_ambience.hpp"
#include "audio/zone_music.hpp"

#include <unordered_map>

namespace zm = wowee::audio::zone_music;
namespace za = wowee::audio::zone_ambience;

namespace {

// Zone-Forest (ZoneMusic 1): 2523 by day and night, 180 to 300 seconds.
constexpr zm::ZoneMusicRow kForest{{180000, 180000}, {300000, 300000}, {2523, 2523}};
// Zone-EvilForest (2): 2524 by day, 2534 by night.
constexpr zm::ZoneMusicRow kEvilForest{{180000, 180000}, {300000, 300000}, {2524, 2534}};
// Valley of Heroes (ZoneIntroMusicTable 61): 2541, an hour.
constexpr zm::IntroRow kValleyOfHeroes{2541, 60};

zm::Context at(uint64_t now) {
    zm::Context c;
    c.now = now;
    return c;
}

}  // namespace

TEST_CASE("the highest slot with music for the time plays", "[audio][music]") {
    zm::Slots slots{};
    const zm::Channel quiet;
    CHECK(zm::pick(slots, quiet, at(0)) == zm::Pick{});

    zm::fillZoneMusic(slots[zm::kSlotPlace], &kEvilForest);
    CHECK(zm::pick(slots, quiet, at(0)) == zm::Pick{zm::kSlotPlace, 2524, 180000, 300000});
    auto night = at(0);
    night.time = zm::timeIndex(false);
    CHECK(zm::pick(slots, quiet, night).sound == 2534);

    // The world state's over the place's, the server's over both, the
    // screen effect's over all.
    zm::fillZoneMusic(slots[zm::kSlotWorldState], &kForest);
    CHECK(zm::pick(slots, quiet, at(0)).slot == zm::kSlotWorldState);
    zm::fillServer(slots[zm::kSlotServer], 11803);
    CHECK(zm::pick(slots, quiet, at(0)) == zm::Pick{zm::kSlotServer, 11803, 0, 0});
    zm::fillZoneMusic(slots[zm::kSlotScreenEffect], &kForest);
    CHECK(zm::pick(slots, quiet, at(0)) == zm::Pick{zm::kSlotScreenEffect, 2523, 180000, 300000});

    // A row not there empties the slot.
    zm::fillZoneMusic(slots[zm::kSlotScreenEffect], nullptr);
    CHECK(slots[zm::kSlotScreenEffect] == zm::SlotMusic{});

    // Music off picks nothing.
    auto off = at(0);
    off.enabled = false;
    CHECK(zm::pick(slots, quiet, off).sound == 0);
}

TEST_CASE("a sound of 0 holds the slots below back", "[audio][music]") {
    zm::Slots slots{};
    zm::fillZoneMusic(slots[zm::kSlotPlace], &kForest);
    const zm::ZoneMusicRow nightless{{0, 0}, {0, 0}, {7341, 0}};
    zm::fillZoneMusic(slots[zm::kSlotWorldState], &nightless);
    auto night = at(0);
    night.time = 1;
    CHECK(zm::pick(slots, {}, night) == zm::Pick{zm::kSlotWorldState, 0, 0, 0});
}

TEST_CASE("slots 0 to 2 wait out the silence, the rest do not", "[audio][music]") {
    zm::Slots slots{};
    zm::fillZoneMusic(slots[zm::kSlotPlace], &kForest);
    auto c = at(1000);
    c.silenceUntil = 5000;
    CHECK(zm::pick(slots, {}, c) == zm::Pick{zm::kSilenceSlot, zm::kSilence, 0, 0});
    // Nothing is started for the silence.
    CHECK_FALSE(zm::shouldStart(zm::pick(slots, {}, c), {}, false));

    // Sound_ZoneMusicNoDelay: no silence at all.
    c.noDelay = true;
    CHECK(zm::pick(slots, {}, c).sound == 2523);
    c.noDelay = false;

    // Over at its end.
    c.now = 5000;
    CHECK(zm::pick(slots, {}, c).sound == 2523);

    // The server's music in slot 3 plays through it.
    c.now = 1000;
    zm::fillServer(slots[zm::kSlotServer], 11803);
    CHECK(zm::pick(slots, {}, c).sound == 11803);
}

TEST_CASE("the silence after a zone track", "[audio][music]") {
    // The slots still pick the sound: between the least and the most.
    CHECK(zm::silenceAfter(2523, 180000, 300000, 2523, 1000, 0) == 181000);
    CHECK(zm::silenceAfter(2523, 180000, 300000, 2523, 1000, 0x80000000u) == 241000);
    CHECK(zm::silenceAfter(2523, 180000, 300000, 2523, 1000, 0xFFFFFFFFu) < 301000);
    // A least not under the most: the most.
    CHECK(zm::silenceAfter(2523, 1800000, 300000, 2523, 1000, 12345) == 301000);
    // Something else picked: none.
    CHECK(zm::silenceAfter(2523, 180000, 300000, 2524, 1000, 12345) == 0);

    CHECK(zm::onEnd(zm::kSlotPlace) == zm::OnEnd::Silence);
    CHECK(zm::onEnd(zm::kSlotWorldState) == zm::OnEnd::Silence);
    CHECK(zm::onEnd(zm::kSlotServer) == zm::OnEnd::EmptyServerSlot);
    CHECK(zm::onEnd(zm::kSlotPlaceIntro) == zm::OnEnd::IntroDelay);
    CHECK(zm::onEnd(zm::kSlotScreenEffect) == zm::OnEnd::None);
}

TEST_CASE("the channel is given what is picked unless it plays it", "[audio][music]") {
    const zm::Pick forest{zm::kSlotPlace, 2523, 180000, 300000};
    CHECK(zm::shouldStart(forest, {}, false));
    CHECK_FALSE(zm::shouldStart(forest, {true, 2523, zm::kSlotPlace}, false));
    // The same sound ended: started again.
    CHECK(zm::shouldStart(forest, {false, 2523, zm::kSlotPlace}, false));
    // Another plays: crossed over four seconds; out of quiet a hundredth.
    CHECK(zm::shouldStart(forest, {true, 2524, zm::kSlotPlace}, false));
    CHECK(zm::fadeInSeconds({true, 2524, zm::kSlotPlace}) == 4.0f);
    CHECK(zm::fadeInSeconds({}) == Catch::Approx(0.01f));
    // Not while the script's PlayMusic plays.
    CHECK_FALSE(zm::shouldStart(forest, {}, true));
}

TEST_CASE("an intro plays once in its delay and to its end", "[audio][music]") {
    zm::Slots slots{};
    zm::fillZoneMusic(slots[zm::kSlotPlace], &kForest);
    zm::fillIntro(slots[zm::kSlotPlaceIntro], &kValleyOfHeroes);
    CHECK(slots[zm::kSlotPlaceIntro].silenceMin[0] == 60);

    std::unordered_map<int32_t, uint64_t> due;
    auto c = at(1000);
    c.introDue = &due;
    const zm::Channel zone{true, 2523, zm::kSlotPlace};
    CHECK(zm::pick(slots, zone, c) == zm::Pick{zm::kSlotPlaceIntro, 2541, 60, 0});

    // Playing, it goes on whatever the slots say.
    const zm::Channel intro{true, 2541, zm::kSlotPlaceIntro};
    zm::fillIntro(slots[zm::kSlotPlaceIntro], nullptr);
    CHECK(zm::pick(slots, intro, c) == zm::Pick{zm::kSlotPlaceIntro, 2541, 0, 0});

    // Ended: not straight after itself, and not again for its delay.
    zm::fillIntro(slots[zm::kSlotPlaceIntro], &kValleyOfHeroes);
    due[2541] = zm::introDueAfter(60, 1000);
    CHECK(due[2541] == 1000 + 3600000);
    const zm::Channel introEnded{false, 2541, zm::kSlotPlaceIntro};
    CHECK(zm::pick(slots, introEnded, c).sound == 2523);
    CHECK(zm::pick(slots, zone, c).sound == 2523);
    c.now = 1000 + 3600000;
    CHECK(zm::pick(slots, zone, c).sound == 2541);
    // ...but still not straight after another intro.
    CHECK(zm::pick(slots, introEnded, c).sound == 2523);
}

TEST_CASE("a new ZoneMusic row ends the silence unless it is waited out", "[audio][music]") {
    const zm::Channel quiet;
    const zm::Channel forest{true, 2523, zm::kSlotPlace};
    const zm::Pick evil{zm::kSlotPlace, 2524, 180000, 300000};
    // Another sound than what plays: the next starts at once.
    CHECK(zm::zoneMusicEndsSilence(zm::kSlotPlace, 2, true, evil, forest));
    CHECK_FALSE(zm::zoneMusicEndsSilence(zm::kSlotPlace, 1, true, {zm::kSlotPlace, 2523, 0, 0}, forest));
    // Slots 0..2 in their silence go on waiting; the screen effect's does not.
    const zm::Pick silence{zm::kSilenceSlot, zm::kSilence, 0, 0};
    CHECK_FALSE(zm::zoneMusicEndsSilence(zm::kSlotPlace, 2, true, silence, quiet));
    CHECK(zm::zoneMusicEndsSilence(zm::kSlotScreenEffect, 2, true, silence, quiet));
    // A row not there: only for an id above 0.
    CHECK(zm::zoneMusicEndsSilence(zm::kSlotPlace, 9999, false, silence, quiet));
    CHECK_FALSE(zm::zoneMusicEndsSilence(zm::kSlotPlace, 0, false, silence, quiet));
    CHECK_FALSE(zm::zoneMusicEndsSilence(zm::kSlotWorldState, -1, false, silence, quiet));
}

TEST_CASE("a kit's file by its weights, never the last twice", "[audio][music]") {
    CHECK_FALSE(zm::pickFile({}, std::nullopt, 0));
    CHECK(zm::pickFile({0}, 0, 0) == 0u);
    const std::vector<uint32_t> three{1, 1, 1};
    CHECK(zm::pickFile(three, std::nullopt, 0) == 0u);
    CHECK(zm::pickFile(three, std::nullopt, 0xFFFFFFFFu) == 2u);
    // The last played is left out.
    CHECK(zm::pickFile(three, 0, 0) == 1u);
    CHECK(zm::pickFile(three, 0, 0xFFFFFFFFu) == 2u);
    CHECK(zm::pickFile(three, 1, 0x80000000u) == 2u);
    // Weighted.
    CHECK(zm::pickFile({1, 3}, std::nullopt, 0x40000000u) == 1u);
    // Only the last has weight: it again.
    CHECK(zm::pickFile({0, 5}, 1, 0) == 1u);
    CHECK_FALSE(zm::pickFile({0, 0}, std::nullopt, 0));
}

TEST_CASE("the place's music takes the ambience's chain", "[audio][music]") {
    // A building's group's row, then the building's, then - outside - the
    // area's and the zone's (0x0078e9a0).
    CHECK(za::placeAmbience(13, 0, 0, 0, false) == 13);
    CHECK(za::placeAmbience(13, 2, 0, 0, false) == 2);
    CHECK(za::placeAmbience(13, 2, 204, 0, false) == 204);
    CHECK(za::placeAmbience(13, 2, 204, 156, true) == 156);
    CHECK(za::placeAmbience(13, 2, 0, 0, true) == 0);
}

TEST_CASE("a world state's row gives its intro and music", "[audio][music]") {
    // Orgrimmar's row for world state 2019 = 1: intro 62, music 14.
    std::vector<za::WorldStateSound> rows{{2019, 1, 1637, 0, 36, 62, 14}};
    za::Place place;
    place.zone = 1637;
    const auto value = [](uint32_t state) -> uint32_t { return state == 2019 ? 1 : 0; };
    const auto row = za::worldStateRow(rows, place, value);
    REQUIRE(row);
    CHECK(row->intro == 62);
    CHECK(row->music == 14);
    CHECK(za::worldStateAmbience(rows, place, value) == 36u);
    CHECK_FALSE(za::worldStateRow(rows, place, [](uint32_t) { return 0u; }));
}
