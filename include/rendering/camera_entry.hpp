#pragma once

// The camera on entering a world.
//
// The client keeps one camera from the login to the logout. It writes
// cameraSavedDistance and cameraSavedPitch with the character's own CVars
// (0x005ff320, from 0x00512890 for AccountData 0x20) and sets itself by them
// as those are read back (0x005ff3e0, from 0x00518bf0 for 0x20) - at the login
// and not on a map change, over which it stands as the player left it.

namespace wowee::rendering {

/// Yards from the character, and degrees down.
struct CameraStand {
    float distance = 5.55f;
    float pitchDown = 10.0f;
};

/// `kept` is the stand before the load; `stored` reads the CVars.
template <typename Stored>
[[nodiscard]] CameraStand cameraStandOnEntry(bool mapChange, CameraStand kept, Stored&& stored) {
    return mapChange ? kept : stored();
}

}  // namespace wowee::rendering
