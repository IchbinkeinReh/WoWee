#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
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

    /// The byte a listing row carries at mask bit 0x10 (record +0x90). Three of the
    /// sort keys read it: the browse panel's tank, healer and damage columns test
    /// bits 0x2, 0x4 and 0x8 of it (0x00555cd0:4741-4752). The panel's fourth role
    /// column is not a bit at all — its header swaps the two arrays the rows are
    /// walked in, which is the byte at 0x00acd660 below.
    uint8_t roles = 0;

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
 * clear and either the low dword set or a bit set below it (0xf07fffff), and a
 * passing entry is that row deleted. An entry that fails it names a group
 * record, which 0x005589d0 deletes with every member record in its array: the
 * group row and the listing rows it owns go together.
 *
 * A row whose guid is already held updates it in place, as the client's guid hash
 * does; a whole-list packet clears first. False when the packet is cut short, with
 * the rows read whole kept, which is what the client leaves behind too.
 *
 * `wholeList`, when given, says whether the packet was that whole list — the client
 * clears its window then (0x00558af0), which is the same place it unregisters the
 * name callbacks its rows registered, so the count of `lfgNameAnswerClosesGate`
 * above has to be dropped with it.
 */
bool parseLfgList(network::Packet& packet, uint32_t heldListId,
                  std::vector<LfgListRow>& rows, bool* wholeList = nullptr);

/**
 * The client's gate on the names its list rows are waiting for (0x005528a0).
 *
 * A listing row that had to be allocated asks for its guid's name and registers
 * this callback with the request (`0x0055b770:9931-9937`), bumping a counter when
 * the record was new; the callback itself is a bare "an answer arrived" notice — it
 * names no guid, it only closes one request:
 *
 *   cmp dword [0xbeaaac],0 / je ret       nothing outstanding: say nothing at all
 *   sub dword [0xbeaaac],1  / jne ret     still waiting on the others
 *   push 0 / push 0x1d5 / call 0x81b530   script event 469, UPDATE_LFG_LIST
 *
 * So the panel is re-announced exactly once, by the answer that lands last — by
 * which time every row has its name, and with it the class the name query brought
 * along with it. Those names are why the counter exists: the row getter resolves a
 * row's guid through the name cache and answers "UNKNOWN" when nothing is there
 * (0x00555fd0:4856-4861), and the class name and token are read off the same
 * resolved record at +0x144 and +0x148 (0x00555fd0:4873, :4901 through 0x0072aab0).
 *
 * The count's semantics are the client's own, including the hole in them: the
 * callback has no guid to match an answer to, and clearing the window unregisters
 * the callback of every row it drops (0x00558af0:6967) without closing its request.
 * A count left standing across a clear therefore never reaches zero again, which is
 * why the holder below drops it where the client drops the callbacks.
 */
inline bool lfgNameAnswerClosesGate(uint32_t& outstanding) {
    if (outstanding == 0) return false;
    --outstanding;
    return outstanding == 0;
}

/**
 * The name this client's script event 469 (0x1d5) goes by.
 *
 * The client keeps the name of every script event in a table indexed by the event id:
 * `FUN_00608880` (0x00608880, 6290 bytes) writes the names one slot per id up from
 * `0x00c24eb0`, and `FUN_0081ac90` looks an event up in the registry built from it by the
 * same arithmetic (`DAT_00d3f7d8 + id*4`, 0x00810000.c:9546). So 469 is the slot at
 * `0x00c24eb0 + 469*4 = 0x00c25604`, which holds **"UPDATE_LFG_LIST"** — with
 * `UPDATE_LFG_TYPES` at 468 and `UPDATE_LFG_LIST_INCREMENTAL` at 470 beside it, neither of
 * which any code here fires. `LFG_UPDATE` is a different event, 501, and the LFG module
 * fires it with a boolean through the "%b" format at `0x00a02e38`: true from 0x00559310
 * (0x00550000.c:7410) and false from :10509 and :10619.
 *
 * The arithmetic is the client's own, and it lands right where it can be checked against
 * what the code around it says: 613 = "GLYPH_UPDATED" is fired with a glyph slot number off
 * a change to the player field at +0x1248 (0x006d0000.c:8175, `PLAYER_C.cpp:0x17a0`), and
 * 476 = "RAID_TARGET_UPDATE" from `.\RaidInfo.cpp:0x1e9` (0x00570000.c:2221).
 */
inline constexpr std::string_view kLfgListChangedEvent = "UPDATE_LFG_LIST";

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
 * Orders the parsed rows the way the client's row getter walks them: one array of
 * player records, each with the listings whose group guid (mask bit 0x8) names it,
 * and one array of the listings that belong to no one (0x0055b770:9940-9974 puts
 * those in an array of their own).
 *
 * **Which array comes first is the byte at 0x00acd660**, which is what `listingsFirst`
 * is: with it clear the getter walks the player array and then the listings
 * (0x00553530:2831-2847), and with it set the listings come first (:2849-2864). The
 * LFG module's init sets it (`0x0055d440:10772`), and grepping the whole decompile
 * turns up exactly three sites for it — that init, that read, and the swap below —
 * so listings come first until a header says otherwise.
 *
 * A player record with no listing attached produces **no row at all**, which is
 * what the client does with one: `FUN_00553530` hands back a null listing and the
 * getter returns no values for that index (0x00555fd0:4850-4852), so the row is
 * never seen by the interface.
 *
 * Arrival order within each array, which `sortLfgBrowserRows` below then replaces.
 * Within a group a listing goes to the head of the member array when its +0x8c byte
 * is set and to the tail otherwise (0x00556e60), which is history this layer does
 * not keep.
 */
std::vector<LfgBrowserRow> buildLfgBrowserRows(const std::vector<LfgListRow>& rows,
                                               bool listingsFirst);

/**
 * The seven keys the two comparators switch on, numbered as the client numbers them
 * in its key list at 0x00bea820 — an array of seven {key, direction} pairs, read
 * from slot 0 down, first key that says anything deciding the order (0x00555a70:4579
 * and 0x00555cd0:4679; both stop past slot 6).
 *
 * The browse panel's seven column headers name six of them (`lfrframe.xml:697-805`
 * gives each header its `sortType`, and its OnClick passes that to `SearchLFGSort`):
 * "name", "level", "class", "tank", "healer", "damage", and "group" for the last,
 * which the client takes as **swap the two arrays** rather than as a key
 * (0x0055d2b0:10645-10711) — the byte above.
 */
namespace lfg_sort_key {
constexpr uint32_t kZone   = 0;   ///< the listing's +0x84 through a store record's name
constexpr uint32_t kLevel  = 1;   ///< the byte at listing +0x2c
constexpr uint32_t kClass  = 2;   ///< the row's class through a store record's name
constexpr uint32_t kName   = 3;   ///< "name": the two rows' names, compared by strncmp
constexpr uint32_t kTank   = 4;   ///< listing +0x90 bit 0x2
constexpr uint32_t kHealer = 5;   ///< listing +0x90 bit 0x4
constexpr uint32_t kDamage = 6;   ///< listing +0x90 bit 0x8
constexpr uint32_t kCount  = 7;
}  // namespace lfg_sort_key

/// The role bits the three role keys test in a listing row's +0x90 byte.
namespace lfg_role_flag {
constexpr uint8_t kTank = 0x2, kHealer = 0x4, kDamage = 0x8;
}

/// One slot of the client's key list: a key and whether it is read reversed. The
/// comparator negates its result when the direction byte is set (0x00555cd0:4754-4758),
/// so clear means ascending.
struct LfgSortEntry {
    uint32_t key = 0;
    bool descending = false;

    [[nodiscard]] friend bool operator==(const LfgSortEntry&, const LfgSortEntry&) = default;
};

/// How the LFG module's init leaves the list: all seven keys in key order, none
/// reversed (0x0055d440:10766-10771). So the order the panel opens in is the zone
/// first, then level, class, name and the three roles.
inline std::vector<LfgSortEntry> defaultLfgSortKeys() {
    std::vector<LfgSortEntry> keys;
    for (uint32_t k = 0; k < lfg_sort_key::kCount; ++k) keys.push_back(LfgSortEntry{k, false});
    return keys;
}

/**
 * A column header's word to a key, read the way 0x0055d2b0:10645-10683 reads it:
 * case-insensitively (0x0076e780 is strnicmp), and the chain's default is **key 3**,
 * which is what an unrecognised word sorts by. "group" never gets here: the client
 * tests that word first and swaps the arrays instead (:10709-10711), so
 * `lfgSortKeyForName` leaves it as the default and the caller decides.
 */
uint32_t lfgSortKeyForName(std::string_view name);

/**
 * One header click, over both pieces of state at once. The word **"group" never
 * reaches the key list**: the client tests it first (0x0055d2b0:10645) and, on it,
 * swaps the two arrays the rows are walked in and returns without sorting anything
 * (:10709-10711) — `listingsFirst` flips and this returns true. Any other word goes
 * through `lfgSortKeyForName` into `applyLfgSortKey`, and returns false. Every word
 * is read without regard to case, that first test included.
 */
bool applyLfgSortHeaderClick(std::vector<LfgSortEntry>& keys, bool& listingsFirst,
                             std::string_view word);

/**
 * What clicking a column header does to the key list (0x0055d2b0:10684-10705). The
 * key is found in the list — it is always there, since init put all seven in — and
 * moved to slot 0 keeping the direction it had, shifting the slots ahead of it down
 * one. If it was **already** in slot 0 its direction flips instead, which is the
 * only way a column ever reverses: a second click on the same header.
 */
void applyLfgSortKey(std::vector<LfgSortEntry>& keys, uint32_t key);

/// The name a row sorts by: the client resolves the row's listing guid to a player
/// and compares what answers (0x0067d770, then 0x0076e760, which is strncmp). An
/// empty answer is its own answer for "nothing there", and the comparator moves on.
using LfgRowName = std::function<std::string(const LfgListRow& listing)>;

/**
 * The comparison one of the two arrays makes: 0x00555a70 for the player array and
 * 0x00555cd0 for the array of listings that belong to no one. They differ in one
 * way that shows: **the player comparator handles only the first four keys**, so the
 * three role keys decide nothing for a player row and everything for the other
 * array (0x00555a70's switch stops at case 3). Both read the level and the name
 * through the row's **listing** — for a player row that is member[0], which is what
 * the getter hands the interface anyway (:4560-4571).
 *
 * Keys 0 and 2 are read out of store records this client has no answer for — the
 * zone key takes the listing's +0x84 as a key into a store whose record's `+0x2c`
 * is a string (0x00555cd0:4682-4697: the range at 0x00ad3144 and 0x00ad3140 and the
 * record array at 0x00ad3154), and the class key takes a `+0x148` of the resolved
 * player through the same store getter the row getters lost their `this` for, whose
 * record's `+0x10` is a string (:4711-4724). Neither table has been identified, so
 * both keys say nothing here and the walk moves to the next one, which is also what
 * the client does when its lookup comes back empty.
 */
int compareLfgBrowserRows(const LfgBrowserRow& a, const LfgBrowserRow& b,
                          const std::vector<LfgListRow>& rows,
                          const std::vector<LfgSortEntry>& keys,
                          bool playerArray, const LfgRowName& nameOf);

/**
 * The sort 0x0055b700 does before the getter walks anything: **each array on its
 * own comparator** (:9646-9651), the arrays being the two `buildLfgBrowserRows`
 * lays out end to end. Neither qsort is stable, and this one is not either.
 */
void sortLfgBrowserRows(std::vector<LfgBrowserRow>& out, const std::vector<LfgListRow>& rows,
                        const std::vector<LfgSortEntry>& keys, const LfgRowName& nameOf);

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
 * client passes as `encounterIndex - 1` (0x00553c30:3258) and `0x005538b0` counts
 * matches down to. Null when the instance has no that-many bosses, for which the
 * client pushes three nils rather than nothing (0x005538b0:3069-3071).
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
