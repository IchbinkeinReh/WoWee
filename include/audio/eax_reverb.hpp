#pragma once

// The world's reverb: the FMOD/EAX reverb the client sets from
// SoundProviderPreferences.dbc when Sound_EnableReverb is on.
//
// 0x004d2050 asks whether reverb is on (0x008782a0), takes the preset of the
// highest slot that has one (0x004c8590: 10 down to 0, slot 9 only with the
// camera in liquid), fills FMOD_REVERB_PROPERTIES from that row (0x004d1ec0)
// - or the OFF preset for none - and sets it (0x008794e0). The slots:
//   0 the place (0x0078e9a0): its WMO group's or building's SoundProviderPref
//     (WMOAreaTable +0x10), else its area's or zone's (AreaTable +0x14);
//   1 the world state's (WorldStateZoneSounds +0x1c, 0x004cbe70);
//   9 the place's underwater one (WMOAreaTable +0x14, AreaTable +0x18).
//
// FMOD's reverb is its own (an I3DL2 late-reverb network with early
// reflections). This one is a Schroeder-Moorer network - eight damped combs
// and four allpasses a side - tuned from the same properties: Room and
// Reverb (millibels) the late level, ReflectionsDelay and ReverbDelay the
// pre-delay, Reflections an early tap, DecayTime the combs' feedback (an
// RT60), DecayHFRatio their damping, RoomHF the input's high cut and
// EnvSize the network's size. The OFF preset (Room -10000) is silence.

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace wowee::pipeline {
class DBCFile;
}

namespace wowee::audio {

/// The properties of 0x004d1ec0 this reverb reads, in FMOD's units.
struct EaxReverbParams {
    float envSize = 7.5f;            // metres, 1..100
    float envDiffusion = 1.0f;       // 0..1
    int32_t room = -10000;           // mB, the whole effect's level
    int32_t roomHF = -10000;         // mB at HFReference
    float decayTime = 1.0f;          // s, 0.1..20
    float decayHFRatio = 1.0f;       // 0.1..2
    int32_t reflections = -2602;     // mB
    float reflectionsDelay = 0.007f; // s
    int32_t reverb = 200;            // mB, the late level against Room
    float reverbDelay = 0.011f;      // s
    float hfReference = 5000.0f;     // Hz
    bool operator==(const EaxReverbParams&) const = default;
};

/// A SoundProviderPreferences row as 0x004d1ec0 reads it (+0x10 DecayTime,
/// +0x14 EnvSize, +0x18 EnvDiffusion, +0x1c Room, +0x20 RoomHF, +0x24
/// DecayHFRatio, +0x28 Reflections, +0x2c ReflectionsDelay, +0x30 Reverb,
/// +0x34 ReverbDelay, +0x58 HFReference), or none for a row the table does
/// not have.
std::optional<EaxReverbParams> soundProviderPreset(const pipeline::DBCFile& dbc, uint32_t id);

/// The reverb on interleaved float samples.
class EaxReverb {
public:
    void configure(const EaxReverbParams& params, uint32_t sampleRate, uint32_t channels);
    /// Adds the reverb of `samples` to them, in place.
    void process(float* samples, uint32_t frames);
    [[nodiscard]] bool silent() const { return wet_ <= 0.0f && early_ <= 0.0f; }

private:
    struct Comb {
        std::vector<float> buffer;
        size_t index = 0;
        float store = 0.0f;
        float feedback = 0.0f;
    };
    struct Allpass {
        std::vector<float> buffer;
        size_t index = 0;
    };
    struct Side {
        std::array<Comb, 8> combs;
        std::array<Allpass, 4> allpasses;
    };
    std::array<Side, 2> sides_;
    std::vector<float> preDelay_;   // mono send, pre-delay line
    size_t preIndex_ = 0;
    size_t preLength_ = 1;
    size_t earlyLength_ = 1;
    float damp_ = 0.0f;
    float inputLowpass_ = 0.0f;
    float inputCoef_ = 1.0f;
    float wet_ = 0.0f;
    float early_ = 0.0f;
    float diffusion_ = 0.5f;
    uint32_t channels_ = 2;
};

}  // namespace wowee::audio
