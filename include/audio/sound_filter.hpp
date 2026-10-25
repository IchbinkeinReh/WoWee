#pragma once

// A sound filter: the effect chain SoundFilter.dbc names and
// SoundFilterElem.dbc builds.
//
// The client keeps a DSP chain per SoundFilter row (0x0055f4d0 finds it by
// name), one unit per SoundFilterElem row in OrderIndex order (0x004cdf70):
// FilterType 1 pitch shift (0x004cd9d0 -> FMOD DSP 11: Params[1] the pitch,
// FFT size 512), 2 high pass (DSP 5: cutoff, resonance), 3 low pass (DSP 3:
// cutoff, resonance), 4 chorus (DSP 12: dry, wet 1-3, delay ms, rate Hz,
// depth, feedback from Params[1..8]) and 5 echo (DSP 6: delay ms, decay
// ratio, dry, wet from Params[1..4]); Params[0] 0 bypasses the unit. A play
// asked to go through one (its play's +0x60 set, the name at +0x64) is
// routed into the chain's channel group (0x004ce250). A Death Knight's
// error speech is the one use: "Death Knight <race> <gender>"
// (0x00986d00 -> 0x004cda20).
//
// Here the chain runs once over a clip's samples rather than live, which
// is the same for a sound played once from its start. The pitch shift is a
// windowed overlap-add of grains read at the pitch's rate, which keeps a
// clip's length as FMOD's does; the rest are the plain forms of FMOD Ex's
// units.

#include <array>
#include <cstdint>
#include <vector>

namespace wowee::audio::sound_filter {

enum class Type : uint32_t { PitchShift = 1, HighPass = 2, LowPass = 3, Chorus = 4, Echo = 5 };

struct Element {
    uint32_t type = 0;
    std::array<float, 9> params{};
};

/// Runs the chain over interleaved samples in place.
void apply(std::vector<float>& samples, uint32_t channels, uint32_t sampleRate,
           const std::vector<Element>& chain);

/// A 16-bit PCM WAV file of interleaved samples, clipped to [-1, 1].
std::vector<uint8_t> encodeWav(const std::vector<float>& samples, uint32_t channels, uint32_t sampleRate);

}  // namespace wowee::audio::sound_filter
