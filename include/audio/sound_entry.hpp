#pragma once

// What a SoundEntries row asks of the play of its kit.
//
// The client keeps these rules in the one routine that plays a kit row
// (0x004c6a40), and that routine is what the whole client funnels its sound ids
// through: 99 call sites over 18 files of the decompile, from the model event
// blocks and the spells to the voice and the emitters. A path of ours that plays
// a row and skips part of this is playing less than the row asked for.
//
// The record's Flags (column 25, the record's +0x3c):
//   0x200  the kit loops;
//   0x400  the play's pitch is drawn across 0.85..1.15 (004c0000.c:6020-6025, the
//          bounds set in front of the draw; 0x00878760 multiplies the draw onto
//          the sound's base pitch), so the same clip is never heard the same way
//          twice;
//   0x800  the play's volume moves by -0.15..+0.15 (:6009-6014), and the client's
//          volume setter 0x00879710 clamps what it is asked into [0,1] - which
//          nothing here does to a per-play volume, so the clamp goes with the
//          swing.
// Neither swing is drawn when the ask brings its own volume in the (0,1] range:
// the client swings only the branch that uses the row's VolumeFloat
// (:6005-6018), and every play of ours is on that branch.
//
// What the swings are for is foley, not music. Of the 12941 rows 1218 loop, 126
// ask for a pitch and 7 of those also for a volume. By their names, 61 of the 126
// are footfalls and death thuds: 16 with Foot in the name (FootstepHorse<Surface>,
// FootStepsHuge<Surface>, FootstepsGiantAll, FootstepsMediumLArgeMud,
// MassiveFootsteps, MurlocFootsteps, CrabFootSteps, FlyingMountFootstepDirt), 16
// named Character<Surface> (MediumLarge / Small / Giant across Dirt, Grass, Snow,
// Stone, Wood), and 29 DeathThud*; 35 of them name a Foot*.wav among their files.
// The other 65 are trap and encounter one-offs (AhnQirajSandTrap*, G_BearTrap*,
// ShadowmoonMeteor*, Ulduar_*, XT-002 FistImpacts, PhoenixWingFlaps,
// DruidMorph_Impact_Base, Coilfang_pump_Pistons, Catapult_Plague_Barrel_*). Four
// of the seven volume rows are the Ebon Hold crowd screams (CrowdScream1..4,
// flags 0xc00), two are BoneGolem_Cleave_Impact_Chest and one is
// ManScream2 (20/100). Exactly one row of all 12941 asks for a pitch swing and a
// loop together: 15582 Lightning_Fel_Precast_Low_Hand (IronCouncil), flags 0x600.

#include <algorithm>
#include <cstdint>

namespace wowee::audio::sound_entry {

inline constexpr uint32_t kFlagLoops = 0x200;
inline constexpr uint32_t kFlagPitchSwing = 0x400;
inline constexpr uint32_t kFlagVolumeSwing = 0x800;

/// The pitch the row asks of the play: 1.0 unless it asks to be swung, and then
/// the client's own 0.85..1.15. `draw` is uniform in [0,1].
constexpr float pitchForFlags(uint32_t flags, float draw) {
    return (flags & kFlagPitchSwing) != 0 ? 0.85f + 0.3f * draw : 1.0f;
}

/// The volume the row asks of the play, swung if it asks and then clamped the way
/// the client's volume setter clamps. `draw` is uniform in [0,1].
constexpr float volumeForFlags(uint32_t flags, float volume, float draw) {
    if ((flags & kFlagVolumeSwing) == 0) return volume;
    return std::clamp(volume + (-0.15f + 0.3f * draw), 0.0f, 1.0f);
}

}  // namespace wowee::audio::sound_entry
