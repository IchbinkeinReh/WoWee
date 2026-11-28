#pragma once

// What the MCP tools say of a game object's update fields, of its template,
// of a unit the realm shows a game master alone, and of the building group
// the player stands in. Pure, so tests/test_mcp_server.cpp can check it.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "mcp/object_kind.hpp"

namespace wowee::mcp {

namespace object_fields {

using json = nlohmann::json;

inline std::string hex(uint32_t v) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%X", v);
    return text;
}

/// GAMEOBJECT_FLAGS by name (3.3.5's GO_FLAG_*).
inline json flagNames(uint32_t flags) {
    json out = json::array();
    static constexpr std::array<std::pair<uint32_t, const char*>, 9> kNames = {{{0x1, "in_use"},
                                                                                {0x2, "locked"},
                                                                                {0x4, "interact_cond"},
                                                                                {0x8, "transport"},
                                                                                {0x10, "not_selectable"},
                                                                                {0x20, "no_despawn"},
                                                                                {0x40, "triggered"},
                                                                                {0x200, "damaged"},
                                                                                {0x400, "destroyed"}}};
    for (const auto& [bit, name] : kNames)
        if (flags & bit) out.push_back(name);
    return out;
}

/// GAMEOBJECT_DYNAMIC's low half by name (GO_DYNFLAG_LO_*).
inline json dynamicNames(uint32_t dynamic) {
    json out = json::array();
    static constexpr std::array<std::pair<uint32_t, const char*>, 5> kNames = {
        {{0x1, "activate"}, {0x2, "animate"}, {0x4, "no_interact"}, {0x8, "sparkle"}, {0x10, "stopped"}}};
    for (const auto& [bit, name] : kNames)
        if (dynamic & bit) out.push_back(name);
    return out;
}

/// The fields as the client has them; each only where the realm sent it.
struct Fields {
    bool hasFlags = false;
    uint32_t flags = 0;
    bool hasBytes1 = false;
    uint32_t bytes1 = 0;
    bool hasDynamic = false;
    uint32_t dynamic = 0;
    bool hasFaction = false;
    uint32_t faction = 0;
    bool hasLevel = false;
    uint32_t level = 0;
    bool hasScale = false;
    uint32_t scaleBits = 0;  ///< OBJECT_FIELD_SCALE_X, a float
};

/// GAMEOBJECT_BYTES_1: state, type, art kit, animation progress, one byte
/// each from the low end.
inline json fieldsJson(const Fields& f) {
    json out = json::object();
    if (f.hasFlags) {
        out["flags"] = hex(f.flags);
        out["flag_names"] = flagNames(f.flags);
    }
    if (f.hasBytes1) {
        out["state"] = f.bytes1 & 0xFFu;
        out["type"] = (f.bytes1 >> 8) & 0xFFu;
        out["type_name"] = objectKind((f.bytes1 >> 8) & 0xFFu);
        out["art_kit"] = (f.bytes1 >> 16) & 0xFFu;
        out["anim_progress"] = (f.bytes1 >> 24) & 0xFFu;
    }
    if (f.hasDynamic) {
        out["dynamic"] = hex(f.dynamic);
        out["dynamic_names"] = dynamicNames(f.dynamic & 0xFFFFu);
        if (f.dynamic >> 16) out["path_progress"] = f.dynamic >> 16;
    }
    if (f.hasFaction) out["faction"] = f.faction;
    if (f.hasLevel) out["level"] = f.level;
    if (f.hasScale) {
        float scale = 0.0f;
        std::memcpy(&scale, &f.scaleBits, sizeof(scale));
        out["scale"] = std::round(static_cast<double>(scale) * 1000.0) / 1000.0;
    }
    return out;
}

/// The template the realm answered the query with: its type, the data words
/// up to the last that is not zero, its size and quest items.
inline json templateJson(uint32_t type, const uint32_t (&data)[24], bool hasData, float size, uint32_t displayId,
                         const std::array<uint32_t, 6>& questItems) {
    json out = {{"type", type}, {"type_name", objectKind(type)}, {"display", displayId}};
    if (hasData) {
        size_t last = 0;
        for (size_t i = 0; i < 24; ++i)
            if (data[i] != 0) last = i + 1;
        json words = json::array();
        for (size_t i = 0; i < last; ++i) words.push_back(data[i]);
        out["data"] = std::move(words);
    }
    out["size"] = std::round(static_cast<double>(size) * 1000.0) / 1000.0;
    json items = json::array();
    for (uint32_t item : questItems)
        if (item != 0) items.push_back(item);
    if (!items.empty()) out["quest_items"] = std::move(items);
    return out;
}

/// What tells that the realm shows a unit to the player only because the
/// player is a game master (PLAYER_FLAGS 0x8). The client is not told so;
/// these are the realm's own rules (TrinityCore / AzerothCore):
/// - a trigger creature is sent to a GM with its visible model, 17519 where
///   it has none, and to others with an invisible one;
/// - a stealthed (aura 16) or invisible (aura 18) unit is sent to a GM
///   whatever the player detects;
/// - a spirit healer or guide (npc flags 0x4000 / 0x8000) is seen by the dead
///   alone.
struct GmSight {
    bool playerIsGm = false;
    bool playerDeadOrGhost = false;
    uint32_t displayId = 0;
    uint32_t npcFlags = 0;
    bool stealthAura = false;
    bool invisibilityAura = false;
};

inline constexpr uint32_t kGmTriggerDisplay = 17519;
inline constexpr uint32_t kNpcFlagSpiritHealer = 0x4000;
inline constexpr uint32_t kNpcFlagSpiritGuide = 0x8000;

/// Empty where nothing says so (or the player is no GM).
inline json gmOnlyReasons(const GmSight& s) {
    json out = json::array();
    if (!s.playerIsGm) return out;
    if (s.displayId == kGmTriggerDisplay) out.push_back("trigger (display 17519, sent to a GM only)");
    if (s.stealthAura) out.push_back("stealthed (aura 16)");
    if (s.invisibilityAura) out.push_back("invisible (aura 18)");
    if ((s.npcFlags & (kNpcFlagSpiritHealer | kNpcFlagSpiritGuide)) && !s.playerDeadOrGhost)
        out.push_back("spirit healer/guide while the player lives");
    return out;
}

/// The building group the player stands in, as IsIndoors reads it
/// (0x0077fbf0), for `state`.
struct PlaceIn {
    bool known = false;
    bool indoors = false;
    bool inGroup = false;
    uint32_t wmoId = 0;
    uint32_t instanceId = 0;
    uint32_t group = 0;
    uint32_t groupFlags = 0;  ///< MOGI
    bool hasRow = false;
    uint32_t rowFlags = 0;  ///< WMOAreaTable
    uint32_t groundArea = 0;
};

inline json placeJson(const PlaceIn& p) {
    if (!p.known) return json(nullptr);
    json out = {{"indoors", p.indoors}};
    if (p.inGroup) {
        out["wmo"] = p.wmoId;
        out["instance"] = p.instanceId;
        out["group"] = p.group;
        out["group_flags"] = hex(p.groupFlags);
        // 0x8 exterior, 0x40 exterior lit, 0x2000 interior.
        out["exterior"] = (p.groupFlags & 0x8u) != 0;
    } else {
        out["on_ground"] = true;
    }
    out["ground_area"] = p.groundArea;
    if (p.hasRow) out["row_flags"] = hex(p.rowFlags);
    return out;
}

}  // namespace object_fields

}  // namespace wowee::mcp
