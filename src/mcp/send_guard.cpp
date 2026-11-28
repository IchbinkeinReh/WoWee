#include "mcp/send_guard.hpp"
#include "mcp/settings_ledger.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

namespace wowee::mcp {

namespace {

bool startsWith(std::string_view s, std::string_view head) {
    return s.size() >= head.size() && s.substr(0, head.size()) == head;
}

bool endsWith(std::string_view s, std::string_view tail) {
    return s.size() >= tail.size() && s.substr(s.size() - tail.size()) == tail;
}

bool isOneOf(std::string_view s, std::initializer_list<std::string_view> list) {
    return std::find(list.begin(), list.end(), s) != list.end();
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool sameName(std::string_view a, std::string_view b) {
    return !a.empty() && lower(a) == lower(b);
}

Verdict never(std::string why) {
    return {.kind = Verdict::Kind::Never, .why = std::move(why)};
}

/// Refused unless --mcp-allow names it.
Verdict social(Social s, std::string_view what, const GuardContext& context) {
    if ((context.allow & static_cast<uint32_t>(s)) != 0) return {};
    return {.kind = Verdict::Kind::Social,
            .social = s,
            .why = std::string(what) + " is not done towards other players (--mcp-allow=" + socialName(s) + ")"};
}

/// What the realm would keep of the settings.
Verdict keeps(std::string_view what, const GuardContext& context) {
    if ((context.allow & static_cast<uint32_t>(Social::Settings)) != 0) return {};
    return {.kind = Verdict::Kind::Social,
            .social = Social::Settings,
            .why = std::string(what) + " changed here are not kept on the realm (--mcp-allow=settings)"};
}

/// The bytes of a packet in order; past the end it says so and answers zeros.
class Reader {
public:
    explicit Reader(std::span<const uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] bool ok() const { return ok_; }
    void skip(size_t n) {
        if (n > bytes_.size() - at_) ok_ = false;
        else at_ += n;
    }
    uint8_t u8() {
        if (at_ >= bytes_.size()) {
            ok_ = false;
            return 0;
        }
        return bytes_[at_++];
    }
    uint32_t u32() {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(u8()) << (8 * i);
        return v;
    }
    uint64_t u64() {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(u8()) << (8 * i);
        return v;
    }
    uint64_t packedGuid() {
        const uint8_t mask = u8();
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            if (mask & (1u << i)) v |= static_cast<uint64_t>(u8()) << (8 * i);
        }
        return v;
    }
    std::string text() {
        std::string out;
        while (ok_) {
            const uint8_t c = u8();
            if (!ok_ || c == 0) break;
            out += static_cast<char>(c);
        }
        return out;
    }

private:
    std::span<const uint8_t> bytes_;
    size_t at_ = 0;
    bool ok_ = true;
};

/// Whether an act at `guid` is at another player, or at what is theirs.
bool aimsAtOther(uint64_t guid, const GuardContext& context) {
    if (guid == 0 || guid == context.ownGuid) return false;
    if (isPlayerGuid(guid)) return true;
    return context.belongsToOtherPlayer && context.belongsToOtherPlayer(guid);
}

Verdict judgeAim(uint64_t guid, const GuardContext& context) {
    if (aimsAtOther(guid, context)) return social(Social::Players, "a spell, item or attack", context);
    return {};
}

/// SpellCastTargets: the guid of the unit, game object or corpse in it.
std::optional<uint64_t> castTarget(Reader& r) {
    const uint32_t mask = r.u32();
    // TARGET_FLAG_UNIT, UNIT_MINIPET, GAMEOBJECT, CORPSE_ENEMY, CORPSE_ALLY.
    constexpr uint32_t kGuid = 0x2u | 0x10000u | 0x800u | 0x200u | 0x8000u;
    const uint64_t guid = (mask & kGuid) ? r.packedGuid() : 0;
    if (!r.ok()) return std::nullopt;
    return guid;
}

Verdict judgeCast(std::string_view opcode, std::span<const uint8_t> payload, const GuardContext& context) {
    if (!context.wotlkWire) return judgeAim(context.targetGuid, context);
    Reader r(payload);
    if (opcode == "CMSG_USE_ITEM") r.skip(1 + 1 + 1 + 4 + 8 + 4 + 1);  // bag, slot, count, spell, item, glyph, flags
    else if (opcode == "CMSG_PET_CAST_SPELL") r.skip(8 + 1 + 4 + 1);   // caster, count, spell, flags
    else r.skip(1 + 4 + 1);                                            // count, spell, flags
    const auto guid = castTarget(r);
    if (!guid) return never("a cast whose target could not be read");
    return judgeAim(*guid, context);
}

/// CMSG_MESSAGECHAT: type, language, whom (of a whisper) or where (of a
/// channel), what.
Verdict judgeChat(std::span<const uint8_t> payload, const GuardContext& context) {
    constexpr uint32_t kWhisper = 7;
    constexpr uint32_t kChannel = 17;
    Reader r(payload);
    const uint32_t type = r.u32();
    r.skip(4);
    std::string whom;
    if (type == kWhisper || type == kChannel) whom = r.text();
    const std::string what = r.text();
    if (type == kWhisper && r.ok() && sameName(whom, context.ownName)) {
        if (!what.empty() && (what[0] == '.' || what[0] == '!')) return judgeGmCommand(what, context);
        return {};
    }
    // A command in any other chat is the server's to take out of it, and one
    // it does not know stays in: it is said.
    return social(Social::Chat, "chat", context);
}

struct Family {
    std::string_view prefix;
    Social social;
    std::string_view what;
};

/// Families of names refused as a whole, but for the ones in kHarmless.
constexpr std::array<Family, 25> kFamilies = {{
    {"CMSG_GUILD_", Social::Guild, "guild business"},
    {"MSG_GUILD_", Social::Guild, "guild business"},
    {"MSG_SAVE_GUILD_EMBLEM", Social::Guild, "guild business"},
    {"CMSG_SET_GUILD_BANK_TEXT", Social::Guild, "guild business"},
    {"CMSG_PETITION_", Social::Guild, "a charter"},
    {"MSG_PETITION_", Social::Guild, "a charter"},
    {"CMSG_OFFER_PETITION", Social::Guild, "a charter"},
    {"CMSG_TURN_IN_PETITION", Social::Guild, "a charter"},
    {"CMSG_ARENA_TEAM_", Social::Guild, "arena team business"},
    {"CMSG_CALENDAR_", Social::Guild, "the calendar"},
    {"CMSG_GROUP_", Social::Group, "a group"},
    {"MSG_RAID_", Social::Group, "a raid"},
    {"MSG_PARTY_", Social::Group, "a party"},
    {"CMSG_PARTY_", Social::Group, "a party"},
    {"CMSG_LFG_", Social::Group, "the dungeon finder"},
    {"CMSG_SEARCH_LFG_", Social::Group, "the dungeon finder"},
    {"CMSG_BATTLEFIELD_", Social::Group, "a battleground"},
    {"CMSG_BATTLEMASTER_JOIN", Social::Group, "a battleground"},
    {"CMSG_LOOT_M", Social::Group, "group loot"},  // LOOT_METHOD, LOOT_MASTER_GIVE; LOOT_MONEY below
    {"CMSG_CHANNEL_", Social::Chat, "a chat channel"},
    {"CMSG_AUCTION_", Social::Auction, "an auction"},
    {"CMSG_MAIL_", Social::Mail, "mail"},
    {"CMSG_DUEL_", Social::Duel, "a duel"},
    {"CMSG_GMTICKET", Social::Tickets, "a ticket"},
    {"CMSG_GMRESPONSE_", Social::Tickets, "a ticket"},
}};

// One left empty would be the beginning of every name there is.
static_assert(std::all_of(kFamilies.begin(), kFamilies.end(), [](const Family& f) { return !f.prefix.empty(); }));

struct One {
    std::string_view opcode;
    Social social;
    std::string_view what;
};

constexpr std::array<One, 41> kSingles = {{
    {"CMSG_EMOTE", Social::Emote, "an emote"},
    {"CMSG_TEXT_EMOTE", Social::Emote, "an emote"},
    {"CMSG_SEND_MAIL", Social::Mail, "mail"},
    {"CMSG_INITIATE_TRADE", Social::Trade, "a trade"},
    {"CMSG_BEGIN_TRADE", Social::Trade, "a trade"},
    {"CMSG_ACCEPT_TRADE", Social::Trade, "a trade"},
    {"CMSG_SET_TRADE_ITEM", Social::Trade, "a trade"},
    {"CMSG_SET_TRADE_GOLD", Social::Trade, "a trade"},
    {"CMSG_ACCEPT_LFG_MATCH", Social::Group, "the dungeon finder"},
    {"CMSG_SET_LOOKING_FOR_GROUP", Social::Group, "the dungeon finder"},
    {"CMSG_SET_LFG_COMMENT", Social::Group, "the dungeon finder"},
    {"CMSG_SUMMON_RESPONSE", Social::Group, "a summons"},
    {"CMSG_LOOT_ROLL", Social::Group, "group loot"},
    {"CMSG_OPT_OUT_OF_LOOT", Social::Group, "group loot"},
    {"CMSG_PUSHQUESTTOPARTY", Social::Group, "a shared quest"},
    {"MSG_QUEST_PUSH_RESULT", Social::Group, "a shared quest"},
    {"CMSG_QUEST_CONFIRM_ACCEPT", Social::Group, "a shared quest"},
    {"MSG_MINIMAP_PING", Social::Group, "a map ping"},
    {"MSG_RANDOM_ROLL", Social::Group, "a roll"},
    {"CMSG_INSTANCE_LOCK_RESPONSE", Social::Group, "an instance's lock"},
    {"CMSG_ACCEPT_LEVEL_GRANT", Social::Friends, "a granted level"},
    {"CMSG_GRANT_LEVEL", Social::Friends, "a granted level"},
    {"CMSG_REFER_A_FRIEND", Social::Friends, "the friends list"},
    {"CMSG_ADD_FRIEND", Social::Friends, "the friends list"},
    {"CMSG_DEL_FRIEND", Social::Friends, "the friends list"},
    {"CMSG_ADD_IGNORE", Social::Friends, "the ignore list"},
    {"CMSG_DEL_IGNORE", Social::Friends, "the ignore list"},
    {"CMSG_ADD_VOICE_IGNORE", Social::Friends, "the ignore list"},
    {"CMSG_DEL_VOICE_IGNORE", Social::Friends, "the ignore list"},
    {"CMSG_SET_CONTACT_NOTES", Social::Friends, "the friends list"},
    {"CMSG_CHAT_IGNORED", Social::Chat, "an ignored whisper's answer"},
    {"CMSG_BUG", Social::Tickets, "a bug report"},
    {"CMSG_COMPLAIN", Social::Tickets, "a complaint"},
    {"CMSG_REPORT_PVP_AFK", Social::Tickets, "a report"},
    {"CMSG_GMSURVEY_SUBMIT", Social::Tickets, "a survey"},
    {"CMSG_TOGGLE_PVP", Social::Players, "the pvp flag"},
    {"CMSG_CONTROLLER_EJECT_PASSENGER", Social::Players, "a passenger put off"},
    {"CMSG_PLAYER_VEHICLE_ENTER", Social::Players, "another player's vehicle"},
    {"CMSG_VOICE_SESSION_ENABLE", Social::Chat, "voice chat"},
    {"CMSG_VOICE_SET_TALKER_MUTED_REQUEST", Social::Chat, "voice chat"},
    {"CMSG_SET_ACTIVE_VOICE_CHANNEL", Social::Chat, "voice chat"},
}};

/// Of the families above what only asks, says no, or ends something: reading
/// a roster or a mailbox's list, declining, cancelling.
constexpr std::array<std::string_view, 43> kHarmless = {{
    "CMSG_GUILD_QUERY", "CMSG_GUILD_ROSTER", "CMSG_GUILD_INFO", "CMSG_GUILD_DECLINE", "CMSG_GUILD_BANK_QUERY_TAB",
    "CMSG_GUILD_BANKER_ACTIVATE", "MSG_GUILD_PERMISSIONS", "MSG_GUILD_BANK_LOG_QUERY",
    "MSG_GUILD_BANK_MONEY_WITHDRAWN", "MSG_GUILD_EVENT_LOG_QUERY", "CMSG_PETITION_QUERY", "CMSG_PETITION_SHOWLIST",
    "CMSG_PETITION_SHOW_SIGNATURES", "MSG_PETITION_DECLINE", "CMSG_ARENA_TEAM_QUERY", "CMSG_ARENA_TEAM_ROSTER",
    "CMSG_ARENA_TEAM_DECLINE", "CMSG_CALENDAR_GET_CALENDAR", "CMSG_CALENDAR_GET_EVENT",
    "CMSG_CALENDAR_GET_NUM_PENDING", "CMSG_GROUP_DECLINE", "CMSG_GROUP_CANCEL", "CMSG_LFG_GET_STATUS",
    "CMSG_LFG_LEAVE", "CMSG_SEARCH_LFG_LEAVE", "CMSG_BATTLEFIELD_LIST", "CMSG_BATTLEFIELD_STATUS", "CMSG_LOOT_MONEY",
    "CMSG_CHANNEL_LIST", "CMSG_CHANNEL_DISPLAY_LIST", "CMSG_CHANNEL_VOICE_OFF", "CMSG_CHANNEL_VOICE_ON",
    "CMSG_AUCTION_LIST_ITEMS", "CMSG_AUCTION_LIST_BIDDER_ITEMS", "CMSG_AUCTION_LIST_OWNER_ITEMS",
    "CMSG_AUCTION_LIST_PENDING_SALES", "CMSG_MAIL_MARK_AS_READ", "CMSG_DUEL_CANCELLED", "CMSG_GMTICKET_GETTICKET",
    "CMSG_GMTICKET_SYSTEMSTATUS", "CMSG_GMTICKETSYSTEM_TOGGLE", "CMSG_LFG_SET_ROLES",
    "MSG_RAID_READY_CHECK_FINISHED",
}};

/// What is not done at all through the MCP server, whatever it was started
/// with: what cannot be had back.
constexpr std::array<std::string_view, 18> kNever = {{
    "CMSG_CHAR_DELETE", "CMSG_CHAR_CREATE", "CMSG_CHAR_RENAME", "CMSG_CHAR_CUSTOMIZE", "CMSG_CHAR_FACTION_CHANGE",
    "CMSG_CHAR_RACE_CHANGE", "CMSG_DESTROYITEM", "CMSG_DESTROY_ITEMS", "CMSG_PET_ABANDON", "CMSG_UNLEARN_SKILL",
    "CMSG_WHOIS", "CMSG_WORLD_TELEPORT", "CMSG_TELEPORT_TO_UNIT", "CMSG_SERVER_COMMAND", "CMSG_RUN_SCRIPT",
    "CMSG_BOOTME", "CMSG_SERVER_BROADCAST", "CMSG_GODMODE",
}};

}  // namespace

std::vector<std::string_view> namedOpcodes() {
    std::vector<std::string_view> out;
    for (const auto& s : kSingles) out.push_back(s.opcode);
    out.insert(out.end(), kHarmless.begin(), kHarmless.end());
    out.insert(out.end(), kNever.begin(), kNever.end());
    out.emplace_back("CMSG_DISMISS_CONTROLLED_VEHICLE");
    out.emplace_back("CMSG_UPDATE_ACCOUNT_DATA");
    out.emplace_back("CMSG_SET_ACTIONBAR_TOGGLES");
    return out;
}

bool isPlayerGuid(uint64_t guid) {
    return guid != 0 && (guid >> 48) == 0;
}

Verdict judgeSend(std::string_view opcode, std::span<const uint8_t> payload, const GuardContext& context) {
    if (opcode.find("MESSAGECHAT") != std::string_view::npos) return judgeChat(payload, context);
    if (opcode == "CMSG_CAST_SPELL" || opcode == "CMSG_PET_CAST_SPELL" || opcode == "CMSG_USE_ITEM") {
        return judgeCast(opcode, payload, context);
    }
    if (opcode == "CMSG_ATTACKSWING") {
        Reader r(payload);
        const uint64_t guid = r.u64();
        if (!r.ok()) return never("an attack whose target could not be read");
        return judgeAim(guid, context);
    }
    if (opcode == "CMSG_PET_ACTION") {
        Reader r(payload);
        const uint64_t pet = r.u64();
        const uint32_t action = r.u32();
        const uint64_t guid = r.u64();
        if (!r.ok()) return never("a pet's order that could not be read");
        // COMMAND_ABANDON: the pet let go of for good, or dismissed.
        constexpr uint32_t kCommand = 0x07;
        constexpr uint32_t kAbandon = 3;
        if ((action >> 24) == kCommand && (action & 0x00FFFFFFu) == kAbandon) {
            // A charmed creature let go of is the realm's again, not a pet
            // lost (tester's wish: PetDismiss of an enslaved demon).
            // A summoned pet that is no hunter's is only unsummoned (tester's
            // wish: PetDismiss of the warlock's imp).
            if (pet == 0 || (pet != context.charmGuid && pet != context.summonedPetGuid))
                return never("abandoning a pet cannot be had back");
            return {};
        }
        return judgeAim(guid, context);
    }

    if (opcode == "CMSG_UPDATE_ACCOUNT_DATA") {
        Reader r(payload);
        const uint32_t type = r.u32();
        if (!r.ok()) return never("account data whose kind could not be read");
        return isSettingsData(type) ? keeps(settingsDataName(type), context) : Verdict{};
    }
    if (opcode == "CMSG_SET_ACTIONBAR_TOGGLES") return keeps("the action bars shown", context);

    if (opcode == "CMSG_DISMISS_CONTROLLED_VEHICLE") {
        // Getting out of what the character drives itself: the mover is named
        // first. A player's own mount, or somebody else's creature, is theirs.
        Reader r(payload);
        const uint64_t mover = r.packedGuid();
        const bool own = r.ok() && mover != 0 && mover == context.vehicleGuid && !isPlayerGuid(mover) &&
                         !(context.belongsToOtherPlayer && context.belongsToOtherPlayer(mover));
        return own ? Verdict{} : social(Social::Players, "a shared vehicle", context);
    }

    for (const std::string_view name : kNever) {
        if (opcode == name) return never(std::string(opcode) + " is never sent through the MCP server");
    }
    // The game masters' and the developers' own packets.
    if (endsWith(opcode, "_CHEAT") || startsWith(opcode, "CMSG_CHEAT_") || startsWith(opcode, "CMSG_DEBUG_") ||
        startsWith(opcode, "CMSG_GM_") || startsWith(opcode, "MSG_GM_") || startsWith(opcode, "CMSG_MAELSTROM_") ||
        startsWith(opcode, "CMSG_SET_ARENA_")) {
        return never(std::string(opcode) + " is never sent through the MCP server");
    }
    for (const std::string_view name : kHarmless) {
        if (opcode == name) return {};
    }
    for (const auto& s : kSingles) {
        if (opcode == s.opcode) return social(s.social, s.what, context);
    }
    for (const auto& f : kFamilies) {
        if (startsWith(opcode, f.prefix)) return social(f.social, f.what, context);
    }
    return {};
}

namespace {

bool isNumber(std::string_view s) {
    if (!s.empty() && s[0] == '-') s.remove_prefix(1);
    if (s.empty()) return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || c == '.'; });
}

/// A number, or what a shift-clicked link is written as.
bool isNumberOrLink(std::string_view s) {
    return isNumber(s) || (!s.empty() && (s[0] == '[' || s[0] == '|'));
}

/// Whether the words after a command's verb are ones it may be given with.
/// Many of them take a player's name, or the number of one, and are then
/// about that player wherever they are - so those are taken bare, or with the
/// character's own name.
/// The words from `from` on as one name in double quotes ("Die Gilde"), its
/// inside in lower case; nothing when they are not that. A guild's name is
/// always quoted here, so it cannot be read as a player's.
std::optional<std::string> quotedName(const std::vector<std::string>& w, size_t from) {
    if (from >= w.size()) return std::nullopt;
    std::string joined;
    for (size_t i = from; i < w.size(); ++i) joined += (i > from ? " " : "") + w[i];
    if (joined.size() < 3 || joined.front() != '"' || joined.back() != '"') return std::nullopt;
    std::string inside = joined.substr(1, joined.size() - 2);
    if (inside.find('"') != std::string::npos) return std::nullopt;
    return inside;
}

/// A name as the words of a command have it: lower case, single spaces.
std::string asWords(std::string_view s) {
    std::string out;
    for (const char c : lower(s)) {
        if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
        out += c;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

bool gmWordsAllowed(const std::vector<std::string>& w, const std::string& ownName, const std::string& ownGuild) {
    const std::string& verb = w[0];
    const std::string sub = w.size() > 1 ? w[1] : std::string();
    const size_t args = w.size() - 1;
    // A guild for the character itself to lead (the user, 2026-11-27, for
    // the guild bank): `.guild create "Name"` (the leader is the selected
    // player or the character - and nothing but the character may be
    // selected), or with the own name before it. Deleted only the guild the
    // character is in, by its name.
    if (verb == "guild") {
        if (sub == "create") {
            if (quotedName(w, 2)) return true;
            return args >= 3 && sameName(w[2], ownName) && quotedName(w, 3).has_value();
        }
        if (sub == "delete") {
            const auto name = quotedName(w, 2);
            return name && !ownGuild.empty() && *name == asWords(ownGuild);
        }
        return false;
    }
    // An arena team for the character itself to captain (tester's wish, V529):
    // `.arena create "Name" 2|3|5`, or with the own name before the team's.
    if (verb == "arena") {
        if (sub != "create" || args < 3 || !isOneOf(w.back(), {"2", "3", "5"})) return false;
        std::vector<std::string> rest(w.begin(), w.end() - 1);
        if (quotedName(rest, 2)) return true;
        return args >= 4 && sameName(w[2], ownName) && quotedName(rest, 3).has_value();
    }
    if (isOneOf(verb, {"gps", "revive", "die", "dismount", "demorph", "combatstop", "maxskill", "cooldown", "levelup",
                       "respawn", "save", "distance"})) {
        return args == 0 || (args == 1 && sameName(sub, ownName));
    }
    if (isOneOf(verb, {"help", "commands"})) return true;
    if (isOneOf(verb, {"aura", "unaura", "learn", "unlearn"})) return isNumberOrLink(sub) || sub == "all";
    // A second number would make the first one a player's (the realm reads
    // an optional player before the item, a guid by number): a count comes
    // after the character's own name (`.additem Locki ID COUNT`, tester's
    // wish), positive, as a negative count takes items away.
    if (verb == "additem") {
        if (args == 3 && sameName(sub, ownName) && isNumberOrLink(w[2]) && isNumber(w[3]) && w[3][0] != '-' &&
            w[3] != "0" && w[3].find('.') == std::string::npos)
            return true;
        return args >= 1 && isNumberOrLink(sub) && (args == 1 || !isNumber(sub));
    }
    if (verb == "morph") return args == 1 && isNumber(sub);
    // The zone's weather (`.wchange TYPE GRADE`: type 0 fine, 1 rain, 2 snow,
    // 3 storm; grade 0..1), for V670 - set back to the zone's own after.
    if (verb == "wchange") return args == 2 && isNumber(sub) && isNumber(w[2]) && sub[0] != '-' && w[2][0] != '-';
    if (verb == "go") {
        return isOneOf(sub, {"xyz", "zonexy", "grid", "creature", "gameobject", "taxinode", "trigger", "graveyard",
                             "quest"});
    }
    if (verb == "tele") return args == 1 && !isOneOf(sub, {"name", "group", "add", "del"});
    if (verb == "gm") return isOneOf(sub, {"on", "off", "fly", "visible"}) && args <= 2;
    if (verb == "modify" || verb == "mod") {
        return isOneOf(sub, {"speed", "hp", "mana", "energy", "rage", "money", "scale", "mount", "faction",
                             "reputation", "honor", "talentpoints", "runicpower", "standstate", "phase", "drunk"});
    }
    if (verb == "quest") {
        return isOneOf(sub, {"add", "complete", "remove", "reward"}) && args == 2 && isNumberOrLink(w[2]);
    }
    if (verb == "npc") return isOneOf(sub, {"info", "near"});
    if (verb == "gobject" || verb == "gobj") return isOneOf(sub, {"info", "near", "target"});
    if (verb == "cast") return isNumberOrLink(sub) || isOneOf(sub, {"self", "back"});
    if (verb == "lookup") {
        // Not `lookup player`, which answers with accounts and addresses.
        return isOneOf(sub, {"item", "spell", "quest", "creature", "object", "gobject", "area", "tele", "faction",
                             "skill", "map", "event", "itemset", "taxinode", "title"});
    }
    if (verb == "server") return isOneOf(sub, {"info", "motd"});
    if (verb == "instance") return isOneOf(sub, {"listbinds", "unbind"});
    return false;
}

/// A command's words, in lower case, past its '.' or '!'.
std::vector<std::string> gmWords(std::string_view text) {
    std::vector<std::string> words;
    size_t at = 1;
    while (at < text.size() && words.size() < 16) {
        while (at < text.size() && text[at] == ' ') ++at;
        std::string word;
        while (at < text.size() && text[at] != ' ') word += text[at++];
        if (!word.empty()) words.push_back(lower(word));
    }
    return words;
}

}  // namespace

bool gmCommandOnlyTells(std::string_view text) {
    const std::vector<std::string> words = gmWords(text);
    if (words.empty()) return false;
    const std::string sub = words.size() > 1 ? words[1] : std::string();
    if (isOneOf(words[0], {"gps", "lookup", "distance", "help", "commands", "server"})) return true;
    if (words[0] == "npc") return isOneOf(sub, {"info", "near"});
    if (words[0] == "gobject" || words[0] == "gobj") return isOneOf(sub, {"info", "near", "target"});
    return false;
}

std::string popupPressRefusal(const std::string& name, const std::string& popup, const std::string& which,
                              const std::string& expected) {
    if (popup.empty()) return {};
    if (expected.empty()) {
        return name + " is a button of the dialog " + popup + " (" + (which.empty() ? "hidden" : which) +
               "): give popup=<its which> to press it";
    }
    if (which != expected) {
        return popup + " shows " + (which.empty() ? std::string("nothing") : which) + ", not " + expected +
               ": not pressed";
    }
    return {};
}

std::optional<uint64_t> selectionOf(std::string_view opcode, std::span<const uint8_t> payload) {
    if (opcode != "CMSG_SET_SELECTION") return std::nullopt;
    Reader r(payload);
    const uint64_t guid = r.u64();
    // One that cannot be read selects somebody, as far as anybody here knows.
    return r.ok() ? guid : ~uint64_t{0};
}

Verdict judgeGmCommand(std::string_view text, const GuardContext& context) {
    const std::vector<std::string> words = gmWords(text);
    if (words.empty()) return never("an empty command");
    // Where somebody else is selected, a command is about them.
    const std::array<uint64_t, 2> selected = {context.targetGuid, context.selectedOnRealm};
    for (const uint64_t guid : selected) {
        if (aimsAtOther(guid, context)) {
            return never("a command would be about the selected player; clear the target first");
        }
    }
    // Nor about anything else that is selected: a creature takes an aura, a
    // morph or a blow as a player does.
    if (!gmCommandOnlyTells(text)) {
        for (const uint64_t guid : selected) {
            if (guid != 0 && guid != context.ownGuid) {
                return never("the command ." + words[0] +
                             " would be about what is selected; select the character itself (on=self) or clear the "
                             "target first");
            }
        }
    }
    if (!gmWordsAllowed(words, context.ownName, context.ownGuildName)) {
        return never("the command ." + words[0] + (words.size() > 1 ? " " + words[1] : std::string()) +
                     " is not one the MCP server gives like that (docs/MCP_SERVER.md lists them)");
    }
    return {};
}

Verdict judgeChatText(std::string_view text, bool luaAllowed) {
    size_t at = 0;
    while (at < text.size() && text[at] == ' ') ++at;
    if (at >= text.size() || text[at] != '/') return {};
    std::string verb;
    for (size_t i = at + 1; i < text.size() && text[i] != ' '; ++i) verb += text[i];
    verb = lower(verb);
    if (!luaAllowed && isOneOf(verb, {"run", "script", "dump", "tinspect", "eventtrace", "etrace"})) {
        return never("/" + verb + " runs Lua, which the server was not started for (--mcp-lua)");
    }
    if (isOneOf(verb, {"quit", "exit"})) return never("/" + verb + " ends the client; the quit tool does that");
    if (isOneOf(verb, {"delete", "deletechar"})) return never("/" + verb + " cannot be had back");
    return {};
}

}  // namespace wowee::mcp
