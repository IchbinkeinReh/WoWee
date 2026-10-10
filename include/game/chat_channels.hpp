#pragma once

/// The client's chat channels: which it joins by itself and under what name
/// (ChatChannels.dbc, 0x00507a80 and 0x00508090), the numbered slots it keeps
/// for them (0x005076b0, 0x00507880, 0x00507900, 0x00507a30) and what
/// SMSG_CHANNEL_NOTIFY tells the interface (0x0050e120).
///
/// A channel the DBC knows is never joined by the name the player sees in the
/// list. The client asks for it by its shortcut - "Allgemein" - and sends the
/// row's id with the row's name filled in with where the player is:
/// "Allgemein - Dalaran". That is why nothing here may carry an English name:
/// every name comes out of the client's own ChatChannels.dbc, which is in the
/// client's language.

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wowee::game::chat_channels {

/// ChatChannels.dbc's Flags column, as the client tests them.
constexpr uint32_t kFlagInitial          = 0x00001;  // joined by a character with no chat cache
constexpr uint32_t kFlagZoneDependent    = 0x00002;
constexpr uint32_t kFlagGlobal           = 0x00004;
constexpr uint32_t kFlagTrade            = 0x00008;
constexpr uint32_t kFlagCityOnly         = 0x00010;
constexpr uint32_t kFlagCityName         = 0x00020;  // named after the capitals in a city (0x00507a80)
constexpr uint32_t kFlagDefense          = 0x10000;
constexpr uint32_t kFlagGuildRecruitment = 0x20000;  // 0x004fe260 looks it up by this
constexpr uint32_t kFlagLookingForGroup  = 0x40000;

/// AreaTable's Flags: a city and its districts (the zone 0x00507a80 tests),
/// and the one row that names them all - "Hauptstädte", "City" - which
/// 0x0050edd0 takes as the last row carrying it.
constexpr uint32_t kAreaFlagCity     = 0x100;
constexpr uint32_t kAreaFlagCapitals = 0x200;

/// The most slots the client keeps (0x005076b0 refuses an eleventh).
constexpr size_t kMaxSlots = 10;

/// What is read of a ChatChannels.dbc row: the client's in-memory row is ID,
/// Flags, FactionGroup, Name (+0xc) and Shortcut (+0x10).
struct Row {
    uint32_t id = 0;
    uint32_t flags = 0;
    std::string name;      // "Allgemein - %s"
    std::string shortcut;  // "Allgemein"
};

/// The client's comparison for names (0x0076ea40): case folded, whole string.
/// It folds the accented letters as well; ASCII is what is folded here, which
/// is every name ChatChannels.dbc ships.
inline bool sameName(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(a[i])) !=
            std::toupper(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

/// The row's name with the place in it, as 0x00507a80 prints it with
/// SStrPrintf: the one %s is the place. A row without one - WorldDefense,
/// LookingForGroup - is its own name wherever the player is.
inline std::string formatName(const std::string& pattern, const std::string& place) {
    std::string out;
    bool placed = false;
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '%' && i + 1 < pattern.size()) {
            if (pattern[i + 1] == 's' && !placed) {
                out += place;
                placed = true;
                ++i;
                continue;
            }
            if (pattern[i + 1] == '%') {
                out += '%';
                ++i;
                continue;
            }
        }
        out += pattern[i];
    }
    return out;
}

/// The bit a row has in the client's zone channel mask (0x00bcf010): its id,
/// counted from one. The client asserts the id is under 33; nothing past it
/// is ever set.
inline uint32_t maskBit(uint32_t id) {
    return (id >= 1 && id <= 32) ? (1u << (id - 1)) : 0u;
}

/// The mask a character without a chat cache starts with (0x00508320, the
/// default branch of the chat cache reader): every row flagged initial. On
/// 3.3.5 that is General, Trade, LocalDefense and LookingForGroup.
inline uint32_t initialMask(const std::vector<Row>& rows) {
    uint32_t mask = 0;
    for (const auto& r : rows)
        if (r.flags & kFlagInitial) mask |= maskBit(r.id);
    return mask;
}

/// Where the player is, as 0x00507a80 reads it: the real zone's text
/// (0x00bd0780, what GetRealZoneText answers), whether the zone is a city
/// (its AreaTable row carries kAreaFlagCity) and the name of the capitals'
/// row, empty when AreaTable has none.
struct Place {
    std::string zone;
    bool zoneIsCity = false;
    std::string capitals;
};

/// What a join asks the server for.
struct Join {
    uint32_t zoneChannelId = 0;  // the row's id; zero for a channel of the player's own
    std::string name;            // what goes on the wire
    std::string shortcut;        // the row's shortcut, empty for one of the player's own
    uint32_t rowFlags = 0;
};

/// The first half of 0x00507a80, which every join goes through: a name that
/// is a row's shortcut becomes that row's channel, its name filled in with
/// the place - the capitals' name where the row is named after them and the
/// zone is a city, the real zone's otherwise. Nothing is joined when that
/// text is empty, which is how the client waits for a zone at login. Any other
/// name is a channel of the player's own and is asked for as it was typed.
inline std::optional<Join> resolveJoin(const std::vector<Row>& rows, const std::string& requested,
                                       const Place& place) {
    for (const auto& r : rows) {
        if (!sameName(r.shortcut, requested)) continue;
        const std::string& text =
            ((r.flags & kFlagCityName) && place.zoneIsCity && !place.capitals.empty())
                ? place.capitals : place.zone;
        if (text.empty()) return std::nullopt;
        return Join{r.id, formatName(r.name, text), r.shortcut, r.flags};
    }
    return Join{0, requested, {}, 0};
}

/// A channel slot: the client's 0xa8-byte record at 0x00bcf094.
struct Slot {
    int number = 0;                 // +0x00, one-based; zero is a free slot
    std::string name;               // +0x04, the server's name once joined
    uint8_t flags = 0;              // +0x90, the flags YOU_JOINED carries
    uint32_t zoneChannelId = 0;     // +0x94
    uint32_t instanceId = 0;        // +0x98, YOU_JOINED's last field; arg10
    bool pending = true;            // +0x9c, asked for and not (or no longer) joined
    bool leaving = false;           // +0xa0, the player left it
};

/// The slots, numbered as the client numbers them: a channel's number is the
/// slot it was given when it was asked for, not the order the server answers
/// in, and a channel left frees its number for the next one asked for.
class Slots {
public:
    /// 0x004fdfc0: any slot of that name, joined or not.
    Slot* byName(const std::string& name) {
        for (auto& s : slots_) if (s.number != 0 && sameName(s.name, name)) return &s;
        return nullptr;
    }
    const Slot* byName(const std::string& name) const {
        return const_cast<Slots*>(this)->byName(name);
    }
    /// 0x004fe020: the slot of a zone channel.
    Slot* byZoneId(uint32_t id) {
        if (id == 0) return nullptr;
        for (auto& s : slots_) if (s.number != 0 && s.zoneChannelId == id) return &s;
        return nullptr;
    }
    const Slot* byZoneId(uint32_t id) const { return const_cast<Slots*>(this)->byZoneId(id); }
    /// The slot a notice or a leave names: by id where the server gives one.
    Slot* find(uint32_t id, const std::string& name) { return id != 0 ? byZoneId(id) : byName(name); }
    /// 0x004fdf50 / 0x004fe160: the joined channel with that number.
    const Slot* joinedByNumber(int number) const {
        if (number < 1 || number > static_cast<int>(slots_.size())) return nullptr;
        const Slot& s = slots_[static_cast<size_t>(number - 1)];
        return (s.number == number && !s.pending) ? &s : nullptr;
    }

    /// 0x005076b0: a slot for a channel being asked for - the first free one,
    /// else a new one on the end. False when one is already held for that
    /// name or id, or all ten are taken; the client then sends nothing.
    bool reserve(const std::string& name, uint32_t zoneChannelId) {
        if (byName(name) || byZoneId(zoneChannelId)) return false;
        for (size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].number == 0) {
                slots_[i] = fresh(static_cast<int>(i) + 1, name, zoneChannelId);
                return true;
            }
        }
        if (slots_.size() >= kMaxSlots) return false;
        slots_.push_back(fresh(static_cast<int>(slots_.size()) + 1, name, zoneChannelId));
        return true;
    }

    /// 0x00507880, on YOU_JOINED: the slot is joined, and a zone channel
    /// takes the server's name - which is the zone the server put it in.
    /// Nothing for a channel no slot was asked for. Returns the slot.
    Slot* joined(uint32_t zoneChannelId, const std::string& name, uint32_t instanceId, uint8_t flags) {
        Slot* s = find(zoneChannelId, name);
        if (!s) return nullptr;
        s->pending = false;
        if (s->zoneChannelId != 0) s->name = name;
        s->instanceId = instanceId;
        s->flags = flags;
        return s;
    }

    /// 0x00507900, on YOU_LEFT: no longer joined, the number still held.
    void left(uint32_t zoneChannelId, const std::string& name) {
        if (Slot* s = find(zoneChannelId, name)) s->pending = true;
    }

    /// 0x00507a30: the slot freed, its number for the next channel.
    void clear(uint32_t zoneChannelId, const std::string& name) {
        if (Slot* s = find(zoneChannelId, name)) {
            *s = Slot{};
            s->leaving = true;
        }
    }

    void reset() { slots_.clear(); }
    const std::vector<Slot>& all() const { return slots_; }

private:
    static Slot fresh(int number, const std::string& name, uint32_t zoneChannelId) {
        Slot s;
        s.number = number;
        s.name = name;
        s.zoneChannelId = zoneChannelId;
        s.pending = true;
        s.leaving = false;
        return s;
    }
    std::vector<Slot> slots_;
};

/// What a SMSG_CHANNEL_NOTIFY is to the interface (0x0050e120): the word that
/// is arg1 of the event, which of the two events carries it, and whether the
/// slot is freed once it has been told (local_18). PLAYER_JOINED and
/// PLAYER_LEFT are CHAT_MSG_CHANNEL_JOIN and _LEAVE, and MODE_CHANGE is no
/// chat line at all; none of the three is described here.
struct Notice {
    const char* word = nullptr;
    bool user = false;       // CHAT_MSG_CHANNEL_NOTICE_USER (0x16) rather than _NOTICE (0x15)
    bool freesSlot = false;
};

/// The notice for a type byte. YOU_JOINED and YOU_LEFT depend on the slot and
/// are settled by the caller: `slotWasJoined` is whether the zone channel's
/// slot was already joined when YOU_JOINED came (then it is YOU_CHANGED - the
/// server moved the player to another zone's channel), and for YOU_LEFT
/// `suspended` is the server saying the channel is a constant one being left
/// for want of the zone, which keeps the slot, unless the player left it.
inline Notice noticeFor(uint8_t type, bool slotWasJoined = false, bool slotFound = false,
                        bool playerLeft = false, bool constant = false) {
    switch (type) {
        case 0x02: return {slotWasJoined ? "YOU_CHANGED" : "YOU_JOINED", false, false};
        case 0x03:
            // No slot, no word: the client sends the interface an empty one,
            // which no chat frame matches to a channel and so never prints.
            if (!slotFound) return {};
            if (!playerLeft && constant) return {"SUSPENDED", false, false};
            return {"YOU_LEFT", false, true};
        case 0x04: return {"WRONG_PASSWORD", false, true};
        case 0x05: return {"NOT_MEMBER", false, false};
        case 0x06: return {"NOT_MODERATOR", false, false};
        case 0x07: return {"PASSWORD_CHANGED", true, false};
        case 0x08: return {"OWNER_CHANGED", true, false};
        case 0x09: return {"PLAYER_NOT_FOUND", true, false};
        case 0x0A: return {"NOT_OWNER", false, false};
        case 0x0B: return {"CHANNEL_OWNER", true, false};
        case 0x0D: return {"ANNOUNCEMENTS_ON", true, false};
        case 0x0E: return {"ANNOUNCEMENTS_OFF", true, false};
        case 0x0F: return {"MODERATION_ON", true, false};
        case 0x10: return {"MODERATION_OFF", true, false};
        case 0x11: return {"MUTED", false, false};
        case 0x12: return {"PLAYER_KICKED", true, false};
        case 0x13: return {"BANNED", false, true};
        case 0x14: return {"PLAYER_BANNED", true, false};
        case 0x15: return {"PLAYER_UNBANNED", true, false};
        case 0x16: return {"PLAYER_NOT_BANNED", true, false};
        case 0x17: return {"PLAYER_ALREADY_MEMBER", true, false};
        case 0x18: return {"INVITE", true, false};
        case 0x19: return {"INVITE_WRONG_FACTION", true, false};
        case 0x1A: return {"WRONG_FACTION", true, true};
        case 0x1B: return {"INVALID_NAME", true, true};
        case 0x1C: return {"NOT_MODERATED", true, false};
        case 0x1D: return {"PLAYER_INVITED", true, false};
        case 0x1E: return {"PLAYER_INVITE_BANNED", true, false};
        case 0x1F: return {"THROTTLED", false, false};
        case 0x20: return {"NOT_IN_AREA", false, true};
        case 0x21: return {"NOT_IN_LFG", false, true};
        case 0x22: return {"VOICE_ON", true, false};
        case 0x23: return {"VOICE_OFF", true, false};
        default: return {};
    }
}

/// The channel as a chat event's arg4 names it (0x00509dd0): "1. Allgemein -
/// Dalaran" when a slot carries that name, the bare name when none does.
inline std::string numberedName(const Slot* slot, const std::string& name) {
    if (!slot) return name;
    return std::to_string(slot->number) + ". " + name;
}

/// A GlobalStrings format filled the way Lua's string.format fills it, with
/// %d, %s and the positional %1$s the localized strings use; "%%" is a percent
/// sign. Arguments past the end read as empty.
inline std::string luaFormat(const std::string& fmt, const std::vector<std::string>& args) {
    std::string out;
    size_t next = 0;
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%' || i + 1 >= fmt.size()) { out += fmt[i]; continue; }
        size_t j = i + 1;
        if (fmt[j] == '%') { out += '%'; i = j; continue; }
        size_t index = next;
        bool positional = false;
        if (std::isdigit(static_cast<unsigned char>(fmt[j])) && j + 1 < fmt.size() && fmt[j + 1] == '$') {
            index = static_cast<size_t>(fmt[j] - '1');
            positional = true;
            j += 2;
        }
        if (j >= fmt.size() || (fmt[j] != 's' && fmt[j] != 'd')) { out += fmt[i]; continue; }
        if (!positional) ++next;
        if (index < args.size()) out += args[index];
        i = j;
    }
    return out;
}

/// The line FrameXML's ChatFrame_MessageEventHandler prints for a notice, from
/// its CHAT_<word>_NOTICE string and the event's arguments: CHANNEL_NOTICE is
/// format(s, arg8, arg4) with the instance after the name when there is one;
/// CHANNEL_NOTICE_USER is format(s, arg8, arg4, arg2, arg5) when a second
/// player is named, format(s, arg4, arg2) for INVITE and format(s, arg8,
/// arg4, arg2) otherwise.
inline std::string noticeLine(const std::string& format, bool user, const std::string& word,
                              int number, const std::string& arg4, const std::string& player,
                              const std::string& target, uint32_t instanceId) {
    const std::string n = std::to_string(number);
    if (!user) {
        std::string name = arg4;
        if (instanceId > 0) name += " " + std::to_string(instanceId);
        return luaFormat(format, {n, name});
    }
    if (!target.empty()) return luaFormat(format, {n, arg4, player, target});
    if (word == "INVITE") return luaFormat(format, {arg4, player});
    return luaFormat(format, {n, arg4, player});
}

/// The rows of the channels this client used to join by English name, for a
/// chat window layout saved while it did ("General:0"). Matched by what the
/// rows are rather than what they are called, which is what the client's
/// language changes.
inline const Row* rowForLegacyName(const std::vector<Row>& rows, const std::string& name) {
    const auto first = [&](auto pred) -> const Row* {
        for (const auto& r : rows) if (pred(r)) return &r;
        return nullptr;
    };
    if (name == "General") return first([](const Row& r) { return r.id == 1; });
    if (name == "Trade")
        return first([](const Row& r) {
            return (r.flags & kFlagTrade) && (r.flags & kFlagZoneDependent) && !(r.flags & kFlagLookingForGroup);
        });
    if (name == "LocalDefense")
        return first([](const Row& r) { return (r.flags & kFlagDefense) && (r.flags & kFlagZoneDependent); });
    if (name == "WorldDefense")
        return first([](const Row& r) { return (r.flags & kFlagDefense) && (r.flags & kFlagGlobal); });
    if (name == "LookingForGroup")
        return first([](const Row& r) { return (r.flags & kFlagLookingForGroup) != 0; });
    if (name == "GuildRecruitment")
        return first([](const Row& r) { return (r.flags & kFlagGuildRecruitment) != 0; });
    return nullptr;
}

}  // namespace wowee::game::chat_channels
