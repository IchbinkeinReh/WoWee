#pragma once

// The pointer's speed while the game has the focus: the CVar mouseSpeed.
//
// The client keeps no pointer of its own - the cursor is the operating
// system's - and mouseSpeed is the system's own pointer speed, set for as long
// as the game window is the active one. Its setter (0x004015e0 -> 0x0086a070)
// clamps the value to 0.1 - 2.0 and makes ten times it the Windows pointer
// speed (SPI_SETMOUSESPEED, 1 - 20); the window procedure (0x0086a2da) applies
// it while the window is active and not minimised and puts the desktop's own
// speed back, saved when the game started, when it is not, and on exit. The
// CVar's default is that desktop speed over ten (0x00869630), so left alone it
// changes nothing.
//
// Only Windows has this: other systems give a program no say over the
// pointer's speed, and there the CVar is kept and does nothing.

namespace wowee::core::pointer_speed {

/// The desktop's own pointer speed as mouseSpeed counts it ("%1.1f" of
/// speed x 0.1): the CVar's default. "1.0" where the system has none to give.
const char* desktopDefault();

/// The CVar's new value (SetCVar, or its stored value at start).
void setMouseSpeed(float mouseSpeed);
/// Whether the game window is the active one: the value applies while it is,
/// the desktop's own speed while it is not.
void setActive(bool active);

/// Puts the desktop's own speed back (on exit).
void restore();

}  // namespace wowee::core::pointer_speed
