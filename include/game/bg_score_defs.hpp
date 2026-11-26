#pragma once

#include <cstdint>
#include <cstddef>

namespace wowee {
namespace game {

/**
 * Battleground score presentation, keyed by map.
 *
 * The server sends world states as bare key/value pairs - SMSG_INIT_WORLD_STATES
 * carries no labels - so which key means "Alliance flags" and what it counts up
 * to is client knowledge. This table is that knowledge.
 *
 * It lives here rather than beside a renderer because two surfaces present the
 * same scores: this client's own heads-up display, and FrameXML's
 * WorldStateAlwaysUpFrame through GetWorldStateUIInfo. Keeping one table means
 * a new battleground is described once.
 */
struct BgScoreDef {
    uint32_t    mapId;
    const char* name;
    uint32_t    allianceKey;   // world state key for the Alliance value
    uint32_t    hordeKey;      // world state key for the Horde value
    uint32_t    maxKey;        // world state key for the maximum (0 = use hardcodedMax)
    uint32_t    hardcodedMax;  // used when maxKey is 0
    const char* unit;          // suffix label ("flags", "resources"); empty = no maximum shown
};

inline constexpr BgScoreDef kBgScoreDefs[] = {
    // Warsong Gulch: 3 flag captures wins
    { .mapId = 489, .name = "Warsong Gulch",          .allianceKey = 1581, .hordeKey = 1582, .maxKey = 0,    .hardcodedMax = 3, .unit = "flags" },
    // Arathi Basin: 1600 resources wins
    { .mapId = 529, .name = "Arathi Basin",           .allianceKey = 1218, .hordeKey = 1219, .maxKey = 0, .hardcodedMax = 1600, .unit = "resources" },
    // Alterac Valley: reinforcements count down from 600 / 800 etc.
    {  .mapId = 30, .name = "Alterac Valley",         .allianceKey = 1322, .hordeKey = 1323, .maxKey = 0,  .hardcodedMax = 600, .unit = "reinforcements" },
    // Eye of the Storm: 1600 resources wins
    { .mapId = 566, .name = "Eye of the Storm",       .allianceKey = 2757, .hordeKey = 2758, .maxKey = 0, .hardcodedMax = 1600, .unit = "resources" },
    // Strand of the Ancients (WotLK)
    { .mapId = 607, .name = "Strand of the Ancients", .allianceKey = 3476, .hordeKey = 3477, .maxKey = 0,    .hardcodedMax = 4, .unit = "" },
    // Isle of Conquest (WotLK): reinforcements (300 default)
    { .mapId = 628, .name = "Isle of Conquest",       .allianceKey = 4221, .hordeKey = 4222, .maxKey = 0,  .hardcodedMax = 300, .unit = "reinforcements" },
};

/// The definition for a map, or nullptr when the map is not a scored battleground.
inline const BgScoreDef* findBgScoreDef(uint32_t mapId) {
    for (const auto& d : kBgScoreDefs) {
        if (d.mapId == mapId) return &d;
    }
    return nullptr;
}

}  // namespace game
}  // namespace wowee
