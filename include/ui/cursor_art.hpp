#pragma once

/**
 * cursor_art.hpp - the client's own named cursors.
 *
 * SetCursor takes either one of a fixed list of names or a texture path. The
 * names are not the file names: the binding (0x005104a0) walks a table of
 * (number, name) pairs, hands the number to the setter (0x00616800), and the
 * number indexes art the client loaded once at startup - the strings at
 * 0x00ad2808 formatted into `Interface\Cursor\%s.blp`, each loaded into a
 * 0x1000-byte image slot of the *same* index (0x006163b0), which the setter then
 * copies out (`&DAT_00c26df0 + kind * 0x1000` at 0x006162c0). So the kind is the
 * index into both arrays: kind 1 is Point.blp, kind 0 has no art at all, and
 * kind 0x35 is the slot the custom-path setter (0x00616830) writes its own
 * picture into.
 *
 * Reading the kinds off the decompile needs care. Ghidra prints that pair table
 * name-first, number after it - but the binding dereferences the number at
 * `-0x1a0 + i*8` and the name it compares at `-0x1a0 + i*8 + 4` (0x005108f9), so
 * the number belongs to the name *after* it, not the one before: POINT_CURSOR is
 * 1, not the 2 printed beside it. What is printed beside CAST_CURSOR is what
 * Point's index is. Off by one here is invisible for most of the table - it
 * shows up as a cursor one entry along, which still looks like a cursor.
 *
 * Two names do not read as their art at all: GATHER_CURSOR's file is
 * GatherHerbs.blp and LOCK_CURSOR's is PickLock.blp, and VEHICLE_CURSOR's is
 * spelled vehichleCursor.blp because the art file is. So a name cannot be turned
 * into a file name by lowercasing either.
 *
 * The same numbers are what the client's own hover code passes, including the
 * "can't" half: the hovered-unit code at 0x004f0000 computes `base + 26` for a
 * thing too far away to act on - 18 with 44 for a repair NPC, 22 with 48 for an
 * innkeeper, 8 with 34 for a merchant - which only lines up as
 * RepairNPC/UnableRepairNPC, Innkeeper/UnableInnkeeper and Pickup/UnablePickup
 * when the kind is the art index. That is also the answer for the sell cursor
 * the merchant windows ask for (0x005856d0 hands the setter 3 or 29): Buy.blp
 * and UnableBuy.blp, the coin bag, not the cast swirl.
 */

#include <cstddef>
#include <string>

namespace wowee {
namespace ui {

/// No cursor of the script's: the pointer is whatever hovering the world makes
/// it. The reference has no such value - it keeps one kind, starts it at 1
/// (Point) and restores that on ResetCursor - and 0 is how this client says
/// "nothing was asked for".
constexpr int kCursorKindNone = 0;
/// Not a kind in the reference's own list: the named setter refuses it outright
/// (`param_1 != 0x35` at 0x00616800), because it is the slot a path's picture
/// goes into - so no name can select it, and only a path can.
constexpr int kCursorKindCustom = 0x35;
/// The one pair the reference's own code asks for by number instead of by name:
/// the coin bag, and its "cannot pay" twin. The merchant and buyback bindings
/// hand the setter 3 or 29 (0x005856d0, 0x00585890), and the hovered-unit code
/// reaches the same pair as `base + 26`, which is how every "can't" cursor in
/// the table sits.
constexpr int kCursorKindBuy = 3;
constexpr int kCursorKindBuyError = 29;

/// The art for a kind, as the reference names the file - mixed case and all,
/// including the entry that is lower case because the art file is. Empty for a
/// kind with none. This is the string array at 0x00ad2808 read straight out of
/// the binary, entry for entry, so the kind indexes it.
inline const char* cursorArtForKind(int kind) {
    static constexpr const char* kArt[] = {
        "",                       // 0  nothing of its own
        "Point",                  // 1  POINT_CURSOR
        "Cast",                   // 2  CAST_CURSOR
        "Buy",                    // 3  BUY_CURSOR
        "Attack",                 // 4  ATTACK_CURSOR
        "Interact",               // 5  INTERACT_CURSOR
        "Speak",                  // 6  SPEAK_CURSOR
        "Inspect",                // 7  INSPECT_CURSOR
        "Pickup",                 // 8  PICKUP_CURSOR
        "Taxi",                   // 9  TAXI_CURSOR
        "Trainer",                // 10 TRAINER_CURSOR
        "Mine",                   // 11 MINE_CURSOR
        "Skin",                   // 12 SKIN_CURSOR
        "GatherHerbs",            // 13 GATHER_CURSOR
        "PickLock",               // 14 LOCK_CURSOR
        "Mail",                   // 15 MAIL_CURSOR
        "LootAll",                // 16 LOOT_ALL_CURSOR
        "Repair",                 // 17 REPAIR_CURSOR
        "RepairNPC",              // 18 REPAIRNPC_CURSOR
        "Item",                   // 19 ITEM_CURSOR
        "SkinHorde",              // 20 SKIN_HORDE_CURSOR
        "SkinAlliance",           // 21 SKIN_ALLIANCE_CURSOR
        "Innkeeper",              // 22 INNKEEPER_CURSOR
        "Quest",                  // 23 QUEST_CURSOR
        "QuestRepeatable",        // 24 QUEST_REPEATABLE_CURSOR
        "QuestTurnIn",            // 25 QUEST_TURNIN_CURSOR
        "vehichleCursor",         // 26 VEHICLE_CURSOR - the file's own spelling
        "UnablePoint",            // 27 POINT_ERROR_CURSOR
        "UnableCast",             // 28 CAST_ERROR_CURSOR
        "UnableBuy",              // 29 BUY_ERROR_CURSOR
        "UnableAttack",           // 30 ATTACK_ERROR_CURSOR
        "UnableInteract",         // 31 INTERACT_ERROR_CURSOR
        "UnableSpeak",            // 32 SPEAK_ERROR_CURSOR
        "UnableInspect",          // 33 INSPECT_ERROR_CURSOR
        "UnablePickup",           // 34 PICKUP_ERROR_CURSOR
        "UnableTaxi",             // 35 TAXI_ERROR_CURSOR
        "UnableTrainer",          // 36 TRAINER_ERROR_CURSOR
        "UnableMine",             // 37 MINE_ERROR_CURSOR
        "UnableSkin",             // 38 SKIN_ERROR_CURSOR
        "UnableGatherHerbs",      // 39 GATHER_ERROR_CURSOR
        "UnablePickLock",         // 40 LOCK_ERROR_CURSOR
        "UnableMail",             // 41 MAIL_ERROR_CURSOR
        "UnableLootAll",          // 42 LOOT_ALL_ERROR_CURSOR
        "UnableRepair",           // 43 REPAIR_ERROR_CURSOR
        "UnableRepairNPC",        // 44 REPAIRNPC_ERROR_CURSOR
        "UnableItem",             // 45 ITEM_ERROR_CURSOR
        "UnableSkinHorde",        // 46 SKIN_HORDE_ERROR_CURSOR
        "UnableSkinAlliance",     // 47 SKIN_ALLIANCE_ERROR_CURSOR
        "UnableInnkeeper",        // 48 INNKEEPER_ERROR_CURSOR
        "UnableQuest",            // 49 QUEST_ERROR_CURSOR
        "UnableQuestRepeatable",  // 50 QUEST_REPEATABLE_ERROR_CURSOR
        "UnableQuestTurnIn",      // 51 QUEST_TURNIN_ERROR_CURSOR
        "UnablevehichleCursor",   // 52 VEHICLE_ERROR_CURSOR
        "",                       // 53 the custom path's own slot, no name
    };
    if (kind < 0 || kind >= static_cast<int>(sizeof(kArt) / sizeof(kArt[0]))) return "";
    return kArt[kind];
}

/// The path of a kind's art, in the form the rest of this client loads cursors
/// by - the same string the reference formats at load time. Empty for a kind
/// with no art of its own.
inline std::string cursorPathForKind(int kind) {
    const char* art = cursorArtForKind(kind);
    if (!*art) return {};
    return std::string("Interface\\Cursor\\") + art + ".blp";
}

/// The path a script named instead of a cursor, made loadable.
///
/// The reference hands the string straight to its texture opener, which adds the
/// extension itself - which is how FrameXML's vehicle bar gets away with
/// `Interface\CURSOR\vehichleCursor`. This client's asset manager resolves a
/// file name, so the extension has to be there. A path that already names a file
/// is left exactly as it was given, backslashes and case included: the lookup
/// folds case, and a path pointing somewhere other than the cursor folder is a
/// legitimate thing for a script to ask for.
inline std::string cursorPathForCustom(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    const std::string leaf = slash == std::string::npos ? path : path.substr(slash + 1);
    if (leaf.find('.') != std::string::npos) return path;
    return path + ".blp";
}

/// The reference's name table (0x005104a0), which SetCursor walks to turn its
/// argument into a kind. The none kind for anything that is not one of those
/// names: a texture path is the other half of the argument, and reaches the
/// custom kind.
inline int cursorKindForName(const std::string& name) {
    static constexpr const char* kNames[] = {
        "POINT_CURSOR", "CAST_CURSOR", "BUY_CURSOR", "ATTACK_CURSOR",
        "INTERACT_CURSOR", "SPEAK_CURSOR", "INSPECT_CURSOR", "PICKUP_CURSOR",
        "TAXI_CURSOR", "TRAINER_CURSOR", "MINE_CURSOR", "SKIN_CURSOR",
        "GATHER_CURSOR", "LOCK_CURSOR", "MAIL_CURSOR", "LOOT_ALL_CURSOR",
        "REPAIR_CURSOR", "REPAIRNPC_CURSOR", "ITEM_CURSOR", "SKIN_HORDE_CURSOR",
        "SKIN_ALLIANCE_CURSOR", "INNKEEPER_CURSOR", "QUEST_CURSOR",
        "QUEST_REPEATABLE_CURSOR", "QUEST_TURNIN_CURSOR", "VEHICLE_CURSOR",
        "POINT_ERROR_CURSOR", "CAST_ERROR_CURSOR", "BUY_ERROR_CURSOR",
        "ATTACK_ERROR_CURSOR", "INTERACT_ERROR_CURSOR", "SPEAK_ERROR_CURSOR",
        "INSPECT_ERROR_CURSOR", "PICKUP_ERROR_CURSOR", "TAXI_ERROR_CURSOR",
        "TRAINER_ERROR_CURSOR", "MINE_ERROR_CURSOR", "SKIN_ERROR_CURSOR",
        "GATHER_ERROR_CURSOR", "LOCK_ERROR_CURSOR", "MAIL_ERROR_CURSOR",
        "LOOT_ALL_ERROR_CURSOR", "REPAIR_ERROR_CURSOR", "REPAIRNPC_ERROR_CURSOR",
        "ITEM_ERROR_CURSOR", "SKIN_HORDE_ERROR_CURSOR",
        "SKIN_ALLIANCE_ERROR_CURSOR", "INNKEEPER_ERROR_CURSOR",
        "QUEST_ERROR_CURSOR", "QUEST_REPEATABLE_ERROR_CURSOR",
        "QUEST_TURNIN_ERROR_CURSOR", "VEHICLE_ERROR_CURSOR",
    };
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i) {
        if (name == kNames[i]) return static_cast<int>(i) + 1;  // POINT_CURSOR is 1
    }
    return kCursorKindNone;
}

}  // namespace ui
}  // namespace wowee
