#pragma once

/// A unit drawn as a player character: SMSG_MIRRORIMAGE_DATA.
///
/// A unit whose UNIT_FIELD_FLAGS_2 carry UNIT_FLAG2_MIRROR_IMAGE (0x10) is
/// not drawn from its own display. The client asks for its look with
/// CMSG_GET_MIRRORIMAGE_DATA (its guid) and draws the race and sex, skin,
/// face, hair, facial hair and worn items the answer gives, the way it draws
/// a player. The mage's Mirror Image uses it, and so do the server's
/// player-like NPCs - AzerothCore's NPCBots dress their bots this way
/// (creature outfits), which is why they came out as whatever their
/// placeholder display was: a troll with the wrong hair, a bot of another
/// race in someone else's robe.
///
/// 3.3.5a layout, 68 bytes:
///   uint64 guid, uint32 displayId, uint8 race, uint8 gender, uint8 class,
///   uint8 skin, face, hairStyle, hairColor, facialHair, uint32 guildId,
///   uint32 itemDisplayId[11] - head, shoulders, shirt, chest, waist, legs,
///   feet, wrists, hands, back, tabard (the server's EquipmentSlots order,
///   with back before tabard).
/// A creature caster sends zeros for everything after the class.
///
/// No game headers, so a test can read it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace wowee::game {

constexpr uint32_t kUnitFlag2MirrorImage = 0x00000010;

/// Where this client numbers the displays it makes for such units: past any
/// CreatureDisplayInfo row, so none is ever taken for one.
constexpr uint32_t kMirrorDisplayIdBase = 0xE0000000u;

struct MirrorImageLook {
    uint64_t guid = 0;
    uint32_t displayId = 0;
    uint8_t raceId = 0;
    uint8_t gender = 0;
    uint8_t classId = 0;
    uint8_t skin = 0;
    uint8_t face = 0;
    uint8_t hairStyle = 0;
    uint8_t hairColor = 0;
    uint8_t facialHair = 0;
    uint32_t guildId = 0;
    /// Wire order: head, shoulders, shirt, chest, waist, legs, feet, wrists,
    /// hands, back, tabard.
    std::array<uint32_t, 11> items{};

    /// Whether there is a character to draw: a race the client has a body
    /// for. A creature caster's answer leaves it zero.
    [[nodiscard]] bool hasCharacter() const { return raceId != 0; }

    /// Whether two answers draw the same character (the guild and the class
    /// draw nothing).
    [[nodiscard]] bool sameLookAs(const MirrorImageLook& o) const {
        return displayId == o.displayId && raceId == o.raceId && gender == o.gender && skin == o.skin &&
               face == o.face && hairStyle == o.hairStyle && hairColor == o.hairColor &&
               facialHair == o.facialHair && items == o.items;
    }

    /// The items in CreatureDisplayInfoExtra's NPCItemDisplay order - helm,
    /// shoulder, shirt, chest, belt, legs, feet, wrist, hands, tabard, cape -
    /// the order the humanoid NPC path dresses by. Only the last two differ.
    [[nodiscard]] std::array<uint32_t, 11> itemsInExtraOrder() const {
        std::array<uint32_t, 11> out = items;
        out[9] = items[10];   // tabard
        out[10] = items[9];   // back
        return out;
    }
};

/// Reads the answer from its first byte. Empty when it is shorter than the
/// part that names the unit and its race; the appearance and items, where
/// cut short, stay zero.
[[nodiscard]] inline std::optional<MirrorImageLook> parseMirrorImageData(const uint8_t* data, size_t size) {
    size_t pos = 0;
    auto u8 = [&]() -> uint8_t { return pos < size ? data[pos++] : (pos++, 0); };
    auto u32 = [&]() -> uint32_t {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(u8()) << (8 * i);
        return v;
    };
    if (size < 15) return std::nullopt;
    MirrorImageLook look;
    uint64_t g = 0;
    for (int i = 0; i < 8; ++i) g |= static_cast<uint64_t>(u8()) << (8 * i);
    look.guid = g;
    look.displayId = u32();
    look.raceId = u8();
    look.gender = u8();
    look.classId = u8();
    look.skin = u8();
    look.face = u8();
    look.hairStyle = u8();
    look.hairColor = u8();
    look.facialHair = u8();
    look.guildId = u32();
    for (auto& item : look.items) item = u32();
    return look;
}

namespace mirror_model_path {
/// Model paths compared as the archive compares them: case and slash
/// direction do not matter, nor the .mdx/.m2 extension a DBC may name.
[[nodiscard]] inline bool samePath(std::string_view a, std::string_view b) {
    auto norm = [](std::string_view p) {
        std::string out;
        out.reserve(p.size());
        for (char c : p) {
            if (c == '/') c = '\\';
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            out.push_back(c);
        }
        for (const char* ext : {".mdx", ".mdl", ".m2"}) {
            const std::string e(ext);
            if (out.size() >= e.size() && out.compare(out.size() - e.size(), e.size(), e) == 0) {
                out.resize(out.size() - e.size());
                break;
            }
        }
        return out;
    };
    return norm(a) == norm(b);
}
}  // namespace mirror_model_path

}  // namespace wowee::game
