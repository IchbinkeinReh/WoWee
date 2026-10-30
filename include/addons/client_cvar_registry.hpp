#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace wowee::addons {

/// The flags the client registers a CVar with (0x00767fc0, kept at the CVar's
/// +0x1c), or none when the client registers no CVar by that name. Matched
/// case-insensitively, as the client's CVar lookup (0x00767460) matches.
///
/// The bits GetCVarInfo and its neighbours read: 0x10 stored with the account
/// on the server, 0x20 stored with the character, 0x40 hidden - a hidden CVar
/// is answered as though it were not there at all.
std::optional<uint32_t> clientCVarFlags(std::string_view name);

/// Every name the client registers by literal, spelled as it spells it, in the
/// table's own case-insensitive (that is, alphabetical) order.
///
/// The client's `cvarlist` (0x00767220) does not print in that order: it walks
/// its own registered list, which is in the order the cvars were registered at
/// start-up. Reproducing that would mean reading the client's whole init order
/// out of the binary, and the listing is a debug dump whose order nobody reads,
/// so this client prints alphabetically instead.
///
/// Not in it are the names the client builds in a loop: the eight camera views'
/// Distance/Pitch/Yaw (0x005fd910) and the per-motion camera smoothing knobs.
/// `clientCVarFlags` matches those by prefix, and listing them means inventing
/// the names rather than reading them, so they stay out.
std::span<const std::string_view> clientCVarNames();

constexpr uint32_t kCVarStoredWithAccount = 0x10;
constexpr uint32_t kCVarStoredWithCharacter = 0x20;
constexpr uint32_t kCVarHidden = 0x40;

/// Bit 2: the client will not write a CVar registered this way.
///
/// SetCVar answers `"%s" is read-only` (0x00514c10, which treats bit 8 - 0x100 -
/// the same way), and the two console setters skip the write on the `>> 2 & 1`
/// test of 0x00766990 and 0x00766a20. Of the names the client registers by
/// literal, one carries it: `timingTestError`.
constexpr uint32_t kCVarReadOnly = 0x4;

}  // namespace wowee::addons
