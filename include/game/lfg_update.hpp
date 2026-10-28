#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace wowee {
namespace network { class Packet; }
namespace game {

/**
 * SMSG_LFG_UPDATE_PLAYER and SMSG_LFG_UPDATE_PARTY, and what wow.exe does
 * with them (0x0055bdc0, cases 0x367 and 0x368).
 *
 * The update types are numbered as the client reads them. The ones it tests
 * are 1, 7, 10, 15 and 16, and they fit AzerothCore's LfgUpdateType - group
 * found at 10, removed from the queue at 7, a member offline at 15.
 */
namespace lfg_update_type {
constexpr uint8_t kDefault            = 0;
constexpr uint8_t kLeaderUnk1         = 1;
constexpr uint8_t kRoleCheckAborted   = 4;
constexpr uint8_t kJoinQueue          = 5;
constexpr uint8_t kRoleCheckFailed    = 6;
constexpr uint8_t kRemovedFromQueue   = 7;
constexpr uint8_t kProposalFailed     = 8;
constexpr uint8_t kProposalDeclined   = 9;
constexpr uint8_t kGroupFound         = 10;
constexpr uint8_t kAddedToQueue       = 12;
constexpr uint8_t kProposalBegin      = 13;
constexpr uint8_t kUpdateStatus       = 14;
constexpr uint8_t kGroupMemberOffline = 15;
constexpr uint8_t kGroupDisband       = 16;
}  // namespace lfg_update_type

/// The interface sounds 0x0055bdc0 plays (0x004c8fc0), by SoundEntries id.
namespace lfg_sound {
constexpr uint32_t kDenied       = 17341;  ///< LFG_Denied
constexpr uint32_t kDungeonReady = 17318;  ///< LFG_DungeonReady
constexpr uint32_t kEnterQueue   = 8462;   ///< PVPENTERQUEUE
}  // namespace lfg_sound

/// The body of either update, as the client keeps it to compare with the
/// next one (0x00acd9a0 for the player, 0x00acdac0 for the party).
struct LfgUpdateBody {
    bool extraInfo = false;
    bool joined = false;      ///< the player packet's extraInfo; the party's own byte
    bool queued = false;
    bool unk1 = false;
    bool unk2 = false;
    uint8_t partyBytes[3] = {};
    std::vector<uint32_t> dungeons;   ///< entries, the type in the top byte
    std::string comment;

    /// 0x005570f0: the five flags, the party bytes, the count, the comment
    /// and every dungeon of one found in the other.
    [[nodiscard]] bool sameAs(const LfgUpdateBody& o) const;
};

/// The client's local join record (0x00acd880..0x00acd99c): the dungeon
/// slots JoinLFG sends - each the dungeon id with its LFGDungeons type in the
/// top byte - the comment, whether a join went out, and whether any of it
/// changed since.
struct LfgLocalJoin {
    bool joined = false;           ///< +0x00
    bool dirty = false;            ///< +0x03
    std::vector<uint32_t> slots;   ///< +0x14 list, +0x18 count
    std::string comment;           ///< +0x1c
};

/// Read either update. False when it is cut short.
bool parseLfgUpdate(network::Packet& packet, bool party, uint8_t& updateType,
                    LfgUpdateBody& body);

/// What one update does, in the order the client does it.
struct LfgUpdateOutcome {
    /// The proposal ends here: drop it, then the line and the event below.
    bool proposalEnds = false;
    bool proposalReturnedLine = false;      ///< ERR_LFG_PROPOSAL_FAILED
    const char* proposalEvent = nullptr;    ///< LFG_PROPOSAL_SUCCEEDED / _FAILED
    /// The queue as stated moved; store the new body.
    bool changed = false;
    /// The interface string to say, or none.
    const char* line = nullptr;
    /// The sound that goes with it: a queue joined or left (lfg_sound), or 0.
    uint32_t sound = 0;
    /// LFG_UPDATE, false.
    bool fireUpdate = false;
};

/// Decide what 0x0055bdc0 does with an update. `hasRaid` answers whether a
/// body names a raid, which makes the lines the raid browser's
/// (0x00557260).
LfgUpdateOutcome decideLfgUpdate(uint8_t updateType, const LfgUpdateBody& body,
                                 const LfgUpdateBody& stored, bool party, bool grouped,
                                 bool proposalValid,
                                 const std::function<bool(const LfgUpdateBody&)>& hasRaid);

/// One member of a proposed group, as SMSG_LFG_PROPOSAL_UPDATE lists them.
struct LfgProposalRow {
    uint32_t roles = 0;
    bool isSelf = false;
    bool inDungeon = false;
    bool sameGroup = false;
    bool answered = false;
    bool accepted = false;
};

/// SMSG_LFG_PROPOSAL_UPDATE as 0x0055bdc0 (case 0x361) reads it.
struct LfgProposal {
    uint32_t dungeonEntry = 0;
    uint8_t state = 0;
    uint32_t id = 0;
    uint32_t encountersDone = 0;
    bool silent = false;
    std::vector<LfgProposalRow> members;   ///< five at most, as the client keeps
};

bool parseLfgProposal(network::Packet& packet, LfgProposal& out);

/// What the proposal update raises, in order.
struct LfgProposalOutcome {
    bool show = false;                 ///< LFG_PROPOSAL_SHOW, after LFG_PROPOSAL_UPDATE
    const char* declinedLine = nullptr;  ///< ERR_LFG_PROPOSAL_DECLINED_PARTY / _SELF
    bool failed = false;               ///< the proposal dropped and LFG_PROPOSAL_FAILED
    uint32_t sound = 0;                ///< LFG_Denied with a decline, else 0
};

/// `lastId` is the last proposal id seen, which this updates (0x00552900).
LfgProposalOutcome decideLfgProposal(const LfgProposal& p, uint32_t& lastId);

/// One party member's locks: the dungeon entries (type in the top byte) they
/// may not queue for, and why, in the order they came (0x00559b10).
struct LfgMemberLocks {
    uint64_t guid = 0;
    std::vector<std::pair<uint32_t, uint32_t>> locks;   ///< entry, status
};

/**
 * One row of a raid browser list — SMSG_UPDATE_LFG_LIST (0x360), read by
 * 0x0055b770. The packet carries rows of two kinds, and the client keeps them as
 * two record shapes: the 0x148-byte one from `LFGInfo.cpp:0x2be` (its field
 * reader is 0x00553620) and the 0x1a8-byte one from `LFGInfo.cpp:0x2d1` (its
 * reader is 0x00559050). Which fields the reader's own getters read is what is
 * kept here; the field runs whose meaning the getters never show are skipped and
 * named as such below.
 */
struct LfgListRow {
    uint64_t guid = 0;          ///< the entry guid: a player's, or a listing's
    bool     isListing = false; ///< true for the 0x1a8 rows, false for the 0x148 ones

    /// The row's comment: a fixed 256-byte field in both shapes (record +0x24 for
    /// a player row, +0x91 for a listing row), trimmed at the first NUL.
    std::string comment;

    /// Only a listing row carries a level — the first byte of its field run at
    /// +0x2c, which `SearchLFGGetResults` pushes as the row's second value.
    uint8_t level = 0;
    bool      hasLevel = false;

    /// The byte at listing +0x88, mask bit 0x40. lfrframe.lua greys a row out
    /// when this is set, and its own name for it in the row fill is `status`.
    bool listed = false;

    /// Which bosses of the list's dungeon this row has done: the dword that
    /// follows the eight-byte link in both shapes (+0x130 and +0x1a0), which
    /// 0x00553830 tests a bit at a time against the dungeon's encounter rows.
    uint32_t encounterMask = 0;

    /// The listing row's own group: the guid its mask bit 0x8 carries, which the
    /// reader (0x00559050:7348-7383) looks up in the same guid hash its rows live
    /// in (0x006792e0) and stores as the record's owner at +0x28. Found, the row
    /// is inserted into that record's member array (0x00556e60); not found, it
    /// joins the array of listings that belong to no one (0x0055b770:9940-9974).
    /// A player row has no such field — the eight bytes at its mask bit 0x80
    /// (+0x128) and at a listing's mask bit 0x80 (+0x198) are read and dropped,
    /// because no getter anywhere reads them.
    uint64_t ownerGuid = 0;

    /// The three bytes a player row carries at mask bit 0x10 (+0x124..0x126).
    /// Nothing reads them. The tail of the row getter is 0x005529e0, which pushes
    /// 28 values — four booleans then 24 numbers, or 28 nils when its record
    /// pointer is null — and every one of those pushes lost its arguments in the
    /// decompile, so which record field answers `isLeader`, `isTank`, `isHealer`
    /// and `isDamage` (lfrframe.lua:539 keeps only the first fourteen of the
    /// getter's 38 values) is not established, and these three bytes are not
    /// claimed as those flags.
    uint8_t rawBytes[3] = {0, 0, 0};
    bool      hasRawBytes = false;
};

/**
 * Reads a whole SMSG_UPDATE_LFG_LIST into `rows`, replacing nothing outside it.
 *
 * The packet opens with the list it describes as two separate uint32s — a type
 * and an id — which the client repacks as `type<<24 | id & 0xffffff` and drops
 * the packet unless that equals the list it holds (0x0055b770:9690-9694). Then
 * one byte: when it is 0 the client clears what it holds and reads no removal
 * block, so the rows that follow are the whole list; when it is set, a counted
 * block of removals comes first. Then two counted blocks — the player rows, then
 * the listing rows — and each block opens with its count followed by one more
 * uint32 that the client stores at 0x00beaaa4 and 0x00beaaa0 and never reads
 * anywhere, so it is stepped over. Each row is an eight-byte guid followed by a
 * uint32 saying which fields that row carries. The reads are plain bytes:
 * 0x0047b340/3c0/400 are one, four and eight, and 0x0047b480 is a fixed-width
 * string.
 *
 * A removal entry is named by guid alone, and the client's test on it
 * (0x0055b770:9709) is narrower than "nonzero": the top nibble of the high dword
 * clear and either the low dword set or a bit set below it (0xf07fffff). An entry
 * that fails that test names a row that is unlinked from its group but kept
 * (0x005589d0), which is not something the rows here model yet.
 *
 * A row whose guid is already held updates it in place, as the client's guid hash
 * does; a whole-list packet clears first. False when the packet is cut short, with
 * the rows read whole kept, which is what the client leaves behind too.
 */
bool parseLfgList(network::Packet& packet, uint32_t heldListId,
                  std::vector<LfgListRow>& rows);

/// "no row" for the index fields below — the client stores a null pointer where
/// these hold an index into the rows, and 0 is a real row.
inline constexpr uint32_t kLfgNoRow = 0xffffffffu;

/**
 * One row the raid browser can show, as 0x00553530 resolves an index into one.
 *
 * A row is not one record but a pair: the player record and **the first listing
 * attached to it** — `*param_3 = **(iVar1 + 0x13c)` (0x00553530:2836). The rest of
 * that group's listings are its members, which is what `SearchLFGGetPartyResults`
 * walks from `+0x13c[partyIndex + 1]` (0x00553490): the row's own listing is
 * member[0] and is skipped there, which is why the member count the row getter
 * pushes is one less than the array's count (0x005534c0).
 */
struct LfgBrowserRow {
    uint32_t listing = kLfgNoRow;       ///< the row's own listing: member[0]
    uint32_t player = kLfgNoRow;        ///< its group's player record, if it has one
    std::vector<uint32_t> members;      ///< the group's other listings, member[1..]
};

/**
 * Orders the parsed rows the way the client's row getter walks them: the player
 * records first, each with the listings whose group guid (mask bit 0x8) names it,
 * and then the listings that belong to no one (0x0055b770:9940-9974 puts those in
 * an array of their own, and the getter reaches them only past the player array).
 *
 * A player record with no listing attached produces **no row at all**, which is
 * what the client does with one: `FUN_00553530` hands back a null listing and the
 * getter returns no values for that index (0x00555fd0:4850-4852), so the row is
 * never seen by the interface.
 *
 * Arrival order throughout. The client qsorts both arrays before showing them
 * (0x0055b700, with the comparators at 0x00555a70 and 0x00555cd0, neither of which
 * has been read), and within a group a listing goes to the head of the member
 * array when its +0x8c byte is set and to the tail otherwise (0x00556e60), which
 * is history this layer does not keep.
 */
std::vector<LfgBrowserRow> buildLfgBrowserRows(const std::vector<LfgListRow>& rows);

/**
 * One row of DungeonEncounter.dbc — the table the raid browser's bosses come from.
 *
 * `0x00553830` walks it as rows of 0x1c bytes (count at 0x00ad36b8, base at
 * 0x00ad36cc) matching two of its fields against the joined dungeon's own
 * LFGDungeons record, `+0x1c` and `+0x20`. That pair is the instance map and the
 * difficulty — the same two `0x005553f0` hands `0x00634950` to get a dungeon's map
 * area, and the same two the extracted file answers at columns 23 and 24 of
 * LFGDungeons.dbc against columns 1 and 2 of DungeonEncounter.dbc (Shadowfang
 * Keep's eight, Naxxramas' fifteen, ToC's five, heroic's three to five). So whose
 * bosses a row shows is decided by the **joined** list's dungeon, not by anything
 * the row itself names.
 */
struct LfgEncounterRow {
    uint32_t mapId = 0;         ///< record +0x04
    uint32_t difficulty = 0;    ///< record +0x08
    /// Which bit of a row's `encounterMask` says this boss is done, record +0x10.
    /// The client masks the byte to five bits before shifting (0x00553830:3021), so
    /// a row naming bit 32 or higher addresses a different boss rather than nothing.
    uint32_t bit = 0;
    /// The boss's name, record +0x14 — the first of the three values
    /// `SearchLFGGetEncounterResults` pushes. The third of those is a texture read
    /// from the record's last field, a SpellIcon id, and that column is zero in
    /// every row of the copy this client reads, so the store lookup answers nothing
    /// and the client pushes nothing for it (0x005538b0:3050-3059).
    std::string name;
};

/// Whether one row's mask says this boss is done: the test at 0x00553830:3021.
inline bool lfgEncounterDone(uint32_t mask, const LfgEncounterRow& row) {
    return (mask & (1u << (row.bit & 0x1f))) != 0;
}

/// How many bosses an instance has and how many of them are done, which are
/// `SearchLFGGetResults`' encountersTotal and encountersComplete
/// (0x00555fd0:4907-4926 pushes both as numbers).
struct LfgEncounterCounts {
    uint32_t total = 0;
    uint32_t complete = 0;
};

/// The walk `0x00553830` makes, over the rows in the order they are stored.
LfgEncounterCounts countLfgEncounters(const std::vector<LfgEncounterRow>& rows,
                                      uint32_t mapId, uint32_t difficulty,
                                      uint32_t mask);

/**
 * The `before`-th boss of one instance in that same order — what
 * `SearchLFGGetEncounterResults` answers for its second argument, which the
/// client passes as `encounterIndex - 1` (0x00553c30:3258) and `0x005538b0` counts
/// matches down to. Null when the instance has no that-many bosses, for which the
/// client pushes three nils rather than nothing (0x005538b0:3069-3071).
 */
const LfgEncounterRow* lfgEncounterAt(const std::vector<LfgEncounterRow>& rows,
                                      uint32_t mapId, uint32_t difficulty,
                                      uint32_t before);

/// The party lock block SMSG_LFG_PARTY_INFO carries, and SMSG_LFG_JOIN_RESULT
/// with result 6 after its two uint32s (0x0055b080): a byte count, then per
/// member a guid, a uint32 count and that many entry/status pairs. A member
/// or an entry named twice is the same one, updated. False when cut short,
/// with what was read whole kept.
bool parseLfgPartyLocks(network::Packet& packet, std::vector<LfgMemberLocks>& out);

}  // namespace game
}  // namespace wowee
