#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace wowee::game {

/// One area of a quest's points of interest, as SMSG_QUEST_POI_QUERY_RESPONSE
/// carries it and 0x005e7370 stores it: 0x34 bytes in the client.
struct QuestPoiArea {
    uint32_t poiId = 0;           ///< +0x18
    int32_t objectiveIndex = -1;  ///< +0x1c, -1 for the quest's hand-in
    uint32_t mapId = 0;           ///< +0x20
    uint32_t worldMapAreaId = 0;  ///< +0x24
    uint32_t floor = 0;           ///< +0x28
    uint32_t flags = 0;           ///< +0x2c, the seventh word (bit 4: only on its own WorldMapArea)
    int32_t priority = 0;         ///< +0x30, the sixth word; lower is preferred
    /// +0x10/+0x14: the points' mean, each axis summed and divided by the
    /// count as ints (one point is itself). Server coordinates.
    int32_t centerX = 0;
    int32_t centerY = 0;
    std::vector<std::pair<int32_t, int32_t>> points;  ///< +0x04 count, +0x08 the pairs
};

/// One entry of the client's 25-entry POI icon list (0x00c23b00, 0x30 bytes).
struct QuestPoiIcon {
    int32_t questId = 0;              ///< +0x00
    std::vector<QuestPoiArea> areas;  ///< +0x08 count, +0x0c the areas
    uint32_t mapId = 0;               ///< +0x14, the chosen area's
    int32_t objectiveIndex = 0;       ///< +0x18
    uint32_t floor = 0;               ///< +0x1c
    int32_t x = 0;                    ///< +0x20, server coordinates, pushed apart
    int32_t y = 0;                    ///< +0x24
    bool visible = false;             ///< +0x28, an area on the map shown
    /// +0x2c, 0x005e2950: 0xffffffff once the quest is complete, else one bit
    /// per objective still open (0-3 kills, 4-9 items, 10-13 source items,
    /// 16 exploration).
    uint32_t objectiveMask = 0;
};

/// What the list needs to know about one quest, from the quest log.
struct QuestPoiQuestState {
    int logIndex = 0;          ///< 0x005deeb0, 1-based, 0 when not in the log
    /// 0x005e0ea0(index, 1): complete, and not a quest without objectives.
    bool complete = false;
    /// 0x005e10b0: a quest with nothing to do but hand it in.
    bool noObjectives = false;
    uint32_t objectiveMask = 0;  ///< 0x005e2950
};

/// The map the list is laid out on: the world map's shown WorldMapArea row,
/// or its DungeonMap floor.
struct QuestPoiMapView {
    bool valid = false;
    /// QuestMapUpdateAllQuests counts only for a zone or a dungeon floor
    /// (0x005e63d0), not for a continent.
    bool zoneOrFloor = false;
    bool dungeon = false;           ///< a DungeonMap row is shown (0x00ad36f4)
    uint32_t worldMapAreaId = 0;    ///< WorldMapArea +0x00
    uint32_t mapId = 0;             ///< WorldMapArea +0x04, DungeonMap +0x04
    uint32_t dungeonFloor = 0;      ///< DungeonMap +0x08
    float locLeft = 0.0f;           ///< WorldMapArea +0x10
    float locRight = 0.0f;          ///< WorldMapArea +0x14
    /// 0x00544140: a server-coordinate point onto the shown map, 0..1 across
    /// it. False when it falls off the map.
    std::function<bool(float x, float y, float& u, float& v)> project;
};

/// The client's quest POI icon list (CGQuestLog, 0x00c23b00..0x00c23fb0): up to
/// 25 quests' areas, the one area of each the map marks, and where that mark
/// goes once the marks are pushed apart.
class QuestPoiIconList {
public:
    static constexpr int kSlots = 25;
    using QuestState = std::function<QuestPoiQuestState(int32_t questId)>;

    /// One quest of SMSG_QUEST_POI_QUERY_RESPONSE (0x005e7370). A quest
    /// already listed keeps what it has; otherwise it takes the slot
    /// 0x005e7020 gives it - the first whose quest `inLog` no longer holds -
    /// and with none free the areas are dropped.
    void receive(int32_t questId, std::vector<QuestPoiArea> areas,
                 const std::function<bool(int32_t)>& inLog);

    /// Logout (0x005e70d0): every slot emptied.
    void clear();

    [[nodiscard]] bool contains(int32_t questId) const;

    /// QuestMapUpdateAllQuests's count (0x005e5a50): every slot's objective
    /// mask refreshed, and marked visible when one of the areas its mask
    /// selects lies on the map.
    int updateVisible(const QuestPoiMapView& view, const QuestState& state);

    /// QuestPOIUpdateIcons (0x005e2eb0): each slot's area chosen and its mark
    /// placed, then the marks of quests still in progress pushed apart and
    /// those of completed ones fanned out. `playerX/Y` in server coordinates,
    /// `shiftComplete` the CVar POIShiftComplete.
    void updateIcons(const QuestPoiMapView& view, const QuestState& state,
                     float playerX, float playerY, float shiftComplete);

    /// QuestPOIGetQuestIDByVisibleIndex (0x005e3840), 0-based: completed and
    /// objective-less quests first, then watched ones in watch order, then the
    /// rest. -1 past the end.
    [[nodiscard]] int32_t questIdByVisibleIndex(int index, const QuestState& state,
                                                const std::vector<int32_t>& watched) const;

    /// QuestPOIGetQuestIDByIndex's slot (0-based): the quest id, or -1 for an
    /// index outside the list.
    [[nodiscard]] int32_t questIdAtSlot(int slot) const;

    /// The icon of a quest, or null (0x005deee0).
    [[nodiscard]] const QuestPoiIcon* iconFor(int32_t questId) const;

    [[nodiscard]] const std::array<QuestPoiIcon, kSlots>& icons() const { return icons_; }

    /// SetPOIIconOverlapDistance (0x00ad1014) and ...PushDistance (0x00ad1018,
    /// taken only when not below the overlap distance).
    void setOverlapDistance(float d) { overlapDistance_ = d; }
    void setOverlapPushDistance(float d) {
        if (d >= overlapDistance_) overlapPushDistance_ = d;
    }
    [[nodiscard]] float overlapDistance() const { return overlapDistance_; }
    [[nodiscard]] float overlapPushDistance() const { return overlapPushDistance_; }

    /// The mark's distance threshold: a map is 1024 of these across.
    static constexpr int kPushIterations = 20;  ///< 0x00ad1264
    /// No position: a slot with nothing to mark (0x005e31a0).
    static constexpr int32_t kNoPosition = 999999;

private:
    /// 0x005e0110 / 0x005e0180: whether an area's centre is on the map shown.
    static bool areaOnMap(const QuestPoiArea& area, const QuestPoiMapView& view);
    /// The mask test both 0x005e5a50 and 0x005e2eb0 put each area through.
    static bool areaSelected(const QuestPoiArea& area, uint32_t mask);

    std::array<QuestPoiIcon, kSlots> icons_{};
    float overlapDistance_ = 12.0f;
    float overlapPushDistance_ = 18.0f;
};

}  // namespace wowee::game
