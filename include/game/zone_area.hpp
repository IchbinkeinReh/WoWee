#pragma once

/// Where the player is, as the client names it: the area under the feet, the
/// zone and subzone it splits into, the four texts the interface reads and
/// the event that says they changed (0x0078f020, run every tenth tick by
/// 0x006d7bb0, into 0x005204c0).
///
/// The area is a building's where the player stands in or on one and its
/// WMOAreaTable row names an area - Dalaran's streets are its exterior
/// groups, rows of AreaTable 4395 "Dalaran" and its districts, a long way
/// over the Crystalsong Forest chunks under them - and the ground's
/// otherwise.

#include <cstdint>
#include <optional>
#include <string>

namespace wowee::game::zone_area {

/// What is read of an AreaTable row: its ParentAreaID (+0x8) and its name
/// (+0x2c).
struct AreaInfo {
    uint32_t parent = 0;
    std::string name;
};

/// The area a place is named by (0x0077fa00): straight down from just over
/// it (+0.33) to 1760 yards under it, the building's group when a building is
/// met no further down than the ground (0x007a39f0 for the ground, 0x007a30d0
/// for the buildings, the nearer winning and a tie going to the building) and
/// the group's own WMOAreaTable row names an area (+0x28, looked up exactly -
/// the building's row is not asked); the ground's chunk's area otherwise
/// (0x007a0490, the chunk at the place, not where the line met the ground).
/// `wmoDist`/`groundDist` are how far down each was met, none for not met.
inline uint32_t placeArea(std::optional<float> wmoDist, uint32_t wmoGroupArea,
                          std::optional<float> groundDist, uint32_t groundArea) {
    if (wmoDist && (!groundDist || *wmoDist <= *groundDist) && wmoGroupArea != 0) return wmoGroupArea;
    return groundArea;
}

/// The area the player is in for the sound (0x00782560, which 0x004c6710
/// hands the music and the ambience): the area of the first building group
/// the player is linked to (0x007c2a70: the group of the floor under the
/// feet, unless the ground is nearer) that is not a moving building's, by
/// its exact row; the ground's chunk's otherwise.
inline uint32_t linkedArea(bool linked, uint32_t groupArea, uint32_t groundArea) {
    return linked && groupArea != 0 ? groupArea : groundArea;
}

/// The zone and the subzone an area is (0x0078f020): an area with no parent
/// is a zone and there is no subzone; one with a parent is that parent's
/// subzone. One step up and no further - the client reads the parent's row
/// and goes no higher.
struct Split {
    uint32_t zone = 0;
    uint32_t subzone = 0;
};
template <typename Lookup>
std::optional<Split> split(uint32_t area, Lookup&& lookup) {
    const AreaInfo* row = lookup(area);
    if (!row) return std::nullopt;
    if (row->parent == 0) return Split{area, 0};
    if (!lookup(row->parent)) return std::nullopt;
    return Split{row->parent, area};
}

/// The names of the building the player is in (0x0078ec70), where the player
/// is linked to one: its group's WMOAreaTable row's name (0x007a15b0), and the
/// building's own (0x007a1500) - its row's name, or failing that the name of
/// the area its row gives, or failing that the name of the ground's area.
struct BuildingNames {
    std::string group;
    std::string building;
};

/// The texts (0x005204c0 keeps them: GetZoneText 0x00515570, GetRealZoneText
/// 0x005155a0, GetSubZoneText 0x005155d0, GetMinimapZoneText 0x00515600) and
/// the ids they belong to.
struct Texts {
    uint32_t zoneId = 0;
    uint32_t subzoneId = 0;
    std::string zone;
    std::string subzone;
    std::string realZone;
    std::string minimap;
    bool operator==(const Texts&) const = default;
};

/// The texts for `area` (0x0078f020): the zone's name, and the subzone's when
/// there is one. Inside a building that names itself, the building's names
/// say which part of it the player is in - the group's, or the building's
/// where the group has none: the Lion's Pride Inn is one name on every group
/// of the inn, each of Dalaran's shops a name on its own groups. The real
/// zone is the zone's name whatever the building says. The minimap shows the
/// subzone, or the zone where there is none (0x005134c0). Nothing for an area
/// AreaTable does not have: the client leaves the texts as they were.
template <typename Lookup>
std::optional<Texts> texts(uint32_t area, Lookup&& lookup, const std::optional<BuildingNames>& building) {
    const auto parts = split(area, lookup);
    if (!parts) return std::nullopt;
    Texts out;
    out.zoneId = parts->zone;
    out.subzoneId = parts->subzone;
    out.zone = lookup(parts->zone)->name;
    out.realZone = out.zone;
    if (parts->subzone != 0) out.subzone = lookup(parts->subzone)->name;
    if (building) {
        if (!building->group.empty()) out.subzone = building->group;
        else if (!building->building.empty()) out.subzone = building->building;
    }
    out.minimap = out.subzone.empty() ? out.zone : out.subzone;
    return out;
}

/// The event a change of texts raises (0x005204c0): a new zone id is
/// ZONE_CHANGED_NEW_AREA (0xa8); otherwise a changed zone or subzone text is
/// ZONE_CHANGED_INDOORS (0xa7) where the player is indoors (0x007a1480: the
/// first group linked is not open to the sky) and ZONE_CHANGED (0xa6) where
/// not. The real zone and the minimap's text raise nothing of their own.
enum class Event { None, ZoneChanged, ZoneChangedIndoors, ZoneChangedNewArea };
inline Event event(const Texts& before, const Texts& after, bool indoors) {
    if (before.zoneId != after.zoneId) return Event::ZoneChangedNewArea;
    if (before.zone != after.zone || before.subzone != after.subzone)
        return indoors ? Event::ZoneChangedIndoors : Event::ZoneChanged;
    return Event::None;
}
inline const char* eventName(Event e) {
    switch (e) {
        case Event::ZoneChanged: return "ZONE_CHANGED";
        case Event::ZoneChangedIndoors: return "ZONE_CHANGED_INDOORS";
        case Event::ZoneChangedNewArea: return "ZONE_CHANGED_NEW_AREA";
        case Event::None: break;
    }
    return nullptr;
}

}  // namespace wowee::game::zone_area
