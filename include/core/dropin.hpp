#pragma once

#include <string>

namespace wowee {
namespace core {

/**
 * Drop-in mode: wowee.exe copied into an original WoW 3.3.5a folder (the one
 * holding Wow.exe and Data\*.MPQ) runs from that folder as it is.
 *
 * Everything this client writes goes into one folder of its own beside the
 * executable, WoWee\ - its working directory from here on - so nothing lands
 * in the folders the original client uses (Cache, Logs, WTF, Interface,
 * Screenshots, Errors; Windows does not tell "cache" from "Cache"). Removing
 * WoWee\ and wowee.exe leaves the install as it was. The game data is read
 * from Data\ in place (MpqArchiveSet); the settings and the rest of the
 * per-user files go to WoWee\config.
 *
 * This client's own files (shaders, its addons, its expansion tables, its
 * pictures) travel inside the executable as a payload appended to it
 * (tools/wowee_pack) and are written into WoWee\ on the first start, and again
 * only when the payload changes.
 *
 * Taken when the executable's folder holds MPQ archives (in it or in its
 * Data\) and no assets\shaders of its own - a development build directory is
 * never one. WOWEE_DROPIN=0 turns it off, WOWEE_DROPIN=1 forces it.
 * WOW_DATA_PATH and WOWEE_CONFIG_ROOT, when already set, are kept.
 *
 * Returns what was decided, for the log (empty when not taken); called before
 * the logger exists.
 */
std::string enterDropInRoot();

/// Writes the payload appended to `exePath` into `root` when its stamp there
/// differs. Returns a line for the log ("" when there is no payload).
std::string unpackPayload(const std::string& exePath, const std::string& root);

} // namespace core
} // namespace wowee
