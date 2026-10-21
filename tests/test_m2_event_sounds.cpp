// Which of a model's events fire as its animation steps, and what sound the
// game object and spell effect callbacks play for one.
//
// 0x00830fb0 fires an event whose time is after where the last step got to
// and at or before where this one gets to, again every lap of a looping
// sequence and once for one with flag 0x1. 0x0070c050 (a game object's) and
// 0x006f7b00 (a spell effect's) say what each id plays.
#include <catch_amalgamated.hpp>

#include <vector>

#include "rendering/m2_event_sounds.hpp"

namespace m2_event = wowee::rendering::m2_event;
using m2_event::Owner;
using m2_event::Play;

namespace {

std::vector<uint32_t> crossed(const std::vector<uint32_t>& times, float from, float to, float lap) {
    std::vector<uint32_t> out;
    m2_event::forEachCrossed(times, from, to, lap, [&](uint32_t t) { out.push_back(t); });
    return out;
}

std::vector<uint32_t> crossedBack(const std::vector<uint32_t>& times, uint32_t duration, float from, float to,
                                  float lap) {
    std::vector<uint32_t> out;
    m2_event::forEachCrossedReversed(times, duration, from, to, lap, [&](uint32_t t) { out.push_back(t); });
    return out;
}

}  // namespace

TEST_CASE("A sequence played backwards passes its events from its end", "[m2_event]") {
    const std::vector<uint32_t> times{100, 700};
    // 1000 ms long: the event at 700 comes 300 ms in, the one at 100 at 900.
    CHECK(crossedBack(times, 1000, 0.0f, 299.0f, 0.0f).empty());
    CHECK(crossedBack(times, 1000, 0.0f, 300.0f, 0.0f) == std::vector<uint32_t>{700});
    CHECK(crossedBack(times, 1000, 300.0f, 1000.0f, 0.0f) == std::vector<uint32_t>{100});
    // Looping, in the order passed, lap after lap.
    CHECK(crossedBack(times, 1000, 850.0f, 1350.0f, 1000.0f) == std::vector<uint32_t>{100, 700});
    // An event past the sequence's end is never passed.
    CHECK(crossedBack({1200}, 1000, 0.0f, 1000.0f, 0.0f).empty());
}

TEST_CASE("An event fires once as the animation passes it", "[m2_event]") {
    const std::vector<uint32_t> times{100, 500};
    // After where it was, at or before where it is.
    CHECK(crossed(times, 0.0f, 100.0f, 0.0f) == std::vector<uint32_t>{100});
    CHECK(crossed(times, 100.0f, 200.0f, 0.0f).empty());
    CHECK(crossed(times, 99.0f, 600.0f, 0.0f) == std::vector<uint32_t>{100, 500});
    // Standing still passes nothing, even on an event.
    CHECK(crossed(times, 100.0f, 100.0f, 0.0f).empty());
}

TEST_CASE("A sequence just started fires its first frame's events", "[m2_event]") {
    const std::vector<uint32_t> times{0, 300};
    CHECK(crossed(times, -1.0f, 16.0f, 1000.0f) == std::vector<uint32_t>{0});
    // Where the step only goes on from 0, the one at 0 is behind it.
    CHECK(crossed(times, 0.0f, 16.0f, 1000.0f).empty());
}

TEST_CASE("A looping sequence fires its events again every lap", "[m2_event]") {
    const std::vector<uint32_t> times{0, 400};
    // Over the end of a 1000 ms lap: the 400 of this lap is behind, the 0 of
    // the next is passed.
    CHECK(crossed(times, 990.0f, 1010.0f, 1000.0f) == std::vector<uint32_t>{0});
    // Past the 400 of the next lap as well.
    CHECK(crossed(times, 900.0f, 1400.0f, 1000.0f) == std::vector<uint32_t>{0, 400});
    // A step over two whole laps passes each event twice, in order.
    CHECK(crossed(times, 450.0f, 2450.0f, 1000.0f) == std::vector<uint32_t>{0, 400, 0, 400});
    // Without a lap - flag 0x1 - the end is the end.
    CHECK(crossed(times, 990.0f, 1410.0f, 0.0f).empty());
}

TEST_CASE("A game object's $GOn and $GCn are its display's Sound slots", "[m2_event]") {
    // 0x0070c050 hands 0x0070bf70 the slot in edx.
    CHECK(m2_event::gameObjectSoundSlot(0x304f4724u) == std::optional<uint32_t>(0));  // $GO0
    CHECK(m2_event::gameObjectSoundSlot(0x354f4724u) == std::optional<uint32_t>(5));  // $GO5
    CHECK(m2_event::gameObjectSoundSlot(0x30434724u) == std::optional<uint32_t>(6));  // $GC0
    CHECK(m2_event::gameObjectSoundSlot(0x33434724u) == std::optional<uint32_t>(9));  // $GC3
    CHECK_FALSE(m2_event::gameObjectSoundSlot(0x364f4724u).has_value());              // $GO6
    CHECK_FALSE(m2_event::gameObjectSoundSlot(0x34434724u).has_value());              // $GC4
    CHECK_FALSE(m2_event::gameObjectSoundSlot(m2_event::kSound).has_value());
    CHECK(m2_event::fourCC("$GC0") == 0x30434724u);
    CHECK(m2_event::kSound == 0x444e5324u);
    CHECK(m2_event::kDoodadSoundOnce == 0x4f534424u);
    CHECK(m2_event::kDoodadSoundLoop == 0x4c534424u);
    CHECK(m2_event::kDoodadSoundEnd == 0x45534424u);
    CHECK(m2_event::kCameraShake == 0x4b485324u);
}

TEST_CASE("What a game object's events play", "[m2_event]") {
    // The fishing bobber's splash: $GC0, its display's (668) Sound[6], 3355.
    const auto bite = m2_event::planSound(Owner::GameObject, m2_event::fourCC("$GC0"), 0);
    CHECK(bite.play == Play::ObjectKit);
    CHECK(bite.slotted);
    CHECK(bite.sound == 6u);
    // $SND and $DSO: the data's sound, once, where the event is.
    const auto snd = m2_event::planSound(Owner::GameObject, m2_event::kSound, 1234);
    CHECK(snd.play == Play::Once);
    CHECK_FALSE(snd.slotted);
    CHECK(snd.sound == 1234u);
    CHECK(m2_event::planSound(Owner::GameObject, m2_event::kDoodadSoundOnce, 7).play == Play::Once);
    // $DSL: the data's sound looping on the object, in and out over a
    // second each (0x0070c050 sets the play's +0x10 and +0x14 to 1).
    const auto loop = m2_event::planSound(Owner::GameObject, m2_event::kDoodadSoundLoop, 7);
    CHECK(loop.play == Play::ObjectLoop);
    CHECK(loop.sound == 7u);
    CHECK(loop.fadeIn == 1.0f);
    CHECK(loop.fadeOut == 1.0f);
    // A display's looping kit plays as the defaults have it, without fades.
    CHECK(bite.fadeIn == 0.0f);
    // $DSE is a doodad's; a game object has no case for it.
    CHECK(m2_event::planSound(Owner::GameObject, m2_event::kDoodadSoundEnd, 7).play == Play::None);
    // A camera shake ($SHK) or a footfall plays nothing here.
    CHECK(m2_event::planSound(Owner::GameObject, m2_event::fourCC("$SHK"), 7).play == Play::None);
    CHECK(m2_event::planSound(Owner::GameObject, m2_event::fourCC("$FSD"), 7).play == Play::None);
}

TEST_CASE("What a spell effect's events play", "[m2_event]") {
    // 0x006f7b00: $SND alone.
    const auto snd = m2_event::planSound(Owner::Effect, m2_event::kSound, 99);
    CHECK(snd.play == Play::OnEffect);
    CHECK(snd.sound == 99u);
    CHECK(m2_event::planSound(Owner::Effect, m2_event::kDoodadSoundOnce, 99).play == Play::None);
    CHECK(m2_event::planSound(Owner::Effect, m2_event::fourCC("$GO0"), 99).play == Play::None);
    // A model nobody owns plays nothing.
    CHECK(m2_event::planSound(Owner::None, m2_event::kSound, 99).play == Play::None);
}

TEST_CASE("What a map doodad's events play", "[m2_event]") {
    // 0x007bd5a0: $DSO once where the event is.
    const auto once = m2_event::planSound(Owner::Doodad, m2_event::kDoodadSoundOnce, 3346);
    CHECK(once.play == Play::Once);
    CHECK(once.sound == 3346u);
    // $DSL looping on the doodad, rising over 3 seconds (the play's +0x10),
    // with no fade out of its own.
    const auto loop = m2_event::planSound(Owner::Doodad, m2_event::kDoodadSoundLoop, 3346);
    CHECK(loop.play == Play::ObjectLoop);
    CHECK(loop.sound == 3346u);
    CHECK(loop.fadeIn == 3.0f);
    CHECK(loop.fadeOut == 0.0f);
    // $DSE ends it.
    CHECK(m2_event::planSound(Owner::Doodad, m2_event::kDoodadSoundEnd, 0).play == Play::StopLoop);
    // $SND and a game object's display slots are not a doodad's.
    CHECK(m2_event::planSound(Owner::Doodad, m2_event::kSound, 3346).play == Play::None);
    CHECK(m2_event::planSound(Owner::Doodad, m2_event::fourCC("$GO0"), 0).play == Play::None);
    // The elevator states ($EAC, $EDC, $EWT, $EMV) set its game object's
    // ObjectEffect state, and play nothing themselves.
    for (uint32_t id : {m2_event::fourCC("$EAC"), m2_event::fourCC("$EDC"), m2_event::fourCC("$EWT"),
                        m2_event::fourCC("$EMV")}) {
        CAPTURE(id);
        CHECK(m2_event::planSound(Owner::Doodad, id, 0).play == Play::None);
    }
    // A doodad's events shake nothing.
    CHECK_FALSE(m2_event::shakesCamera(Owner::Doodad, m2_event::kCameraShake));
}

TEST_CASE("A game object's $SHK shakes the camera", "[m2_event]") {
    // 0x0070c050: the data is a SpellEffectCameraShakes row, each of its
    // CameraShakes put on the camera where the event is (0x00606410).
    CHECK(m2_event::shakesCamera(Owner::GameObject, m2_event::kCameraShake));
    CHECK_FALSE(m2_event::shakesCamera(Owner::GameObject, m2_event::kSound));
    // A spell effect's callback (0x006f7b00) has no $SHK, nor a doodad's
    // (0x007bd5a0).
    CHECK_FALSE(m2_event::shakesCamera(Owner::Effect, m2_event::kCameraShake));
    CHECK_FALSE(m2_event::shakesCamera(Owner::None, m2_event::kCameraShake));
}

TEST_CASE("A dynamic object's $SND plays once and its $SHK shakes the camera", "[m2_event]") {
    // 0x00704c30: $SND the data's SoundEntries row with no sound object.
    const auto snd = m2_event::planSound(Owner::DynamicObject, m2_event::kSound, 3346);
    CHECK(snd.play == Play::Once);
    CHECK(snd.sound == 3346);
    CHECK(m2_event::shakesCamera(Owner::DynamicObject, m2_event::kCameraShake));
    // Nothing else of the other owners'.
    CHECK(m2_event::planSound(Owner::DynamicObject, m2_event::kDoodadSoundOnce, 7).play == Play::None);
    CHECK(m2_event::planSound(Owner::DynamicObject, m2_event::kDoodadSoundLoop, 7).play == Play::None);
    CHECK(m2_event::planSound(Owner::DynamicObject, m2_event::fourCC("$GO0"), 0).play == Play::None);
    CHECK_FALSE(m2_event::shakesCamera(Owner::DynamicObject, m2_event::kSound));
}

TEST_CASE("A falling model's events play only every fourth, and only a $SND", "[m2_event]") {
    // 0x007f9d80: one count for all of them; each fourth, a $SND plays once.
    const auto snd = m2_event::planSound(Owner::FallingModel, m2_event::kSound, 3346);
    CHECK(snd.play == Play::Once);
    CHECK(snd.sound == 3346);
    CHECK(m2_event::planSound(Owner::FallingModel, m2_event::kDoodadSoundOnce, 7).play == Play::None);
    CHECK_FALSE(m2_event::shakesCamera(Owner::FallingModel, m2_event::kCameraShake));
    uint32_t counter = 0;
    CHECK_FALSE(m2_event::fallingEventPlays(counter));
    CHECK_FALSE(m2_event::fallingEventPlays(counter));
    CHECK_FALSE(m2_event::fallingEventPlays(counter));
    CHECK(m2_event::fallingEventPlays(counter));
    CHECK_FALSE(m2_event::fallingEventPlays(counter));
    CHECK(counter == 5);
}
