#pragma once

#include <cstdint>

namespace wowee::game {

/// Whether the client acts on a cast's delay or a channel's start or update
/// at all - moving its end, setting or ending the channel, and raising
/// UNIT_SPELLCAST_DELAYED or UNIT_SPELLCAST_CHANNEL_* - for a spell with
/// these Spell.dbc words (SMSG_SPELL_DELAYED 0x00801b80, MSG_CHANNEL_START
/// 0x00801c90, MSG_CHANNEL_UPDATE 0x00801db0):
///   AttributesEx4 (+0x20) 0x8000000 shows it whatever else it carries;
///   Attributes (+0x10) 0x2 hides it;
///   else the active player's own cast is shown, and another unit's only
///   without Attributes 0x180, AttributesEx4 0x1 and - for a channel -
///   AttributesEx3 (+0x1c) 0x2000.
constexpr bool castEventShown(uint32_t attributes, uint32_t attributesEx3, uint32_t attributesEx4,
                              bool casterIsActivePlayer, bool channel) {
    if ((attributesEx4 & 0x8000000u) != 0) return true;
    if ((attributes & 0x2u) != 0) return false;
    if (casterIsActivePlayer) return true;
    if ((attributes & 0x180u) != 0) return false;
    if (channel && (attributesEx3 & 0x2000u) != 0) return false;
    return (attributesEx4 & 0x1u) == 0;
}

}  // namespace wowee::game
