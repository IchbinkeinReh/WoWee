#pragma once

#include <cstddef>

// When a transport's doodads are told what its hull's machinery does: as the
// ship sets off or stops, and those that come later, once each.

namespace wowee::game::transport_machinery {

struct Step {
    bool tell = false;
    /// Under way or docked already: the sequence it is in, not the one into it.
    bool steady = false;
    /// The first of the doodads told.
    size_t from = 0;
};

/// `toldState` is the ship sequence told last (below 0: none), to `toldCount`
/// doodads; `state` the one it is in now, with `count` of them.
[[nodiscard]] constexpr Step step(int toldState, size_t toldCount, int state, size_t count) {
    if (toldState != state) return {.tell = true, .steady = toldState < 0 || toldCount == 0};
    if (toldCount == count) return {};
    return {.tell = true, .steady = true, .from = toldCount < count ? toldCount : 0};
}

}  // namespace wowee::game::transport_machinery
