#pragma once

#include <cstdint>
#include <optional>
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

constexpr uint32_t kCVarStoredWithAccount = 0x10;
constexpr uint32_t kCVarStoredWithCharacter = 0x20;
constexpr uint32_t kCVarHidden = 0x40;

}  // namespace wowee::addons
