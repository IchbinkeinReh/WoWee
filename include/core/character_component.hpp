#pragma once

/// How the client's character component paints worn items onto the body
/// texture: each item's region textures at a layer of their region
/// (0x004f2880, 0x004f2640 and the table at 0x009f6a00), the guild tabard's
/// emblem among them (0x004ec1c0), each region drawn layer by layer
/// (0x004e8f00).

#include "game/guild_emblem.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wowee {
namespace pipeline { class AssetManager; class DBCFile; }
namespace core {

/// 0x004f2880: the character component's item an equipment slot dresses -
/// shirt 2, chest 3, belt 4, legs 5, feet 6, wrists 7, hands 8, tabard 9 -
/// or -1 for one whose textures it does not paint (the head, shoulders and
/// cloak are models; the rest nothing).
constexpr int componentItemIndex(int equipSlot) {
    switch (equipSlot) {
        case 3: return 2;
        case 4: return 3;
        case 5: return 4;
        case 6: return 5;
        case 7: return 6;
        case 8: return 7;
        case 9: return 8;
        case 18: return 9;
        default: return -1;
    }
}

/// The same for an inventory type, as the select and create screens name
/// their items: shirt 4, chest 5 and robe 20, belt 6, legs 7, feet 8,
/// wrists 9, hands 10, tabard 19.
constexpr int componentItemForInventoryType(uint8_t inventoryType) {
    switch (inventoryType) {
        case 4: return 2;
        case 5: case 20: return 3;
        case 6: return 4;
        case 7: return 5;
        case 8: return 6;
        case 9: return 7;
        case 10: return 8;
        case 19: return 9;
        default: return -1;
    }
}

/// 0x009f6a00: the layer a component item's texture takes in each of the
/// eight regions (ItemDisplayInfo's texture columns: upper arm, lower arm,
/// hand, upper torso, lower torso, upper leg, lower leg, foot); -1 where it
/// is not painted at all.
constexpr int componentTextureLayer(int componentItem, int region) {
    constexpr int kLayers[8][8] = {
        // ArmU ArmL Hand TorU TorL LegU LegL Foot
        {0, 0, -1, 0, 0, -1, -1, -1},    // 2 shirt
        {1, 1, -1, 1, 1, 1, 1, -1},      // 3 chest
        {-1, -1, -1, -1, 5, 2, -1, -1},  // 4 belt
        {-1, -1, -1, -1, -1, 0, 0, -1},  // 5 legs
        {-1, -1, -1, -1, -1, -1, 2, 0},  // 6 feet
        {-1, 2, -1, -1, -1, -1, -1, -1}, // 7 wrists
        {-1, 3, 0, -1, -1, -1, -1, -1},  // 8 hands
        {-1, -1, -1, 4, 4, -1, -1, -1},  // 9 tabard
    };
    if (componentItem < 2 || componentItem > 9 || region < 0 || region > 7) return -1;
    return kLayers[componentItem - 2][region];
}

/// One texture of the component: its region, its layer and its file.
struct ComponentTexture {
    int region = 0;
    int layer = 0;
    std::string path;
};

/// ItemDisplayInfo Flags 1: a tabard that carries its guild's emblem
/// (0x006db510, 0x004e3cd0, 0x00705b20 test it).
constexpr uint32_t kItemDisplayFlagGuildTabard = 0x01;

/// 0x004ec1c0: the guild's design on the upper and lower torso - the
/// background (Background_<colour>) at layer 2, the border
/// (Border_<style>_<colour>) at 3 and the emblem (Emblem_<style>_<colour>)
/// at 4, where it takes the tabard's own place.
inline std::vector<ComponentTexture> guildTabardTextures(const game::GuildEmblem& e) {
    std::vector<ComponentTexture> out;
    if (!e.complete()) return out;
    char buf[128];
    for (const auto& [region, half] : {std::pair<int, const char*>{3, "TU"}, std::pair<int, const char*>{4, "TL"}}) {
        std::snprintf(buf, sizeof(buf), "Textures\\GuildEmblems\\Background_%02u_%s_U.blp", e.backgroundColor, half);
        out.push_back({region, 2, buf});
        std::snprintf(buf, sizeof(buf), "Textures\\GuildEmblems\\Border_%02u_%02u_%s_U.blp", e.borderStyle,
                      e.borderColor, half);
        out.push_back({region, 3, buf});
        std::snprintf(buf, sizeof(buf), "Textures\\GuildEmblems\\Emblem_%02u_%02u_%s_U.blp", e.emblemStyle,
                      e.emblemColor, half);
        out.push_back({region, 4, buf});
    }
    return out;
}

/// 0x004e8f00: one texture a region and layer - the one set last - drawn
/// in layer order, as compositeWithRegions takes them.
inline std::vector<std::pair<int, std::string>> componentRegionLayers(const std::vector<ComponentTexture>& textures) {
    std::vector<ComponentTexture> kept;
    for (const auto& t : textures) {
        auto same = std::find_if(kept.begin(), kept.end(), [&](const ComponentTexture& k) {
            return k.region == t.region && k.layer == t.layer;
        });
        if (same != kept.end()) *same = t;
        else kept.push_back(t);
    }
    std::stable_sort(kept.begin(), kept.end(), [](const ComponentTexture& a, const ComponentTexture& b) {
        return a.region != b.region ? a.region < b.region : a.layer < b.layer;
    });
    std::vector<std::pair<int, std::string>> out;
    out.reserve(kept.size());
    for (auto& k : kept) out.emplace_back(k.region, std::move(k.path));
    return out;
}

/// A worn item: its component item (componentItemIndex) and display.
struct ComponentItem {
    int componentItem = -1;
    uint32_t displayId = 0;
};

/// The body texture's item layers for these items, a guild tabard's
/// emblem on it when the design is known; for compositeWithRegions.
std::vector<std::pair<int, std::string>> characterComponentLayers(pipeline::AssetManager& assets,
                                                                  const pipeline::DBCFile& itemDisplayInfo,
                                                                  const std::vector<ComponentItem>& items,
                                                                  bool female,
                                                                  const std::optional<game::GuildEmblem>& emblem);

}  // namespace core
}  // namespace wowee
