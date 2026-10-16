#pragma once

// A rider held back until its mount is drawn.
//
// 0x0073d5d0 hangs the rider's model on its mount's (0x00831630, attachment
// 0) and gives the unit the mount as its model (0x00717910), whose loaded
// callback (0x008251b0 -> 0x00743110, the unit's virtual +0x80) runs once the
// mount and everything hung on it are complete (0x00825170, 0x00824f00). A
// unit that comes into the world riding is therefore never seen on foot: it
// is drawn on its mount or not at all. This client loads the mount after the
// rider (EntitySpawner::processPendingRemotePlayerMounts), so the rider is
// kept out of the world until its mount is there - or, so that a mount that
// never loads cannot hide a unit for ever, until kTimeoutSeconds have gone.

#include <cstdint>

namespace wowee::rendering::rider_mount_wait {

/// How long a rider waits for its mount before it is shown without it.
inline constexpr float kTimeoutSeconds = 4.0f;

struct Input {
    /// UNIT_FIELD_MOUNTDISPLAYID now; 0 on foot.
    uint32_t wantedMount = 0;
    /// The display of the mount model drawn under it, 0 for none.
    uint32_t drawnMount = 0;
    /// Whether the unit has been drawn already, on foot or on another
    /// mount. What is on screen stays until the new mount is ready.
    bool revealed = false;
    /// How long it has waited for its mount.
    float waitedSeconds = 0.0f;
};

enum class Decision {
    Show,          ///< Draw it as it is.
    Hold,          ///< Keep it, its name, shadow and circle out of the world.
    ShowTimedOut,  ///< The mount has not come: draw it anyway.
};

inline Decision decide(const Input& in) {
    if (in.wantedMount == 0) return Decision::Show;            // on foot, or got down
    if (in.drawnMount == in.wantedMount) return Decision::Show;  // on its mount
    if (in.revealed) return Decision::Show;                      // as it was, until the new one
    if (in.waitedSeconds >= kTimeoutSeconds) return Decision::ShowTimedOut;
    return Decision::Hold;
}

inline bool shown(Decision d) { return d != Decision::Hold; }

}  // namespace wowee::rendering::rider_mount_wait
