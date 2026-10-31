#include "game/quest_poi_icons.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>

namespace wowee::game {

namespace {

/// The x87 conversion every one of these goes through (0x0088b9c0): toward
/// zero.
int32_t truncToInt(double v) { return static_cast<int32_t>(v); }

/// imul: two's-complement wrap rather than overflow.
int32_t mulWrap(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
}
int32_t addWrap(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}
int32_t subWrap(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}

// The constants 0x005e2eb0 reads.
constexpr float kPerMapUnit = 0.0009765625f;       // 0x00a1c8a0, 1/1024
constexpr float kForceCap = 0.05f;                 // 0x009f1958
constexpr float kAspectIn = 0.7500187754631042f;   // 0x00a1c89c
constexpr float kAspectOut = 1.333299994468689f;   // 0x00a1c894
constexpr float kNoForce = -9999999.0f;            // 0x00a1c898
constexpr float kShiftEnabled = 0.001f;            // 0x009e1134
constexpr float kShiftReach = 100.0f;              // 0x009e8cc8

}  // namespace

bool QuestPoiIconList::areaSelected(const QuestPoiArea& area, uint32_t mask) {
    if (area.objectiveIndex == -1 && mask == 0xffffffffu) return true;
    return mask != 0xffffffffu &&
           (mask & (1u << (static_cast<uint32_t>(area.objectiveIndex) & 0x1fu))) != 0;
}

bool QuestPoiIconList::areaOnMap(const QuestPoiArea& area, const QuestPoiMapView& view) {
    if (!view.valid || !view.project) return false;
    if (view.dungeon) {
        // 0x005e0180: the DungeonMap row's map and floor, then the centre.
        if (area.mapId != view.mapId || area.floor != view.dungeonFloor) return false;
    } else {
        // 0x005e2eb0's flag test, then 0x005e0110: the WorldMapArea row's map.
        if ((area.flags & 4u) != 0 && area.worldMapAreaId != view.worldMapAreaId) return false;
        if (area.mapId != view.mapId) return false;
    }
    float u = 0.0f, v = 0.0f;
    if (!view.project(static_cast<float>(area.centerX), static_cast<float>(area.centerY), u, v)) {
        return false;
    }
    // A point within a thousandth of the top or left edge is off the map.
    return std::fabs(u * v) >= 0.001f;
}

void QuestPoiIconList::receive(int32_t questId, std::vector<QuestPoiArea> areas,
                               const std::function<bool(int32_t)>& inLog) {
    const bool listed = questId > 0 && contains(questId);
    // 0x005e7020: the quest's own slot, or the first holding a quest the log
    // no longer has - emptied as it is found, even when the quest turns out
    // to be listed further on.
    int slot = -1;
    if (questId > 0) {
        for (int s = 0; s < kSlots; ++s) {
            if (icons_[s].questId == questId) { slot = s; break; }
            const int32_t held = icons_[s].questId;
            if (held == 0 || !inLog || !inLog(held)) {
                icons_[s] = QuestPoiIcon{};
                slot = s;
                break;
            }
        }
    }
    if (listed || slot < 0) return;

    QuestPoiIcon& icon = icons_[slot];
    icon = QuestPoiIcon{};
    icon.questId = questId;
    for (auto& area : areas) {
        int32_t sx = 0, sy = 0;
        for (const auto& [px, py] : area.points) {
            sx = addWrap(sx, px);
            sy = addWrap(sy, py);
        }
        const int32_t n = static_cast<int32_t>(area.points.size());
        if (n > 1) {
            sx /= n;
            sy /= n;
        }
        area.centerX = sx;
        area.centerY = sy;
    }
    icon.areas = std::move(areas);
}

void QuestPoiIconList::clear() {
    icons_.fill(QuestPoiIcon{});
}

bool QuestPoiIconList::contains(int32_t questId) const {
    if (questId <= 0) return false;
    return std::any_of(icons_.begin(), icons_.end(),
                       [questId](const QuestPoiIcon& i) { return i.questId == questId; });
}

int QuestPoiIconList::updateVisible(const QuestPoiMapView& view, const QuestState& state) {
    if (!view.valid) return 0;
    int count = 0;
    for (auto& icon : icons_) {
        icon.objectiveMask = state ? state(icon.questId).objectiveMask : 0u;
        icon.visible = false;
        for (const auto& area : icon.areas) {
            if (icon.objectiveMask == 0xffffffffu) {
                if (area.objectiveIndex != -1) continue;
            } else if (area.objectiveIndex == -1 ||
                       (icon.objectiveMask &
                        (1u << (static_cast<uint32_t>(area.objectiveIndex) & 0x1fu))) == 0) {
                continue;
            }
            if (areaOnMap(area, view)) {
                ++count;
                icon.visible = true;
                break;
            }
        }
    }
    return count;
}

void QuestPoiIconList::updateIcons(const QuestPoiMapView& view, const QuestState& state,
                                   float playerX, float playerY, float shiftComplete) {
    if (!view.valid) return;

    // Each slot's area: the lowest priority among those its mask selects that
    // lie on the map, the nearest of equals, none past priority ten.
    std::vector<int> incomplete;  // local_f0
    std::vector<int> complete;    // aiStack_21c
    std::array<bool, kSlots> movable{};
    for (int s = 0; s < kSlots; ++s) {
        QuestPoiIcon& icon = icons_[s];
        float bestDist = FLT_MAX;
        int32_t bestPriority = 10;
        int best = -1;
        for (size_t k = 0; k < icon.areas.size(); ++k) {
            const QuestPoiArea& area = icon.areas[k];
            if (!areaSelected(area, icon.objectiveMask) || !areaOnMap(area, view)) continue;
            const float dx = std::fabs(playerX - static_cast<float>(area.centerX));
            const float dy = std::fabs(playerY - static_cast<float>(area.centerY));
            const float dist = dy * dy + dx * dx;
            if (area.priority < bestPriority ||
                (dist < bestDist && area.priority == bestPriority)) {
                best = static_cast<int>(k);
                bestDist = dist;
                bestPriority = area.priority;
            }
        }
        if (best < 0) {
            icon.mapId = 0;
            icon.objectiveIndex = 0;
            icon.floor = 0;
            icon.x = kNoPosition;
            icon.y = kNoPosition;
            continue;
        }
        const QuestPoiArea& area = icon.areas[static_cast<size_t>(best)];
        icon.mapId = area.mapId;
        icon.objectiveIndex = area.objectiveIndex;
        icon.floor = area.floor;
        icon.x = area.centerX;
        icon.y = area.centerY;
        if (!(state && state(icon.questId).complete)) {
            // An area of a single point is a place, and stays where it is.
            movable[static_cast<size_t>(s)] = area.points.size() != 1;
            incomplete.push_back(s);
        } else {
            complete.push_back(s);
        }
    }

    // A dungeon floor's marks stay where their areas are.
    if (view.dungeon) return;

    // The overlap distance is in 1024ths of the map's width; y is squeezed by
    // the map's aspect while the marks are pushed and stretched back after.
    const int32_t width = truncToInt(std::fabs(static_cast<double>(view.locLeft) -
                                               static_cast<double>(view.locRight)));
    const double reachExt = static_cast<double>(width) *
                            static_cast<double>(overlapDistance_ + overlapDistance_) *
                            static_cast<double>(kPerMapUnit);
    const float reach = static_cast<float>(reachExt);
    const float cap = static_cast<float>(static_cast<double>(kForceCap) * reachExt);
    const float reach2 = static_cast<float>(reachExt * reachExt);

    for (const int s : incomplete) {
        icons_[s].y = truncToInt(static_cast<double>(icons_[s].y) * static_cast<double>(kAspectIn));
    }

    // Marks closer than the reach push each other the full reach apart along
    // the line between them; two on the same spot take the next of three
    // fixed nudges. Twenty rounds, or fewer once nothing moves.
    static constexpr int32_t kNudge[3][2] = {{0, 2}, {1, -2}, {-2, 1}};
    int nudge = 0;
    for (int round = 0; round < kPushIterations; ++round) {
        std::array<std::array<int32_t, 2>, kSlots> force{};
        const size_t n = incomplete.size();
        for (size_t i = 0; i < n; ++i) {
            const int a = incomplete[i];
            for (size_t j = i + 1; j < n; ++j) {
                const int b = incomplete[j];
                int32_t dx = subWrap(icons_[a].x, icons_[b].x);
                int32_t dy = subWrap(icons_[a].y, icons_[b].y);
                const int32_t d2 = addWrap(mulWrap(dy, dy), mulWrap(dx, dx));
                if (!(static_cast<double>(d2) < static_cast<double>(reach2))) continue;
                if (d2 == 0) {
                    dx = kNudge[nudge][0];
                    dy = kNudge[nudge][1];
                    nudge = (nudge + 1) % 3;
                }
                const double len = std::sqrt(static_cast<double>(dx) * dx +
                                             static_cast<double>(dy) * dy);
                const double k = static_cast<double>(reach) / len;
                const int32_t fx = truncToInt(static_cast<double>(dx) * k);
                const int32_t fy = truncToInt(static_cast<double>(dy) * k);
                if (movable[static_cast<size_t>(a)]) {
                    force[a][0] = addWrap(force[a][0], fx);
                    force[a][1] = addWrap(force[a][1], fy);
                }
                if (d2 != 0 && movable[static_cast<size_t>(b)]) {
                    force[b][0] = subWrap(force[b][0], fx);
                    force[b][1] = subWrap(force[b][1], fy);
                }
            }
        }
        double strongest = kNoForce;
        for (const int s : incomplete) {
            strongest = std::max(strongest, std::fabs(static_cast<double>(force[s][0])));
            strongest = std::max(strongest, std::fabs(static_cast<double>(force[s][1])));
        }
        if (strongest == 0.0) break;
        // No mark moves more than a twentieth of the reach in one round.
        if (static_cast<double>(cap) < strongest) {
            const double scale = static_cast<double>(cap) / strongest;
            for (const int s : incomplete) {
                force[s][0] = truncToInt(static_cast<double>(force[s][0]) * scale);
                force[s][1] = truncToInt(static_cast<double>(force[s][1]) * scale);
            }
        }
        for (const int s : incomplete) {
            icons_[s].x = addWrap(icons_[s].x, force[s][0]);
            icons_[s].y = addWrap(icons_[s].y, force[s][1]);
        }
    }

    // Completed quests' marks: each one near an earlier one is stacked on it
    // and set off by the next of four fixed offsets, scaled by the CVar.
    if (kShiftEnabled < shiftComplete) {
        std::array<int32_t, kSlots> depth{};
        const double near = static_cast<double>(shiftComplete) * static_cast<double>(kShiftReach);
        for (size_t k = 1; k < complete.size(); ++k) {
            const QuestPoiIcon& a = icons_[complete[k]];
            for (size_t m = k; m-- > 0;) {
                const QuestPoiIcon& b = icons_[complete[m]];
                const int32_t dx = subWrap(a.x, b.x);
                const int32_t dy = subWrap(a.y, b.y);
                const int32_t d2 = addWrap(mulWrap(dy, dy), mulWrap(dx, dx));
                if (static_cast<double>(d2) < near) {
                    depth[complete[k]] = depth[complete[m]] + 1;
                    break;
                }
            }
        }
        static constexpr int32_t kOffset[4][2] = {{50, 86}, {-86, 50}, {-50, 86}, {-90, 10}};
        int next = 0;
        for (size_t k = 1; k < complete.size(); ++k) {
            const int s = complete[k];
            if (depth[s] == 0) continue;
            const int32_t ox = truncToInt(static_cast<double>(kOffset[next][0]) *
                                          static_cast<double>(shiftComplete));
            const int32_t oy = truncToInt(static_cast<double>(shiftComplete) *
                                          static_cast<double>(kOffset[next][1]));
            next = (next + 1) & 3;
            icons_[s].x = addWrap(icons_[s].x, ox);
            icons_[s].y = addWrap(icons_[s].y, oy);
        }
    }

    for (const int s : incomplete) {
        icons_[s].y = truncToInt(static_cast<double>(icons_[s].y) * static_cast<double>(kAspectOut));
    }
}

int32_t QuestPoiIconList::questIdByVisibleIndex(int index, const QuestState& state,
                                                const std::vector<int32_t>& watched) const {
    if (index < 0 || index >= kSlots) return -1;
    int count = 0;
    const auto settled = [&state](int32_t questId) {
        if (!state) return false;
        const QuestPoiQuestState st = state(questId);
        return st.logIndex > 0 && (st.complete || st.noObjectives);
    };
    const auto open = [&state](int32_t questId) {
        if (!state) return false;
        const QuestPoiQuestState st = state(questId);
        return st.logIndex > 0 && !st.complete && !st.noObjectives;
    };
    // Completed quests, and those with nothing to do, in slot order.
    for (const auto& icon : icons_) {
        if (!icon.visible || !settled(icon.questId)) continue;
        if (count++ == index) return icon.questId;
    }
    // The watched ones still in progress, in watch order.
    for (const int32_t questId : watched) {
        if (!open(questId) || questId <= 0) continue;
        for (const auto& icon : icons_) {
            if (icon.questId != questId) continue;
            if (icon.visible && count++ == index) return icon.questId;
            break;
        }
    }
    // The rest still in progress, in slot order.
    for (const auto& icon : icons_) {
        if (!icon.visible || !open(icon.questId)) continue;
        if (std::find(watched.begin(), watched.end(), icon.questId) != watched.end()) continue;
        if (count++ == index) return icon.questId;
    }
    return -1;
}

int32_t QuestPoiIconList::questIdAtSlot(int slot) const {
    if (slot < 0 || slot >= kSlots) return -1;
    return icons_[static_cast<size_t>(slot)].questId;
}

const QuestPoiIcon* QuestPoiIconList::iconFor(int32_t questId) const {
    for (const auto& icon : icons_) {
        if (icon.questId == questId) return &icon;
    }
    return nullptr;
}

std::pair<uint32_t, uint32_t> QuestPoiIconList::openTargetFor(int32_t questId,
                                                              const QuestPoiMapView& view,
                                                              const QuestState& state,
                                                              float playerX, float playerY) {
    if (questId < 1) return {};
    QuestPoiIcon* found = nullptr;
    for (auto& icon : icons_) {
        if (icon.questId == questId) {
            found = &icon;
            break;
        }
    }
    // 0x005e2b60 asks the map which row is showing first, and only looks at
    // areas once it has one; with no row shown the quest's own areas tell it
    // nothing about where to go.
    if (!found || !view.valid) return {};
    found->objectiveMask = state ? state(questId).objectiveMask : 0u;

    uint32_t bestMap = 0, bestFloor = 0;
    int bestFloorDiff = INT32_MAX;
    float bestDist = FLT_MAX;
    for (const auto& area : found->areas) {
        if (!areaSelected(area, found->objectiveMask)) continue;
        // Already looking at this area's map and floor: that is the answer,
        // and nothing further is weighed.
        if (area.worldMapAreaId == view.worldMapAreaId && area.floor == view.dungeonFloor) {
            return {area.worldMapAreaId, area.floor};
        }
        // The floor the player would have to change, and only then how far
        // the area is from where they stand - both against what is showing.
        const int floorDiff = std::abs(static_cast<int>(view.dungeonFloor) -
                                       static_cast<int>(area.floor));
        const float dx = std::fabs(playerX - static_cast<float>(area.centerX));
        const float dy = std::fabs(playerY - static_cast<float>(area.centerY));
        const float dist = dy * dy + dx * dx;
        if (area.mapId == view.mapId) {
            if (floorDiff < bestFloorDiff || (floorDiff == bestFloorDiff && dist < bestDist)) {
                bestFloorDiff = floorDiff;
                bestDist = dist;
                bestMap = area.worldMapAreaId;
                bestFloor = area.floor;
            }
        } else if (bestMap == 0) {
            // An area on some other world map, kept only while nothing else
            // has been picked - and given the worst score there is, so any
            // area on this map replaces it.
            bestMap = area.worldMapAreaId;
            bestFloor = area.floor;
        }
    }
    return {bestMap, bestFloor};
}

}  // namespace wowee::game
