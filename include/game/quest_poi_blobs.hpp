#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace wowee::game::quest_blobs {

/// The world map's quest blob settings (WorldMapBlobFrame, a QuestPOIFrame):
/// EnableSmoothing, EnableMerging, SetMergeThreshold (0.1..0.5) and
/// SetNumSplinePoints (8..30), at the client's starting values (0x0058ffe0).
struct Settings {
    bool smoothing = true;        ///< +0x2ac
    bool merging = true;          ///< +0x2ad
    float mergeThreshold = 0.25f; ///< +0x2b0
    int splinePoints = 20;        ///< +0x2b4
};
Settings& settings();

/// 0x0058ed80's smoothing: the area's points through a closed Catmull-Rom
/// spline (0x00ac37b8), `samples` points from t = 0.01 placed by arc length
/// (0x004c4230), each segment's length the sum of twenty chords (0x004c3b10),
/// rounded to whole units.
std::vector<std::pair<int32_t, int32_t>> smoothArea(const std::vector<std::pair<int32_t, int32_t>>& in,
                                                    int samples);

/// 0x009830d0 with two axes: Haines' crossings test.
bool pointInPolygon(float x, float y, const std::vector<std::pair<float, float>>& pts);

/// One blob as 0x0058f1a0 keeps it: its outline, whether it is shown, and
/// the objectives it carries (its own, then any merged into it, up to four).
struct Blob {
    int32_t objectiveIndex = -1;
    std::vector<std::pair<float, float>> pts;
    float minU = 0, minV = 0, maxU = 0, maxV = 0;
    bool shown = false;
    std::vector<int32_t> objectives;
    void bound();
};

/// Whether the objective a blob is for is still open under the quest's mask
/// (0x005e2950's: 0xffffffff once complete).
bool objectiveOpen(uint32_t mask, int32_t objectiveIndex);

/// 0x0058e310: with merging on, for a quest still in progress, each pair of
/// shown blobs of open objectives whose bounds overlap - the smaller hidden,
/// its objectives the larger's, when more than the threshold of its points
/// lie inside the larger. Any affine view of the points gives the same answer.
void merge(std::vector<Blob>& blobs, uint32_t mask);

}  // namespace wowee::game::quest_blobs
