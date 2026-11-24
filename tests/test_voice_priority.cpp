// Which sounds are heard past the real voices (audio/voice_priority.hpp).
#include <catch_amalgamated.hpp>

#include "audio/npc_line_counter.hpp"
#include "audio/finite_audio.hpp"
#include "audio/sound_key.hpp"
#include "audio/sound_names.hpp"
#include "audio/sound_slots.hpp"
#include "audio/voice_priority.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace vp = wowee::audio::voice_priority;

TEST_CASE("Sound_NumChannels is held to the client's range", "[audio]") {
    CHECK(vp::clampRealVoices(vp::kDefaultRealVoices) == 32);
    CHECK(vp::clampRealVoices(4) == 12);
    CHECK(vp::clampRealVoices(500) == 128);
}

TEST_CASE("the active mover's plays are heard over a crowd's", "[audio]") {
    // Three voices for four sounds: the quiet one with the mover's priority
    // keeps its voice, the quietest of the rest loses it.
    const std::vector<vp::Voice> voices = {
        {vp::kDefault, 0.9f}, {vp::kDefault, 0.2f}, {vp::kActiveMover, 0.1f}, {vp::kDefault, 0.5f}};
    const auto heard = vp::heard(voices, 3);
    CHECK(heard[0]);
    CHECK_FALSE(heard[1]);
    CHECK(heard[2]);
    CHECK(heard[3]);
}

TEST_CASE("among equals the first to start keeps its voice", "[audio]") {
    const std::vector<vp::Voice> voices = {{vp::kDefault, 0.5f}, {vp::kDefault, 0.5f}};
    const auto heard = vp::heard(voices, 1);
    CHECK(heard[0]);
    CHECK_FALSE(heard[1]);
    CHECK(vp::heard(voices, 0) == std::vector<bool>{false, false});
}

TEST_CASE("a 3D sound's reach by distance", "[audio]") {
    CHECK(vp::distanceGain(2.0f, 5.0f, 40.0f) == 1.0f);
    CHECK(vp::distanceGain(10.0f, 5.0f, 40.0f) == 0.5f);
    CHECK(vp::distanceGain(50.0f, 5.0f, 40.0f) == 0.0f);
}

TEST_CASE("a sound starts heard while a voice is free", "[voice_priority]") {
    CHECK(vp::startsHeard(0, 32));
    CHECK(vp::startsHeard(31, 32));
    CHECK_FALSE(vp::startsHeard(32, 32));
    CHECK_FALSE(vp::startsHeard(0, 0));
}

TEST_CASE("a handle's sound is found wherever the list has it", "[voice_priority]") {
    // Sounds come and go as the engine takes them away: the last in the place
    // of the one gone.
    std::vector<uint32_t> list;
    wowee::audio::SoundSlots slots;
    const auto add = [&](uint32_t id) {
        list.push_back(id);
        slots.added(id, list.size() - 1);
    };
    const auto remove = [&](size_t at) {
        slots.removed(list[at], at, list.back(), list.size() - 1);
        list[at] = list.back();
        list.pop_back();
    };
    for (uint32_t id = 1; id <= 40; ++id) add(id % 4 == 0 ? 0 : id);  // some without a handle
    for (const size_t at : {size_t{39}, size_t{0}, size_t{5}, size_t{36}, size_t{11}, size_t{0}}) remove(at);
    add(100);
    size_t known = 0;
    for (uint32_t id = 1; id <= 100; ++id) {
        const auto slot = slots.find(id);
        const auto at = std::find(list.begin(), list.end(), id);
        REQUIRE(slot.has_value() == (at != list.end()));
        if (!slot) continue;
        ++known;
        CHECK(list[*slot] == id);
    }
    CHECK(known > 20);
    CHECK_FALSE(slots.find(0));
    slots.forget(100);
    CHECK_FALSE(slots.find(100));
}

TEST_CASE("a sound on silent runs on by the clock", "[voice_priority]") {
    CHECK(vp::virtualCursor(100, 50, 1000, false) == 150);
    CHECK_FALSE(vp::virtualCursor(100, 900, 1000, false));
    CHECK(vp::virtualCursor(100, 950, 1000, true) == 50);
    CHECK(vp::virtualCursor(0, 10500, 1000, true) == 500);
    CHECK_FALSE(vp::virtualCursor(0, 0, 0, true));
}

TEST_CASE("what is no number is silence, and no place", "[audio]") {
    namespace fa = wowee::audio::finite_audio;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    std::vector<float> mix = {0.5f, nan, -0.25f, inf, -inf, 1.0f};
    CHECK(fa::silenceBad(mix.data(), mix.size()) == 3);
    CHECK(mix == std::vector<float>{0.5f, 0.0f, -0.25f, 0.0f, 0.0f, 1.0f});
    CHECK(fa::silenceBad(mix.data(), mix.size()) == 0);
    CHECK(fa::place(glm::vec3(1, 2, 3)));
    CHECK_FALSE(fa::place(glm::vec3(1, nan, 3)));
    CHECK_FALSE(fa::place(glm::vec3(inf, 0, 0)));
}

TEST_CASE("two sounds that differ only within are two", "[audio]") {
    // The cache of decoded sounds went by the first and the last 256 bytes and
    // the size: two of a length, quiet at both ends, were one.
    std::vector<uint8_t> a(40000, 0), b(40000, 0);
    for (size_t i = 12000; i < 28000; ++i) {
        a[i] = static_cast<uint8_t>(i);
        b[i] = static_cast<uint8_t>(i * 7);
    }
    CHECK(wowee::audio::sound_key::of(a) != wowee::audio::sound_key::of(b));
    CHECK(wowee::audio::sound_key::of(a) == wowee::audio::sound_key::of(std::vector<uint8_t>(a)));
    b = a;
    b.push_back(0);
    CHECK(wowee::audio::sound_key::of(a) != wowee::audio::sound_key::of(b));
}

// A play is handed bytes: whose they are is what was noted as they were read.
TEST_CASE("a sound is known by its bytes", "[audio]") {
    namespace sn = wowee::audio::sound_names;
    const std::vector<uint8_t> step(3000, 1), wind(3000, 2);
    sn::note(step, "Sound\\Character\\Footsteps\\a.wav");
    sn::note({}, "nothing.wav");
    sn::note(wind, "");
    CHECK(sn::of(wowee::audio::sound_key::of(step)) == "Sound\\Character\\Footsteps\\a.wav");
    CHECK(sn::of(wowee::audio::sound_key::of(wind)).empty());
    // The first name it was read by.
    sn::note(step, "other.wav");
    CHECK(sn::of(wowee::audio::sound_key::of(step)) == "Sound\\Character\\Footsteps\\a.wav");
}

TEST_CASE("a loop paused out of reach is still the one its handle is for", "[voice_priority]") {
    CHECK(vp::stillGoing(false, true, false));
    CHECK(vp::stillGoing(true, false, false));
    CHECK(vp::stillGoing(false, false, true));
    CHECK_FALSE(vp::stillGoing(false, false, false));
}

// 0x0072a680, 0x00746900.
TEST_CASE("a clicked unit says its hello five times, then its irritated lines in turn", "[audio]") {
    wowee::audio::NpcLineCounter counter;
    const auto click = [&](uint64_t unit, size_t irritated) {
        const auto ask = counter.ask(unit, irritated);
        counter.started(ask);
        return ask.irritated ? static_cast<int>(ask.line) : -1;
    };
    for (int i = 0; i < 5; ++i) CHECK(click(7, 2) == -1);
    CHECK(click(7, 2) == 0);
    CHECK(click(7, 2) == 1);
    for (int i = 0; i < 6; ++i) CHECK(click(7, 2) == -1);
    CHECK(click(7, 2) == 0);
    // One unit is counted: another, and this one again, begin anew.
    CHECK(click(8, 2) == -1);
    for (int i = 0; i < 5; ++i) CHECK(click(7, 2) == -1);
    CHECK(click(7, 2) == 0);
    // A window's hello forgets it.
    counter.forget();
    for (int i = 0; i < 5; ++i) CHECK(click(7, 2) == -1);
    // A line that did not start is not counted.
    wowee::audio::NpcLineCounter refused;
    for (int i = 0; i < 9; ++i) CHECK_FALSE(refused.ask(7, 2).irritated);
}
