#pragma once

// The world's sound behind a loading screen, as the client has it.
//
// The client's loading screen runs its own loop (0x0040a990, then 0x0040a920
// for each step of the load): it pumps the sound system (0x0087c4b0) and
// draws the screen, and nothing else - the world is not stepped, so no unit,
// model, spell or zone of it asks for a sound until the screen goes. What
// was playing before it came up plays on, and its end (0x00409550) stops the
// last world's music and ambience (0x004c93b0, 0x004c85f0) for the new
// world's to start.
//
// wowee steps the world behind its loading screen - the units are spawned
// and dressed, the models animated, the packets read - so the engine holds
// what that stepping asks for instead: a sound played once is not played,
// since its moment has passed by the time the world is shown; a looping one,
// or a stream, is made but waits silent, and starts when the hold ends, as
// the client's would on the world's first step.

#include <cstdint>

namespace wowee::audio::world_sound_hold {

/// What becomes of a sound asked for.
enum class Start : uint8_t {
    Now,       ///< no hold: it plays
    Deferred,  ///< held, and it loops (or streams): it waits for the hold's end
    Refused,   ///< held, and it plays once: it is not played
};

constexpr Start start(bool held, bool loops) {
    if (!held) return Start::Now;
    return loops ? Start::Deferred : Start::Refused;
}

/// The holds, counted: each loading screen takes one and lets it go.
struct Count {
    int holds = 0;
    void begin() { ++holds; }
    /// True when this was the last hold, and what waits should start.
    bool end() {
        if (holds <= 0) return false;
        return --holds == 0;
    }
    [[nodiscard]] bool held() const { return holds > 0; }
};

}  // namespace wowee::audio::world_sound_hold
