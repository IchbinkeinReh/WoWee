// Spell visual kits on a unit: where a kit's models hang, how a model-attach
// row places its model, and when an aura's Flags 8 state kit shows.
//
// An aura's shield, glow or buff effect is its SpellVisual's StateKit, which
// the client hangs on the unit while the aura lasts (0x00724820) and takes off
// as it goes (0x0071e930). These pin the client's rules for that apart from
// the renderer.
#include <catch_amalgamated.hpp>

#include "core/weapon_attachment.hpp"
#include "rendering/spell_kit.hpp"

#include <glm/glm.hpp>

namespace sk = wowee::rendering::spell_kit;
using wowee::core::AnimationSheathInput;
using wowee::core::SheathState;
using wowee::core::animationKitIdle;

TEST_CASE("a kit's model columns hang on 0x00745230's attachments", "[spell_kit]") {
    auto attachmentOf = [](const char* column) -> int {
        for (const auto& slot : sk::kKitSlots)
            if (std::string(slot.column) == column) return static_cast<int>(slot.attachment);
        return -1;
    };
    CHECK(attachmentOf("HeadEffect") == 20);
    CHECK(attachmentOf("ChestEffect") == 34);
    CHECK(attachmentOf("BaseEffect") == 19);
    CHECK(attachmentOf("LeftHandEffect") == 21);
    CHECK(attachmentOf("RightHandEffect") == 22);
    CHECK(attachmentOf("BreathEffect") == 17);
    CHECK(attachmentOf("SpecialEffect0") == 23);
    CHECK(attachmentOf("SpecialEffect1") == 24);
    CHECK(attachmentOf("SpecialEffect2") == 25);
    // Only the state kit loops.
    CHECK(sk::kitLoops(sk::KitType::State));
    CHECK_FALSE(sk::kitLoops(sk::KitType::StateDone));
    CHECK_FALSE(sk::kitLoops(sk::KitType::Cast));
}

TEST_CASE("a model-attach row's offset has its Y negated and its yaw turned back", "[spell_kit]") {
    // 0x006f84f0: (OffsetX, -OffsetY, OffsetZ).
    const glm::mat4 plain = sk::modelAttachMatrix(glm::vec3(1.0f, 2.0f, 3.0f), 0.0f, 0.0f, 0.0f);
    CHECK(plain[3].x == Catch::Approx(1.0f));
    CHECK(plain[3].y == Catch::Approx(-2.0f));
    CHECK(plain[3].z == Catch::Approx(3.0f));
    // A quarter turn of yaw takes +X to -Y (the negated angle about Z).
    const glm::mat4 yawed = sk::modelAttachMatrix(glm::vec3(0.0f), glm::radians(90.0f), 0.0f, 0.0f);
    const glm::vec4 x = yawed * glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
    CHECK(x.x == Catch::Approx(0.0f).margin(1e-6));
    CHECK(x.y == Catch::Approx(-1.0f));
    // Roll before pitch: +Y rolled a quarter turn about X is +Z, which the
    // pitch then turns toward +X.
    const glm::mat4 turned =
        sk::modelAttachMatrix(glm::vec3(0.0f), 0.0f, glm::radians(90.0f), glm::radians(90.0f));
    const glm::vec4 y = turned * glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
    CHECK(y.x == Catch::Approx(1.0f));
    CHECK(y.z == Catch::Approx(0.0f).margin(1e-6));
}

TEST_CASE("0x00720400 hides the Flags 8 kits and shows the first again", "[spell_kit]") {
    sk::UnarmedKitBits bits;
    // Nothing to do for a unit with none of those auras, unless forced.
    CHECK(sk::unarmedKitStep(bits, true, false, false) == sk::UnarmedKitStep::None);
    // The aura lands while the unit holds its weapons: forced, they go.
    CHECK(sk::unarmedKitStep(bits, false, true, true) == sk::UnarmedKitStep::HideAll);
    CHECK(bits.any);
    CHECK_FALSE(bits.shown);
    // Still armed: nothing more.
    CHECK(sk::unarmedKitStep(bits, false, false, true) == sk::UnarmedKitStep::None);
    // Weapons away and idle: the first such kit plays again.
    CHECK(sk::unarmedKitStep(bits, true, false, true) == sk::UnarmedKitStep::ShowFirst);
    CHECK(bits.shown);
    CHECK(sk::unarmedKitStep(bits, true, false, true) == sk::UnarmedKitStep::None);
    // A cast or drawn weapons hide them.
    CHECK(sk::unarmedKitStep(bits, false, false, true) == sk::UnarmedKitStep::HideAll);
    // A second aura landing while they show plays nothing more of the first.
    sk::UnarmedKitBits showing{.any = true, .shown = true};
    CHECK(sk::unarmedKitStep(showing, true, true, true) == sk::UnarmedKitStep::None);
}

TEST_CASE("0x00738180 sets the unit's idle bit where it runs to its end", "[spell_kit]") {
    AnimationSheathInput idle{.current = SheathState::Unarmed, .animId = 0, .animKnown = true,
                              .activePlayer = true};
    CHECK(animationKitIdle(idle) == true);
    // A cast clears it.
    AnimationSheathInput casting = idle;
    casting.casting = true;
    CHECK(animationKitIdle(casting) == false);
    // WeaponFlags 4 clears it.
    AnimationSheathInput hides = idle;
    hides.weaponFlags = 4;
    CHECK(animationKitIdle(hides) == false);
    // Attacking draws melee and leaves it as it was.
    AnimationSheathInput attacking = idle;
    attacking.attacking = true;
    CHECK_FALSE(animationKitIdle(attacking).has_value());
    // In the ranged state it is left alone.
    AnimationSheathInput ranged = idle;
    ranged.current = SheathState::Ranged;
    CHECK_FALSE(animationKitIdle(ranged).has_value());
    // Another unit taking its field's state returns before the end.
    AnimationSheathInput other = idle;
    other.activePlayer = false;
    other.field = SheathState::Melee;
    CHECK_FALSE(animationKitIdle(other).has_value());
}

#include "rendering/camera_shake.hpp"

namespace cs = wowee::rendering::camera_shake;

TEST_CASE("a camera shake fades with the camera's distance", "[camera_shake]") {
    // 0x006004b0: whole within 9 yards, 0.7 per 9 yards beyond, none past 80.
    CHECK(*cs::attenuated(1.0f, 8.0f * 8.0f) == Catch::Approx(1.0f));
    CHECK(*cs::attenuated(1.0f, 18.0f * 18.0f) == Catch::Approx(0.7f));
    CHECK(*cs::attenuated(1.0f, 27.0f * 27.0f) == Catch::Approx(0.49f));
    CHECK_FALSE(cs::attenuated(1.0f, 81.0f * 81.0f).has_value());
    // The row's amplitude is in 36ths.
    CHECK(cs::fromRow(0, 0, 36.0f, 1.0f, 1.0f, 0.0f, 0.0f).amplitude == Catch::Approx(1.0f));
}

TEST_CASE("a camera shake moves along the facing, its left or up", "[camera_shake]") {
    // A quarter cycle in, a 1 Hz shake is at its peak.
    std::vector<cs::Active> shakes;
    const cs::Shake up = cs::fromRow(0, 2, 36.0f, 1.0f, 2.0f, 0.0f, 0.0f);
    shakes.push_back({.shake = up, .origin = glm::vec3(0.0f), .startSeconds = 0.0f});
    glm::vec3 o = cs::offset(shakes, 0.25f, glm::vec3(0.0f), 0.0f);
    CHECK(o.z == Catch::Approx(1.0f));
    CHECK(o.x == Catch::Approx(0.0f));
    // Forward at a facing of a quarter turn is +Y; left of it is -X.
    shakes.clear();
    shakes.push_back({.shake = cs::fromRow(0, 0, 36.0f, 1.0f, 2.0f, 0.0f, 0.0f), .startSeconds = 0.0f});
    shakes.push_back({.shake = cs::fromRow(0, 1, 18.0f, 1.0f, 2.0f, 0.0f, 0.0f), .startSeconds = 0.0f});
    o = cs::offset(shakes, 0.25f, glm::vec3(0.0f), glm::radians(90.0f));
    CHECK(o.y == Catch::Approx(1.0f));
    CHECK(o.x == Catch::Approx(-0.5f));
    // Only the strongest in a direction moves it.
    shakes.clear();
    shakes.push_back({.shake = cs::fromRow(0, 2, 36.0f, 1.0f, 2.0f, 0.0f, 0.0f), .startSeconds = 0.0f});
    shakes.push_back({.shake = cs::fromRow(0, 2, 72.0f, 1.0f, 2.0f, 0.0f, 0.0f), .startSeconds = 0.0f});
    CHECK(cs::offset(shakes, 0.25f, glm::vec3(0.0f), 0.0f).z == Catch::Approx(2.0f));
    // Type 1 decays by e^(-t c).
    const cs::Shake decaying = cs::fromRow(1, 2, 36.0f, 1.0f, 2.0f, 0.0f, 2.0f);
    CHECK(cs::displacement(decaying, 1.0f, 0.25f) == Catch::Approx(std::exp(-0.5f)));
    // Run out (Phase counts toward Duration), it is dropped.
    shakes.clear();
    shakes.push_back({.shake = cs::fromRow(0, 2, 36.0f, 1.0f, 1.0f, 0.5f, 0.0f), .startSeconds = 0.0f});
    CHECK(cs::offset(shakes, 0.6f, glm::vec3(0.0f), 0.0f) == glm::vec3(0.0f));
    CHECK(shakes.empty());
}

TEST_CASE("a kit's colour fade holds, then fades to white", "[spell_kit]") {
    // 0x007265c0 case 13: red held a second, faded over two.
    const sk::ColourFade fade{.startMs = 1000, .colour = 0xFFFF0000u, .holdMs = 1000, .fadeMs = 2000};
    uint32_t c = 0;
    REQUIRE(sk::fadeColour(fade, 1500, c));
    CHECK(c == 0xFFFF0000u);
    // Half-way through the fade: half way back to white, in 0x006acc50's
    // unsigned byte arithmetic (alpha 128 takes 255 to 0x7f).
    REQUIRE(sk::fadeColour(fade, 3000, c));
    CHECK(((c >> 16) & 0xff) == 0xff);
    CHECK((c & 0xff) == 0x7f);
    CHECK(((c >> 8) & 0xff) == 0x7f);
    // Run out.
    CHECK_FALSE(sk::fadeColour(fade, 4001, c));
    // The model's colour from 0xRRGGBB.
    const glm::vec3 rgb = sk::colourToRgb(0xFF336699u);
    CHECK(rgb.r == Catch::Approx(0x33 / 255.0f));
    CHECK(rgb.g == Catch::Approx(0x66 / 255.0f));
    CHECK(rgb.b == Catch::Approx(0x99 / 255.0f));
    // A whole alpha takes the colour outright, keeping the alpha it had.
    CHECK(sk::lerpColour(0x80FFFFFFu, 0xFF102030u, 0xff) == 0x80102030u);
}

TEST_CASE("0x00745230 places a kit's models by the kit's type", "[spell_kit]") {
    using sk::KitModelKind;
    using sk::KitModelPlace;
    using sk::KitType;
    // A cast or impact kit on a unit: columns at their attachments, the
    // WorldEffect and an attachment-less row where the unit stands.
    CHECK(sk::kitModelPlace(KitType::Cast, KitModelKind::Column, 21, false) == KitModelPlace::Attachment);
    CHECK(sk::kitModelPlace(KitType::Cast, KitModelKind::Base, 19, false) == KitModelPlace::Attachment);
    CHECK(sk::kitModelPlace(KitType::Precast, KitModelKind::World, -1, false) == KitModelPlace::AtUnit);
    CHECK(sk::kitModelPlace(KitType::Cast, KitModelKind::AttachRow, -1, false) == KitModelPlace::AtUnit);
    CHECK(sk::kitModelPlace(KitType::Cast, KitModelKind::AttachRow, 0, false) == KitModelPlace::Attachment);
    // Given a place, every model goes there.
    CHECK(sk::kitModelPlace(KitType::Cast, KitModelKind::Column, 22, true) == KitModelPlace::AtPlace);
    // An area kit (flag 0x2000): its head and world effects, and its base
    // and rows only at a place.
    CHECK(sk::kitModelPlace(KitType::Area, KitModelKind::Head, 20, false) == KitModelPlace::Attachment);
    CHECK(sk::kitModelPlace(KitType::Area, KitModelKind::World, -1, false) == KitModelPlace::AtUnit);
    CHECK(sk::kitModelPlace(KitType::Area, KitModelKind::Base, 19, false) == KitModelPlace::None);
    CHECK(sk::kitModelPlace(KitType::Area, KitModelKind::Base, 19, true) == KitModelPlace::AtPlace);
    CHECK(sk::kitModelPlace(KitType::Area, KitModelKind::AttachRow, 5, true) == KitModelPlace::AtPlace);
    CHECK(sk::kitModelPlace(KitType::Area, KitModelKind::Column, 21, true) == KitModelPlace::None);
}

TEST_CASE("a kit's models play once, repeat for the cast, or hold for the aura", "[spell_kit]") {
    CHECK(sk::kitModelLife(sk::KitType::Precast) == sk::KitModelLife::Repeat);
    CHECK(sk::kitModelLife(sk::KitType::State) == sk::KitModelLife::Hold);
    for (auto type : {sk::KitType::PlayImpact, sk::KitType::Cast, sk::KitType::Area, sk::KitType::StateDone})
        CHECK(sk::kitModelLife(type) == sk::KitModelLife::Once);
    // 0x00744870: the Stand runs once, then the Decay where there is one.
    const auto timing = sk::onceTiming(1500.0f, true);
    CHECK(timing.switchAt == Catch::Approx(1.5f));
    CHECK(timing.decays);
    CHECK_FALSE(sk::onceTiming(800.0f, false).decays);
}

TEST_CASE("a unit's impact kit is its caster's or its target's", "[spell_kit]") {
    // 0x00800d00: CasterImpactKit on the caster, TargetImpactKit on any
    // other, the ImpactKit where the visual lacks one.
    CHECK(sk::impactKitFor(true, 10, 11, 12) == 11);
    CHECK(sk::impactKitFor(false, 10, 11, 12) == 12);
    CHECK(sk::impactKitFor(true, 10, 0, 12) == 10);
    CHECK(sk::impactKitFor(false, 10, 11, 0) == 10);
}

TEST_CASE("a kit model placed in the world is sized by the unit and its effect", "[spell_kit]") {
    // 0x006f7950: 0.3 of the narrower side of the model's box, at least 1.
    CHECK(sk::unitWorldEffectSize(1.0f, glm::vec3(-1.0f), glm::vec3(1.0f)) == Catch::Approx(1.0f));
    CHECK(sk::unitWorldEffectSize(1.0f, glm::vec3(-5.0f, -10.0f, 0.0f), glm::vec3(5.0f, 10.0f, 3.0f)) ==
          Catch::Approx(3.0f));
    CHECK(sk::unitWorldEffectSize(0.5f, glm::vec3(-5.0f, -10.0f, 0.0f), glm::vec3(5.0f, 10.0f, 3.0f)) ==
          Catch::Approx(1.5f));
    // 0x006f8ae0: times the effect's Scale, within its allowed scales.
    CHECK(sk::worldKitModelScale(1.0f, 1.0f, 0.1f, 10.0f) == Catch::Approx(1.0f));
    CHECK(sk::worldKitModelScale(3.0f, 2.0f, 0.1f, 4.0f) == Catch::Approx(4.0f));
    CHECK(sk::worldKitModelScale(1.0f, 0.01f, 0.5f, 4.0f) == Catch::Approx(0.5f));
    // No allowed scales at all: 0, which is taken as 1.
    CHECK(sk::worldKitModelScale(2.0f, 1.0f, 0.0f, 0.0f) == Catch::Approx(1.0f));
    // Its offset turns with the facing and is not scaled.
    const glm::mat4 local = sk::modelAttachMatrix(glm::vec3(1.0f, 0.0f, 0.0f), 0.0f, 0.0f, 0.0f);
    const glm::mat4 m = sk::worldKitModelMatrix(glm::vec3(10.0f, 20.0f, 0.0f), glm::radians(90.0f), local, 3.0f);
    CHECK(m[3].x == Catch::Approx(10.0f).margin(1e-5));
    CHECK(m[3].y == Catch::Approx(21.0f));
    CHECK(glm::length(glm::vec3(m[0])) == Catch::Approx(3.0f));
}
