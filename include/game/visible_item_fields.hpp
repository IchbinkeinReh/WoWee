#pragma once

#include <cstdint>

namespace wowee {
namespace game {

/// What the PLAYER_VISIBLE_ITEM pair says about a slot of somebody else's gear.
///
/// The pair is two dwords per slot - entry, then enchantment - at blob dwords
/// 135 and 136 of a player's field block: the descriptor records at 0x00ac4800
/// name them PLAYER_VISIBLE_ITEM_1_ENTRYID and PLAYER_VISIBLE_ITEM_1_ENCHANTMENT,
/// the second written as kind 2, two halfwords.  0x006de360 is the only reader
/// of these the client has, and it reads the pair as `block + 0x21c + slot*8` -
/// 0x21c being 135 dwords, the eight being the pair - into a record:
///
///   record+0x08  the entry's absolute value
///   record+0x0c  the enchantment dword's low halfword
///   record+0x10  its high halfword
///   record+0x3e  bit 0, set when the entry field was zero or negative
///
/// A negative entry is not a different item: the magnitude is stored and the
/// flag at +0x3e says it was flagged.  What for is still open - 0x006de4d0
/// requires that flag before it goes looking for an enchantment-provided spell
/// in record+0x14, so it marks a slot holding something the item tables were
/// never meant to name rather than an empty one.  Reading the magnitude is not
/// a guess either way: every consumer of the record sees the magnitude, so a
/// field of -40546 is item 40546 to the paperdoll, the link and the model.
///
/// The two halfwords are the item's own two enchant values.  record+0x0c is
/// ITEM_FIELD_ENCHANTMENT_1_1 and record+0x10 ITEM_FIELD_ENCHANTMENT_2_1 - item
/// blobs 16 and 19, the same two WoWee takes off an item object three dwords
/// apart as permanentEnchantId and temporaryEnchantId.  That is also why the
/// inspect stream's per-item mask counts enchantment *blocks* rather than gems:
/// readInspectEnchantments writes the block values into the very dwords, block
/// 0 at +0x0c, blocks 2..4 being the three socket gems the link asks for.
///
/// WoWee's own field numbering starts where the player block starts, so blob
/// dword 135 is index 283 - which is the base that was found in the field dump
/// ("base=283 gives 17/19 valid item IDs, base=284 reads enchant values
/// instead"), the enchantment column being the one beside it.

/// The entry as the record stores it: the magnitude of the field.
inline uint32_t visibleItemEntryField(uint32_t field) {
    const int32_t signedField = static_cast<int32_t>(field);
    // Negating in 64 bits so the most negative int32 does not come back itself.
    return signedField < 0
               ? static_cast<uint32_t>(-static_cast<int64_t>(signedField))
               : field;
}

/// The permanent enchant, the number an item link carries second.
inline uint32_t visibleItemEnchantField(uint32_t field) {
    return static_cast<uint16_t>(field & 0xFFFFu);
}

/// The temporary enchant - an oil or a sharpening stone.  The record keeps it
/// beside the permanent one, and no item link has a place for it.
inline uint32_t visibleItemTempEnchantField(uint32_t field) {
    return static_cast<uint32_t>(field >> 16);
}

} // namespace game
} // namespace wowee
