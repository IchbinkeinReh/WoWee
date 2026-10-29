#pragma once

// The half of a SoundEntries row that its own columns do not tell.
//
// A row's column 29 names one of the 1641 SoundEntriesAdvanced rows, by that
// table's own column 0 (its column 1 is the SoundEntries id back, and 1615 of the
// 1617 rows that name a kit agree the two ways round). 1617 of the 12941 rows name
// one - 1395 of them scripted speech (SoundType 10: boss and quest dialogue), and all 7
// liquid ambience loops -
// and the client plays those differently: 0x004c6a40 hands the play to 0x004cb580
// (SoundInterface2AdvancedKitProperties.cpp) and returns from there, *before* it
// sets the reach from the row's MinDistance and DistanceCutoff (004c0000.c:5954-5957
// against :5978-5984). So a row with a kit is not heard at the reach its own columns
// give it.
//
// One rule of that branch is settled, and is what this asks:
//
//   a play that has a position is refused while another play of the same advanced
//   kit is still going within sqrt(6) yards of that position.
//
// 0x004cb580 calls 0x004ca130 with the kit record and the position (:9614-9617);
// that walks the plays still going, comparing the kit record's own id - held at the
// instance's +0x1c, where 0x004cb580 stores the record - and dx^2+dy^2+dz^2 < 6.0
// against the position the instance was started at, +0x28..+0x30 (:9715-9724), and a
// match makes the play return 0xf (:8287-8316). Nothing else about the ask matters:
// the only guard on the check is that the play has a position and that the seventh
// argument of 0x004c6a40 is non-zero, and it is 1 at every one of the 15 call sites
// of that routine in the decompile.
//
// What is *not* acted on here, and should not be until it is understood: the rest of
// the kit record. Its columns 15/16 - 200.0 in 1304 of the 1617, and equal to the
// row's own DistanceCutoff in 1287 of them - look like a reach, and columns 19/20 -
// 180.0 and 270.0, stored as min(col19, col20) at the instance's +0x8c - look like a
// cone, but which part of the play each of them steers is not established: the object
// 0x004cb580 fills is 0xac bytes and its consumer has not been read. Until then the
// row keeps its own MinDistance and DistanceCutoff, which is the reach 0x004c6a40
// would have given it, and the only change here is that a kit already sounding nearby
// is not sounded again.

#include <cstdint>
#include <glm/vec3.hpp>

namespace wowee {
namespace pipeline {
class AssetManager;
}

namespace audio::advanced_kit {

/// The kit a SoundEntries row names, or 0 when the row names none or names one the
/// table does not have (a row whose kit is missing takes neither branch: the client's
/// resolved pointer is null). Reads both tables once, through `assets`.
uint32_t kitIdFor(uint32_t soundEntriesId, pipeline::AssetManager* assets);

/// Whether a play of `kitId` at `at` is turned away, because a play of that kit
/// started earlier is still going within sqrt(6) yards (0x004ca130). Kit 0 - a row
/// with no kit - is never turned away.
bool refusedNear(uint32_t kitId, const glm::vec3& at);

/// Remember that the play `handle` started, so that the next ask near it is refused
/// while it is going. Pass the position the play went to, and handle 0 for a play
/// that did not start, which is not remembered.
void notePlay(uint32_t kitId, const glm::vec3& at, uint32_t handle);

}  // namespace audio::advanced_kit
}  // namespace wowee
