#pragma once

#include <cstdint>

namespace wowee {
namespace game {

/// These two answer for a letter being *read* - the small icon on its inbox row
/// and the paper behind its body - and are written out from the table instead of
/// read from it, so that path needs no table to be loaded first. The compose
/// frame's paper-style popup does read Stationery.dbc by name, and that is where
/// these rows, their item ids and their flag bit come from: see
/// InventoryHandler::stationeryOptions. The ids and pairs below are the whole
/// seven-row table, so the two sides agree as long as the table does.

/// The letterhead a letter is written on, by its Stationery.dbc id.
///
/// FrameXML appends "1" and "2" to this and hangs the two halves behind the
/// mail body: Interface\Stationery\StationeryTest1 and ...2. Answering nil
/// left both halves empty, so every letter was read as dark ink on the black
/// of an empty frame - the font is a dark brown chosen to sit on parchment,
/// and there was no parchment.
///
/// Seven rows, from 3.3.5's own Stationery.dbc, and each name is what the
/// .blp beside it is called. An id not in the table falls back on what kind of
/// mail it is rather than on nothing: a plausible letterhead is right far more
/// often than no letterhead is, and no letterhead is unreadable.
inline const char* stationeryTexture(uint32_t stationeryId, uint8_t messageType) {
    switch (stationeryId) {
        case 1:  case 41: return "StationeryTest";
        case 61:          return "GMStationery";
        case 62:          return "AuctionStationery";
        case 64:          return "Stationery_Val";
        case 65:          return "Stationery_Chr";
        case 67:          return "Stationery_Orp";
        default: break;
    }
    // 2 is an auction house letter, which has its own paper in every client.
    return messageType == 2 ? "AuctionStationery" : "StationeryTest";
}

/// The item a letterhead is sold as, by its Stationery.dbc id, or 0 for an id the
/// table has no row for.
///
/// Not the same picture as above, and the two used to be answered with one
/// letter: `stationeryTexture` is the parchment behind the mail *body*, while this
/// is the small icon on the inbox row.  The client takes the Stationery row's
/// column 1, looks that number up as an *item*, and answers the item's icon
/// (0x0056e520) - which is what makes the value an item id rather than a texture,
/// and rules out feeding the name above through it.
///
/// The same seven rows as above, read out of 3.3.5's own Stationery.dbc, in the
/// table's own order: ids 1, 41, 61, 62, 64, 65, 67 pair with items 8164, 9311,
/// 18154, 21140, 22058, 34171, 46830.  Two rows share a letterhead (ids 1 and 41
/// are both STATIONERYTEST) and still name different items, so the two switches
/// are not the same list and cannot be folded into one.
inline uint32_t stationeryItemId(uint32_t stationeryId) {
    switch (stationeryId) {
        case 1:  return 8164;
        case 41: return 9311;
        case 61: return 18154;
        case 62: return 21140;
        case 64: return 22058;
        case 65: return 34171;
        case 67: return 46830;
    }
    return 0;
}

} // namespace game
} // namespace wowee
