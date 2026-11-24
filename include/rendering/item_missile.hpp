#pragma once

#include <cstdint>
#include <string>

namespace wowee::rendering::item_missile {

/// Whose model a spell's missile is when its visual names none of its own
/// (0x00732ff0): SpellVisual MissileModel -1 to -3 the item in the caster's main
/// hand, off hand or ranged slot, -4 and -5 its ammunition, 0 its ammunition where
/// the cast has any. A visual without a missile flies its ammunition (0x007fffb0).
enum class Source : uint8_t { None, MainHand, OffHand, Ranged, Ammo, PlainAmmo };

[[nodiscard]] constexpr Source source(int32_t missileModel, bool hasMissile, uint32_t ammoDisplay) {
    if (!hasMissile && ammoDisplay == 0) return Source::None;
    switch (missileModel) {
        case -1: return Source::MainHand;
        case -2: return Source::OffHand;
        case -3: return Source::Ranged;
        case -4:
        case -5: return ammoDisplay != 0 ? Source::Ammo : Source::None;
        case 0: return ammoDisplay != 0 ? Source::PlainAmmo : Source::None;
        default: return Source::None;
    }
}

/// An ItemDisplayInfo row's models and their textures, left and right.
struct Display {
    std::string leftModel, rightModel, leftTexture, rightTexture;
};

/// "model|texture" for the model of an item of an inventory type, its texture the
/// one the model leaves to the item; empty for none. An item's own (0x00732100) is
/// its left model, else its right, out of its type's folder; ammunition's and a
/// thrown weapon's (0x004eab50) the right one, with its texture. A head's is by race
/// and none here.
[[nodiscard]] inline std::string model(Source from, const Display& display, uint32_t inventoryType) {
    constexpr uint32_t kHead = 1, kShoulder = 3, kShield = 14, kAmmo = 24, kThrown = 25, kQuiver = 27;
    if (from == Source::None || inventoryType == kHead) return {};
    std::string folder = "Item\\ObjectComponents\\";
    switch (inventoryType) {
        case kShoulder: folder += "Shoulder\\"; break;
        case kShield: folder += "Shield\\"; break;
        case kAmmo: folder += "Ammo\\"; break;
        case kQuiver: folder += "Quiver\\"; break;
        default: folder += "Weapon\\"; break;
    }
    const bool plain = from == Source::PlainAmmo;
    if (plain && inventoryType != kAmmo && inventoryType != kThrown) return {};
    const bool right = plain || display.leftModel.empty();
    std::string name = right ? display.rightModel : display.leftModel;
    const std::string& texture = right ? display.rightTexture : display.leftTexture;
    if (name.empty() || (plain && texture.empty())) return {};
    if (const size_t dot = name.rfind('.'); dot != std::string::npos) name.resize(dot);
    return folder + name + ".m2" + (texture.empty() ? std::string() : "|" + folder + texture + ".blp");
}

}  // namespace wowee::rendering::item_missile
