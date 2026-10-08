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
