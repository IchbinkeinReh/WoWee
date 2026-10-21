#pragma once

/// The client's combat log (UnitCombatLog_C.cpp, 0x0074c040-0x00753fa0).
///
/// Every log packet the client reads becomes an entry: one of fifty subevents
/// (the table at 0x00adb758), a source and a destination - each a guid, a name
/// and the unit flags 0x0074dcb0 works out when the entry is made - an
/// optional spell, and the subevent's suffix, described by a bit word
/// (+0x54) whose bits say which values follow. An entry whose names are not
/// known yet waits for them, and so does every entry made after it: they are
/// announced in the order they were made (0x0074f910), as COMBAT_LOG_EVENT
/// when a filter takes them and COMBAT_LOG_EVENT_UNFILTERED always, with the
/// arguments 0x0074e290 pushes - the same ones CombatLogGetCurrentEntry
/// answers when the interface walks the log back.
///
/// Entries are kept for combatLogRetentionTime seconds, and the oldest is
/// only given up when a new one wants its place (0x00750400). Nothing here
/// knows about the world: what an entry is told about units, names and spells
/// comes from whoever builds it (combat_log_feed.hpp).

#include <array>
#include <cstdint>
#include <functional>
#include <list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wowee::game::combat_log {

/// 0x00adb758, in order.
enum class Event : uint8_t {
    EnvironmentalDamage, SwingDamage, SwingMissed, RangeDamage, RangeMissed,
    SpellCastStart, SpellCastSuccess, SpellCastFailed, SpellMissed, SpellDamage,
    SpellHeal, SpellEnergize, SpellDrain, SpellLeech, SpellInstakill,
    SpellSummon, SpellCreate, SpellInterrupt, SpellExtraAttacks, SpellDurabilityDamage,
    SpellDurabilityDamageAll, SpellAuraApplied, SpellAuraAppliedDose, SpellAuraRemovedDose, SpellAuraRemoved,
    SpellAuraRefresh, SpellDispel, SpellStolen, SpellAuraBroken, SpellAuraBrokenSpell,
    DamageAuraBroken, EnchantApplied, EnchantRemoved, SpellPeriodicMissed, SpellPeriodicDamage,
    SpellPeriodicHeal, SpellPeriodicEnergize, SpellPeriodicDrain, SpellPeriodicLeech, SpellDispelFailed,
    DamageShield, DamageShieldMissed, DamageSplit, PartyKill, UnitDied,
    UnitDestroyed, SpellResurrect, SpellBuildingDamage, SpellBuildingHeal, UnitDissipates,
};
inline constexpr size_t kEventCount = 50;

/// The subevent's name, as the arguments and the log file spell it.
[[nodiscard]] const char* eventName(Event event);

/// COMBATLOG_OBJECT_*: what 0x0074dcb0 says about a unit.
namespace object {
inline constexpr uint32_t kAffiliationMine = 0x1;
inline constexpr uint32_t kAffiliationParty = 0x2;
inline constexpr uint32_t kAffiliationRaid = 0x4;
inline constexpr uint32_t kAffiliationOutsider = 0x8;
inline constexpr uint32_t kReactionFriendly = 0x10;
inline constexpr uint32_t kReactionNeutral = 0x20;
inline constexpr uint32_t kReactionHostile = 0x40;
inline constexpr uint32_t kControlPlayer = 0x100;
inline constexpr uint32_t kControlNpc = 0x200;
inline constexpr uint32_t kTypePlayer = 0x400;
inline constexpr uint32_t kTypeNpc = 0x800;
inline constexpr uint32_t kTypePet = 0x1000;
inline constexpr uint32_t kTypeGuardian = 0x2000;
inline constexpr uint32_t kTypeObject = 0x4000;
inline constexpr uint32_t kTarget = 0x10000;
inline constexpr uint32_t kFocus = 0x20000;
inline constexpr uint32_t kMainTank = 0x40000;
inline constexpr uint32_t kMainAssist = 0x80000;
inline constexpr uint32_t kRaidTarget1 = 0x100000;  ///< and up, one bit an icon
inline constexpr uint32_t kNone = 0x80000000;
}  // namespace object

/// 0x0074d1a0, and CombatLog_Object_IsA (0x0074d600) on the pair it was
/// given: one of the special bits (target, focus, roles, marks), or something
/// in each of the four categories.
[[nodiscard]] inline bool maskComplete(uint32_t bits) {
    if ((bits & 0xFFFF0000u) != 0) return true;
    return (bits & 0xFu) != 0 && (bits & 0xF0u) != 0 && (bits & 0x300u) != 0 &&
           (bits & 0xFC00u) != 0;
}

/// A player's guid: no high nibble, and not all zero (the test 0x0074dcb0
/// and 0x0074fd40 make of a guid).
[[nodiscard]] inline bool isPlayerGuid(uint64_t guid) {
    const auto hi = static_cast<uint32_t>(guid >> 32);
    return (hi & 0xF0000000u) == 0 && (static_cast<uint32_t>(guid) != 0 || (hi & 0xF07FFFFFu) != 0);
}
/// The guid's high word under 0xf0f00000: 0xf0300000 a creature, 0xf0500000
/// a vehicle, 0xf0400000 a pet, 0xf0100000 a game object.
[[nodiscard]] inline uint32_t guidKind(uint64_t guid) {
    return static_cast<uint32_t>(guid >> 32) & 0xF0F00000u;
}
inline constexpr uint32_t kGuidCreature = 0xF0300000u;
inline constexpr uint32_t kGuidVehicle = 0xF0500000u;
inline constexpr uint32_t kGuidPet = 0xF0400000u;
inline constexpr uint32_t kGuidGameObject = 0xF0100000u;
/// The entry or pet number a non-player guid carries (bits 24..43).
[[nodiscard]] inline uint32_t guidEntry(uint64_t guid) {
    return static_cast<uint32_t>(guid >> 24) & 0xFFFFFFu;
}

/// What 0x0074dcb0 asks of the world about units.
class UnitWorld {
public:
    virtual ~UnitWorld() = default;
    struct Object {
        enum class Kind : uint8_t { None, Unit, GameObject, Other };
        Kind kind = Kind::None;
        uint64_t charmedBy = 0;   ///< UNIT_FIELD_CHARMEDBY (+0x18 of a unit's fields)
        uint64_t summonedBy = 0;  ///< UNIT_FIELD_SUMMONEDBY (+0x20)
        uint64_t createdBy = 0;   ///< UNIT_FIELD_CREATEDBY (+0x28), a game object's CREATED_BY
    };
    [[nodiscard]] virtual uint64_t activePlayer() const = 0;
    [[nodiscard]] virtual uint64_t target() const = 0;  ///< 0xbd07b0
    [[nodiscard]] virtual uint64_t focus() const = 0;   ///< 0xbd07d0
    /// A raid roster with anyone on it (0xbeb608).
    [[nodiscard]] virtual bool inRaid() const = 0;
    /// Anyone in the player's party slots (0x0052bd10).
    [[nodiscard]] virtual bool hasPartyMembers() const = 0;
    /// The player, or one of the party slots (0x0052c680).
    [[nodiscard]] virtual bool partyMember(uint64_t guid) const = 0;
    /// On the raid roster (0x005726f0).
    [[nodiscard]] virtual bool raidMember(uint64_t guid) const = 0;
    /// The member flags the roster names (2 main tank, 4 main assist): the
    /// raid's (0x00572900) or the party's (0x0052c9a0).
    [[nodiscard]] virtual uint32_t memberFlags(uint64_t guid, bool raid) const = 0;
    /// The raid target icon on it, 0..7, or 8 for none (0x005728c0).
    [[nodiscard]] virtual uint32_t raidTarget(uint64_t guid) const = 0;
    [[nodiscard]] virtual Object object(uint64_t guid) const = 0;
    /// How the unit regards the player, 0 hated .. 7 exalted (0x007251c0
    /// asked of the unit), or nothing when there is no player.
    [[nodiscard]] virtual std::optional<int> reactionToPlayer(uint64_t unit) const = 0;
};

/// 0x0074dcb0: affiliation, reaction, control and type, the target, focus and
/// role bits and the raid mark - kNone for no unit at all.
[[nodiscard]] uint32_t unitFlags(const UnitWorld& world, uint64_t guid);

/// One argument as Lua receives it.
struct Arg {
    enum class Kind : uint8_t { Nil, Number, String };
    Kind kind = Kind::Nil;
    double number = 0.0;
    std::string text;

    [[nodiscard]] static Arg nil() { return {}; }
    [[nodiscard]] static Arg num(double v) { return {.kind = Kind::Number, .number = v, .text = {}}; }
    [[nodiscard]] static Arg str(std::string v) {
        return {.kind = Kind::String, .number = 0.0, .text = std::move(v)};
    }
    bool operator==(const Arg& o) const {
        return kind == o.kind && number == o.number && text == o.text;
    }
};

/// A spell's name and SchoolMask as the client's Spell table has them; the
/// arguments look them up when they are pushed, not when the entry is made.
struct SpellInfo {
    std::string name;
    uint32_t school = 0;
};
using SpellLookup = std::function<std::optional<SpellInfo>(uint32_t spellId)>;

/// What a name is asked for with, by kind (0x0074f720's cases).
struct NameKey {
    enum class Kind : uint8_t { Player = 1, Pet = 2, Creature = 3, GameObject = 4, Item = 5 };
    Kind kind = Kind::Player;
    uint64_t id = 0;  ///< the player's guid; the pet, creature, object or item number
    bool operator==(const NameKey& o) const { return kind == o.kind && id == o.id; }
};

/// One side of an entry.
struct Party {
    uint64_t guid = 0;
    std::optional<std::string> name;  ///< nil when the side is empty
    std::string realm;                ///< written "name-realm" when there is one
    uint32_t flags = object::kNone;
    std::optional<NameKey> waiting;   ///< the name this side waits for
};

/// The suffix bits of +0x54: which values an entry carries, in the order
/// 0x0074e290 pushes them.
namespace suffix {
inline constexpr uint32_t kAmount = 0x1;          ///< extra attacks
inline constexpr uint32_t kFailedType = 0x2;      ///< the cast failure's text
inline constexpr uint32_t kMissType = 0x4;
inline constexpr uint32_t kEnvironmental = 0x8;
inline constexpr uint32_t kDamage = 0x10;
inline constexpr uint32_t kHeal = 0x20;
inline constexpr uint32_t kEnergize = 0x40;
inline constexpr uint32_t kDrain = 0x80;
inline constexpr uint32_t kExtraSpell = 0x100;
inline constexpr uint32_t kItem = 0x200;
inline constexpr uint32_t kAuraType = 0x400;
inline constexpr uint32_t kAuraAmount = 0x800;
inline constexpr uint32_t kEnchantName = 0x1000;
inline constexpr uint32_t kMissAbsorbed = 0x2000;
inline constexpr uint32_t kMissResisted = 0x4000;
inline constexpr uint32_t kMissBlocked = 0x8000;
}  // namespace suffix

/// SpellMissInfo as the miss names have it (0x00adbfac).
namespace miss {
inline constexpr uint32_t kMiss = 1, kResist = 2, kDodge = 3, kParry = 4, kBlock = 5, kEvade = 6,
                          kImmune = 7, kDeflect = 9, kAbsorb = 10, kReflect = 11;
}

struct Entry {
    int64_t timeMs = 0;  ///< the client's clock when it happened
    Event event = Event::EnvironmentalDamage;
    Party src;
    Party dst;
    uint32_t spellId = 0;
    /// The spell's name as the entry was made, for a filter by name (+0x48).
    std::optional<std::string> filterSpellName;

    uint32_t suffix = 0;
    int32_t amount = 0;            ///< kAmount
    std::string failedType;        ///< kFailedType
    uint32_t missType = 0;         ///< kMissType
    uint32_t environmentalType = 0;  ///< kEnvironmental
    struct Damage {
        int32_t amount = 0, overkill = 0;
        uint32_t school = 0;
        int32_t resisted = 0, blocked = 0, absorbed = 0;
        bool critical = false, glancing = false, crushing = false;
    } damage;                      ///< kDamage
    int32_t missAbsorbed = 0, missResisted = 0, missBlocked = 0;
    struct Heal {
        int32_t amount = 0, overhealing = 0, absorbed = 0;
        bool critical = false;
    } heal;                        ///< kHeal
    struct Power {
        int32_t amount = 0;
        uint32_t powerType = 0;
        int32_t extraAmount = 0;
    } power;                       ///< kEnergize, kDrain
    uint32_t extraSpellId = 0;     ///< kExtraSpell
    uint32_t itemId = 0;           ///< kItem
    std::optional<std::string> itemName;
    std::optional<NameKey> itemWaiting;
    bool auraDebuff = false;       ///< kAuraType
    int32_t auraAmount = 0;        ///< kAuraAmount
    std::string enchantName;       ///< kEnchantName

    [[nodiscard]] bool waiting() const {
        return src.waiting.has_value() || dst.waiting.has_value() || itemWaiting.has_value();
    }
};

/// "0x%016X", as 0x0074d0d0 writes a guid.
[[nodiscard]] std::string guidString(uint64_t guid);
/// 0x0074d120: hexadecimal, an optional "0x" in front, sixteen digits at most.
[[nodiscard]] uint64_t parseGuid(std::string_view text);

/// 0x0074e290: timestamp, event, source guid, name and flags, destination
/// guid, name and flags, then the spell and the suffix.
[[nodiscard]] std::vector<Arg> entryArgs(const Entry& entry, double timestamp, const SpellLookup& spells);

/// The entry as LoggingCombat writes it to Logs\WoWCombatLog.txt
/// (0x0074e880): the event, then each value after a comma - guids "0x%016X",
/// names quoted with '"' and '\' escaped, flags and a spell's school
/// "0x%x", numbers "%d", the miss, environment and aura types bare, "nil"
/// for what is not there. Unlike the event's arguments it has no timestamp,
/// and writes a zero where those have nil for one.
[[nodiscard]] std::string fileLine(const Entry& entry, const SpellLookup& spells);
/// "%u/%u %02u:%02u:%02u.%03u  ", the local time a log file's line starts
/// with (0x007752a0).
[[nodiscard]] std::string fileTimePrefix(unsigned month, unsigned day, unsigned hour, unsigned minute,
                                         unsigned second, unsigned millisecond);

/// One CombatLogAddFilter.
struct Filter {
    std::array<bool, kEventCount> events{};
    uint64_t srcGuid = 0;
    uint32_t srcMask = 0;
    uint64_t dstGuid = 0;
    uint32_t dstMask = 0;
    uint32_t spellId = 0;
    std::optional<std::string> spellName;

    /// 0x0074e050's list: names separated by spaces or commas; nothing at all
    /// is every event.
    void setEvents(const std::optional<std::string>& list);
    /// 0x0074e1a0.
    [[nodiscard]] bool matches(const Entry& entry) const;
};

class Log {
public:
    struct Hooks {
        std::function<int64_t()> nowMs;                 ///< the client's clock (0xcd76ac)
        std::function<int()> retentionSeconds;          ///< combatLogRetentionTime
        std::function<bool(uint64_t)> hidesGuid;        ///< 0x0074d210's creature flag
        /// Both events, the first only when a filter took it (0x0074f910).
        std::function<void(const Entry&, bool filtered)> announce;
    };
    void setHooks(Hooks hooks) { hooks_ = std::move(hooks); }

    /// 0x007538a0: empty, and the clock the timestamps count from.
    void reset(int64_t nowMs, int64_t unixSeconds);

    /// 0x00750400: a fresh entry for the builder to fill. The oldest entry is
    /// taken for it when it has outlived the retention, one per entry made.
    Entry& create(Event event);
    /// 0x0074ff20: into the log, timed `delayMs` ago, and announced when
    /// nothing ahead of it still waits for a name.
    void commit(uint32_t delayMs);

    /// 0x0074fe90: a name that entries waited for. Not found is UNKNOWNOBJECT.
    void answer(const NameKey& key, const std::string& name, const std::string& realm);

    /// 0x0074e290's timestamp: seconds since 1970, to the millisecond.
    [[nodiscard]] double timestampOf(const Entry& entry) const;

    // The Lua API.
    /// CombatLogGetNumEntries (0x0074fa70).
    [[nodiscard]] int numEntries(bool ignoreFilter) const;
    /// CombatLogSetCurrentEntry (0x0074fae0); whether there is one now.
    bool setCurrent(int index, bool ignoreFilter);
    /// CombatLogAdvanceEntry (0x0074fc20).
    bool advance(int count, bool ignoreFilter);
    /// CombatLogGetCurrentEntry (0x0074f2b0).
    [[nodiscard]] const Entry* current() const;
    /// CombatLogClearEntries (0x00751120).
    void clearEntries();
    /// CombatLogResetFilter (0x0074fa60).
    void resetFilters() { filters_.clear(); }
    /// CombatLogAddFilter (0x0074ff70).
    void addFilter(Filter filter) { filters_.push_back(std::move(filter)); }
    /// 0x0074f6c0: no filter at all takes everything.
    [[nodiscard]] bool passesFilters(const Entry& entry) const;

    [[nodiscard]] size_t size() const { return entries_.size(); }

private:
    using It = std::list<Entry>::iterator;
    void remove(It it);
    void finalize(Entry& entry) const;
    void announcePending();

    Hooks hooks_;
    std::list<Entry> entries_;
    std::optional<Entry> draft_;
    It current_ = entries_.end();   ///< 0xca1390
    It announce_ = entries_.end();  ///< 0xca1394: the first not yet announced
    std::vector<Filter> filters_;
    int64_t baseMs_ = 0;      ///< 0xca1388
    int64_t baseUnix_ = 0;    ///< 0xca138c
    bool announcing_ = false;
};

}  // namespace wowee::game::combat_log
