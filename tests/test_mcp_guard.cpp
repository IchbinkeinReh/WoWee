// What the MCP server does not do towards other players, and at all.
//
// The packets are built by hand, byte for byte as the client sends them, so
// that a rule that reads past them or misses them shows here.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "game/opcode_table.hpp"
#include "mcp/options.hpp"
#include "mcp/origin.hpp"
#include "mcp/send_guard.hpp"

using namespace wowee::mcp;

namespace {

using wowee::game::LogicalOpcode;
struct OpcodeName {
    const char* name;
    LogicalOpcode op;
};
const OpcodeName kOpcodeNames[] = {
#include "game/opcode_names_generated.inc"
};

constexpr uint64_t kMe = 0x0000000000000042ULL;
constexpr uint64_t kOther = 0x0000000000000777ULL;
constexpr uint64_t kWolf = 0xF130000C9B00D6E2ULL;
constexpr uint64_t kOthersPet = 0xF140000C9B00D6E3ULL;

GuardContext context(uint32_t allow = 0) {
    GuardContext c;
    c.ownName = "Locki";
    c.ownGuid = kMe;
    c.allow = allow;
    c.belongsToOtherPlayer = [](uint64_t guid) { return guid == kOthersPet; };
    return c;
}

struct Bytes {
    std::vector<uint8_t> b;
    Bytes& u8(uint8_t v) {
        b.push_back(v);
        return *this;
    }
    Bytes& u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
        return *this;
    }
    Bytes& u64(uint64_t v) {
        for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
        return *this;
    }
    Bytes& packed(uint64_t v) {
        uint8_t mask = 0;
        std::vector<uint8_t> bytes;
        for (int i = 0; i < 8; ++i) {
            const auto byte = static_cast<uint8_t>(v >> (8 * i));
            if (byte) {
                mask = static_cast<uint8_t>(mask | (1u << i));
                bytes.push_back(byte);
            }
        }
        b.push_back(mask);
        b.insert(b.end(), bytes.begin(), bytes.end());
        return *this;
    }
    Bytes& text(const std::string& s) {
        b.insert(b.end(), s.begin(), s.end());
        b.push_back(0);
        return *this;
    }
};

Bytes chat(uint32_t type, const std::string& whom, const std::string& what) {
    Bytes b;
    b.u32(type).u32(0);
    if (type == 7 || type == 17) b.text(whom);
    return b.text(what);
}

Bytes cast(uint32_t mask, uint64_t target) {
    Bytes b;
    b.u8(1).u32(7266).u8(0).u32(mask);
    if (mask & 0x2u) b.packed(target);
    return b;
}

bool sends(const char* opcode, const Bytes& bytes, const GuardContext& c = context()) {
    return judgeSend(opcode, bytes.b, c).allowed();
}

bool gives(const std::string& command, const GuardContext& c = context()) {
    return judgeGmCommand(command, c).allowed();
}

}  // namespace

TEST_CASE("nothing is said to anybody", "[mcp][guard]") {
    // say, party, raid, guild, officer, yell, emote, channel, raid leader,
    // raid warning, battleground, afk, dnd
    for (const uint32_t type : {1u, 2u, 3u, 4u, 5u, 6u, 10u, 17u, 39u, 40u, 44u, 23u, 24u}) {
        INFO(type);
        CHECK_FALSE(sends("CMSG_MESSAGECHAT", chat(type, "General", "hello")));
    }
    CHECK_FALSE(sends("CMSG_MESSAGECHAT", chat(7, "Somebody", "hello")));
    CHECK_FALSE(sends("CMSG_MESSAGECHAT", chat(7, "Lockin", "hello")));
    CHECK_FALSE(sends("CMSG_MESSAGECHAT", chat(7, "Lock", "hello")));
    CHECK_FALSE(sends("CMSG_MESSAGECHAT", chat(7, "", "hello")));
    CHECK_FALSE(sends("CMSG_MESSAGECHAT_SAY", chat(1, "", "hello")));
    // A command said aloud that the realm does not know is said aloud.
    CHECK_FALSE(sends("CMSG_MESSAGECHAT", chat(1, "", ".gps")));
}

TEST_CASE("a whisper to oneself goes out", "[mcp][guard]") {
    CHECK(sends("CMSG_MESSAGECHAT", chat(7, "Locki", "note")));
    CHECK(sends("CMSG_MESSAGECHAT", chat(7, "LOCKI", ".gps")));
    CHECK_FALSE(sends("CMSG_MESSAGECHAT", chat(7, "Locki", ".kick Somebody")));
    // With no name known there is nobody to be oneself.
    GuardContext nobody = context();
    nobody.ownName.clear();
    CHECK_FALSE(sends("CMSG_MESSAGECHAT", chat(7, "", "note"), nobody));
    // Cut short, it is not taken for one.
    Bytes cut;
    cut.u32(7).u32(0);
    CHECK_FALSE(sends("CMSG_MESSAGECHAT", cut));
    CHECK_FALSE(sends("CMSG_MESSAGECHAT", Bytes{}));
}

TEST_CASE("what is allowed is allowed by its own name only", "[mcp][guard]") {
    const auto chatAllowed = context(static_cast<uint32_t>(Social::Chat));
    CHECK(sends("CMSG_MESSAGECHAT", chat(1, "", "hello"), chatAllowed));
    CHECK_FALSE(sends("CMSG_SEND_MAIL", Bytes{}, chatAllowed));
    CHECK(sends("CMSG_SEND_MAIL", Bytes{}, context(static_cast<uint32_t>(Social::Mail))));
    const Verdict v = judgeSend("CMSG_GUILD_INVITE", {}, context());
    CHECK(v.kind == Verdict::Kind::Social);
    CHECK(v.social == Social::Guild);
    CHECK(v.why.find("--mcp-allow=guild") != std::string::npos);
}

TEST_CASE("each family is refused, and what only asks or declines is not", "[mcp][guard]") {
    for (const char* opcode :
         {"CMSG_EMOTE", "CMSG_TEXT_EMOTE", "CMSG_SEND_MAIL", "CMSG_MAIL_TAKE_ITEM", "CMSG_MAIL_RETURN_TO_SENDER",
          "CMSG_MAIL_DELETE", "CMSG_INITIATE_TRADE", "CMSG_BEGIN_TRADE", "CMSG_ACCEPT_TRADE", "CMSG_SET_TRADE_GOLD",
          "CMSG_AUCTION_SELL_ITEM", "CMSG_AUCTION_PLACE_BID", "CMSG_AUCTION_REMOVE_ITEM", "CMSG_GROUP_INVITE",
          "CMSG_GROUP_ACCEPT", "CMSG_GROUP_DISBAND", "CMSG_GROUP_UNINVITE_GUID", "MSG_RAID_READY_CHECK",
          "MSG_RAID_TARGET_UPDATE", "CMSG_LFG_JOIN", "CMSG_BATTLEMASTER_JOIN", "CMSG_BATTLEMASTER_JOIN_ARENA",
          "CMSG_BATTLEFIELD_PORT", "CMSG_LOOT_ROLL", "CMSG_LOOT_METHOD", "CMSG_LOOT_MASTER_GIVE", "MSG_RANDOM_ROLL",
          "MSG_MINIMAP_PING", "CMSG_PUSHQUESTTOPARTY", "CMSG_SUMMON_RESPONSE", "CMSG_GUILD_INVITE",
          "CMSG_GUILD_ACCEPT", "CMSG_GUILD_LEAVE", "CMSG_GUILD_DISBAND", "CMSG_GUILD_BANK_DEPOSIT_MONEY",
          "CMSG_GUILD_BANK_SWAP_ITEMS", "CMSG_GUILD_MOTD", "CMSG_PETITION_SIGN", "CMSG_PETITION_BUY",
          "CMSG_OFFER_PETITION", "CMSG_ARENA_TEAM_INVITE", "CMSG_CALENDAR_ADD_EVENT", "CMSG_CALENDAR_EVENT_INVITE",
          "CMSG_DUEL_ACCEPTED", "CMSG_CHANNEL_INVITE", "CMSG_CHANNEL_KICK", "CMSG_CHANNEL_PASSWORD",
          "CMSG_ADD_FRIEND", "CMSG_ADD_IGNORE", "CMSG_SET_CONTACT_NOTES", "CMSG_GRANT_LEVEL", "CMSG_GMTICKET_CREATE",
          "CMSG_COMPLAIN", "CMSG_BUG", "CMSG_REPORT_PVP_AFK", "CMSG_TOGGLE_PVP"}) {
        INFO(opcode);
        CHECK_FALSE(sends(opcode, Bytes{}));
        CHECK(sends(opcode, Bytes{}, context(kSocialAll)));
    }
    for (const char* opcode :
         {"CMSG_GUILD_ROSTER", "CMSG_GUILD_QUERY", "CMSG_GUILD_DECLINE", "CMSG_GROUP_DECLINE", "CMSG_CANCEL_TRADE",
          "CMSG_BUSY_TRADE", "CMSG_DUEL_CANCELLED", "CMSG_AUCTION_LIST_ITEMS", "MSG_AUCTION_HELLO",
          "CMSG_GET_MAIL_LIST", "CMSG_MAIL_MARK_AS_READ", "MSG_QUERY_NEXT_MAIL_TIME", "CMSG_LOOT_MONEY", "CMSG_LOOT",
          "CMSG_LOOT_RELEASE", "CMSG_AUTOSTORE_LOOT_ITEM", "CMSG_JOIN_CHANNEL", "CMSG_LEAVE_CHANNEL", "CMSG_WHO",
          "CMSG_CONTACT_LIST", "CMSG_INSPECT", "CMSG_LFG_GET_STATUS", "CMSG_GMTICKET_GETTICKET",
          "CMSG_PETITION_DECLINE", "CMSG_SET_SELECTION", "CMSG_GOSSIP_HELLO", "CMSG_QUESTGIVER_ACCEPT_QUEST",
          "CMSG_LIST_INVENTORY", "CMSG_BUY_ITEM", "CMSG_SELL_ITEM", "CMSG_AUTOEQUIP_ITEM", "CMSG_SWAP_ITEM",
          "CMSG_ACTIVATETAXI", "MSG_MOVE_HEARTBEAT", "MSG_MOVE_START_FORWARD", "CMSG_PING", "CMSG_TIME_SYNC_RESP",
          "CMSG_WARDEN_DATA", "CMSG_STANDSTATECHANGE", "CMSG_MOUNTSPECIAL_ANIM", "CMSG_PLAYER_LOGIN",
          "CMSG_LOGOUT_REQUEST", "CMSG_UPDATE_ACCOUNT_DATA", "CMSG_CAST_SPELL_UNKNOWN"}) {
        INFO(opcode);
        if (std::string(opcode) == "CMSG_PETITION_DECLINE") continue;  // MSG_PETITION_DECLINE is the packet's name
        CHECK(sends(opcode, Bytes{}));
    }
}

TEST_CASE("what cannot be had back is not done, whatever is allowed", "[mcp][guard]") {
    for (const char* opcode :
         {"CMSG_CHAR_DELETE", "CMSG_CHAR_CREATE", "CMSG_CHAR_RENAME", "CMSG_CHAR_CUSTOMIZE", "CMSG_CHAR_FACTION_CHANGE",
          "CMSG_CHAR_RACE_CHANGE", "CMSG_DESTROYITEM", "CMSG_PET_ABANDON", "CMSG_UNLEARN_SKILL",
          "CMSG_GM_NUKE_CHARACTER", "CMSG_CHEAT_SETMONEY", "CMSG_LEVEL_CHEAT", "CMSG_DEBUG_AISTATE",
          "CMSG_WORLD_TELEPORT", "CMSG_WHOIS", "MSG_GM_SUMMON", "CMSG_SERVER_COMMAND"}) {
        INFO(opcode);
        CHECK(judgeSend(opcode, {}, context(kSocialAll)).kind == Verdict::Kind::Never);
    }
    Bytes abandon;
    abandon.u64(0xF140000000000001ULL).u32(0x07000003u).u64(0);
    CHECK(judgeSend("CMSG_PET_ACTION", abandon.b, context(kSocialAll)).kind == Verdict::Kind::Never);
    Bytes attack;
    attack.u64(0xF140000000000001ULL).u32(0x07000002u).u64(kWolf);
    CHECK(sends("CMSG_PET_ACTION", attack));
}

TEST_CASE("spells, items and attacks are not aimed at other players", "[mcp][guard]") {
    CHECK(sends("CMSG_CAST_SPELL", cast(0, 0)));
    CHECK(sends("CMSG_CAST_SPELL", cast(2, kMe)));
    CHECK(sends("CMSG_CAST_SPELL", cast(2, kWolf)));
    CHECK_FALSE(sends("CMSG_CAST_SPELL", cast(2, kOther)));
    CHECK_FALSE(sends("CMSG_CAST_SPELL", cast(2, kOthersPet)));
    CHECK(sends("CMSG_CAST_SPELL", cast(2, kOther), context(static_cast<uint32_t>(Social::Players))));
    // Its target cut off, it is not let through for want of one.
    Bytes cut;
    cut.u8(1).u32(7266).u8(0).u32(2);
    CHECK_FALSE(sends("CMSG_CAST_SPELL", cut));
    CHECK_FALSE(sends("CMSG_CAST_SPELL", Bytes{}));

    Bytes item;
    item.u8(0).u8(3).u8(1).u32(7266).u64(0x4000000000000001ULL).u32(0).u8(0).u32(2).packed(kOther);
    CHECK_FALSE(sends("CMSG_USE_ITEM", item));
    Bytes own;
    own.u8(0).u8(3).u8(1).u32(433).u64(0x4000000000000001ULL).u32(0).u8(0).u32(0);
    CHECK(sends("CMSG_USE_ITEM", own));
    Bytes pet;
    pet.u64(0xF140000000000001ULL).u8(1).u32(5).u8(0).u32(2).packed(kOther);
    CHECK_FALSE(sends("CMSG_PET_CAST_SPELL", pet));

    Bytes swing;
    swing.u64(kOther);
    CHECK_FALSE(sends("CMSG_ATTACKSWING", swing));
    Bytes swingWolf;
    swingWolf.u64(kWolf);
    CHECK(sends("CMSG_ATTACKSWING", swingWolf));
    CHECK_FALSE(sends("CMSG_ATTACKSWING", Bytes{}));

    // On a wire whose casts are not read, who is selected stands for it.
    GuardContext old = context();
    old.wotlkWire = false;
    old.targetGuid = kOther;
    CHECK_FALSE(sends("CMSG_CAST_SPELL", cast(0, 0), old));
    old.targetGuid = kWolf;
    CHECK(sends("CMSG_CAST_SPELL", cast(0, 0), old));
}

TEST_CASE("a guid is a player's by its high part", "[mcp][guard]") {
    CHECK(isPlayerGuid(kMe));
    CHECK(isPlayerGuid(0x0000FFFFFFFFFFFFULL));
    CHECK_FALSE(isPlayerGuid(0));
    CHECK_FALSE(isPlayerGuid(kWolf));
    CHECK_FALSE(isPlayerGuid(0x4000000000000001ULL));
}

TEST_CASE("every opcode the rules name is one there is", "[mcp][guard]") {
    std::vector<std::string> known;
    for (const auto& o : kOpcodeNames) known.emplace_back(o.name);
    for (const auto name : namedOpcodes()) {
        INFO(name);
        CHECK(std::find(known.begin(), known.end(), std::string(name)) != known.end());
    }
}

TEST_CASE("every packet of other players' business is looked at", "[mcp][guard]") {
    // By its name: one that has these in it and goes out all the same is one
    // of the harmless ones listed here, or a rule is missing.
    const std::vector<std::string> harmless = {
        "CMSG_GUILD_QUERY", "CMSG_GUILD_ROSTER", "CMSG_GUILD_INFO", "CMSG_GUILD_DECLINE", "CMSG_GUILD_BANK_QUERY_TAB",
        "CMSG_GUILD_BANKER_ACTIVATE", "MSG_GUILD_PERMISSIONS", "MSG_GUILD_BANK_LOG_QUERY",
        "MSG_GUILD_BANK_MONEY_WITHDRAWN", "MSG_GUILD_EVENT_LOG_QUERY", "MSG_QUERY_GUILD_BANK_TEXT",
        "CMSG_PETITION_QUERY", "CMSG_PETITION_SHOWLIST", "CMSG_PETITION_SHOW_SIGNATURES", "MSG_PETITION_DECLINE",
        "CMSG_ARENA_TEAM_QUERY", "CMSG_ARENA_TEAM_ROSTER", "CMSG_ARENA_TEAM_DECLINE", "CMSG_CALENDAR_GET_CALENDAR",
        "CMSG_CALENDAR_GET_EVENT", "CMSG_CALENDAR_GET_NUM_PENDING", "CMSG_GROUP_DECLINE", "CMSG_GROUP_CANCEL",
        "CMSG_LFG_GET_STATUS", "CMSG_LFG_LEAVE", "CMSG_LFG_SET_ROLES", "CMSG_SEARCH_LFG_LEAVE",
        "CMSG_AUCTION_LIST_ITEMS", "CMSG_AUCTION_LIST_BIDDER_ITEMS", "CMSG_AUCTION_LIST_OWNER_ITEMS",
        "CMSG_AUCTION_LIST_PENDING_SALES", "MSG_AUCTION_HELLO", "CMSG_MAIL_MARK_AS_READ", "CMSG_GET_MAIL_LIST",
        "MSG_QUERY_NEXT_MAIL_TIME", "CMSG_DUEL_CANCELLED", "CMSG_CANCEL_TRADE", "CMSG_BUSY_TRADE",
        "CMSG_IGNORE_TRADE", "CMSG_CLEAR_TRADE_ITEM", "CMSG_UNACCEPT_TRADE", "MSG_RAID_READY_CHECK_FINISHED",
        "CMSG_REQUEST_PARTY_MEMBER_STATS", "CMSG_REQUEST_RAID_INFO", "CMSG_LFD_PARTY_LOCK_INFO_REQUEST",
        "MSG_NOTIFY_PARTY_SQUELCH", "CMSG_GROUPACTION_THROTTLED", "MSG_INSPECT_ARENA_TEAMS",
        "MSG_SET_RAID_DIFFICULTY", "CMSG_SET_ALLOW_LOW_LEVEL_RAID1", "CMSG_SET_ALLOW_LOW_LEVEL_RAID2",
        "CMSG_EXPIRE_RAID_INSTANCE", "CMSG_CANCEL_PENDING_LFG", "CMSG_DECLINE_LFG_MATCH",
        "CMSG_LFG_CLEAR_AUTOJOIN", "CMSG_CLEAR_LOOKING_FOR_GROUP", "MSG_LOOKING_FOR_GROUP",
        "CMSG_SET_ACTIVE_TALENT_GROUP_OBSOLETE"};
    for (const auto& o : kOpcodeNames) {
        const std::string n = o.name;
        if (n.rfind("SMSG", 0) == 0 || n.rfind("UMSG", 0) == 0) continue;
        const bool theirs = n.find("GUILD") != std::string::npos || n.find("GROUP") != std::string::npos ||
                            n.find("PARTY") != std::string::npos || n.find("RAID") != std::string::npos ||
                            n.find("TRADE") != std::string::npos || n.find("AUCTION") != std::string::npos ||
                            n.find("MAIL") != std::string::npos || n.find("DUEL") != std::string::npos ||
                            n.find("PETITION") != std::string::npos || n.find("ARENA_TEAM") != std::string::npos ||
                            n.find("CALENDAR") != std::string::npos || n.find("LFG") != std::string::npos;
        if (!theirs || std::find(harmless.begin(), harmless.end(), n) != harmless.end()) continue;
        INFO(n);
        CHECK_FALSE(sends(o.name, Bytes{}));
    }
}

TEST_CASE("the commands that are about the character itself are given", "[mcp][gm]") {
    for (const char* command :
         {".revive", ".gps", ".go xyz 1 2 3 0 1.5", ".go creature 1234", ".tele mulgore", ".gm on", ".gm off",
          ".gm fly on", ".modify speed 5", ".modify hp 100", ".additem 6948", ".learn 6603", ".levelup", ".die",
          ".dismount", ".cooldown", ".aura 12345", ".unaura all", ".lookup item Hearth", ".quest add 783",
          ".server info", ".revive Locki", "!gps"}) {
        INFO(command);
        CHECK(gives(command));
    }
}

TEST_CASE("the commands that reach further are not", "[mcp][gm]") {
    for (const char* command :
         {".", ". ", ".kick Other", ".ban account x 1d y", ".announce hi", ".account", ".account set password a b b",
          ".pinfo", ".pinfo Other", ".summon Other", ".appear Other", ".revive Other", ".gps Other", ".levelup Other",
          ".levelup 5", ".additem 6948 5", ".additem Other 6948", ".additemset 1", ".tele name Other mulgore",
          ".tele group mulgore", ".tele del mulgore", ".tele", ".send mail Other \"a\" \"b\"", ".send money",
          ".server shutdown 1", ".reload all", ".npc add 1", ".npc delete", ".gobject delete 1", ".gobject add 1",
          ".lookup player account x", ".lookup player ip 1.2.3.4", ".guild invite", ".group leader Other",
          ".character delete", ".character rename", ".mute Other", ".gm list", ".gm ingame", ".whispers on",
          ".cast dist 1", ".wchange 1 1", ".event start 1", ".instance reset", ".quest add 1 Other", ".modify gender 1",
          ".bf start 1", ".groupsummon Other", ".recall Other", ".unknown"}) {
        INFO(command);
        CHECK_FALSE(gives(command));
    }
}

TEST_CASE("no command is given with another player selected", "[mcp][gm]") {
    GuardContext c = context();
    c.targetGuid = kOther;
    CHECK_FALSE(gives(".revive", c));
    c.targetGuid = kOthersPet;
    CHECK_FALSE(gives(".die", c));
    c.targetGuid = kMe;
    CHECK(gives(".revive", c));
    c.targetGuid = kWolf;
    CHECK(gives(".die", c));
}

// The realm answers "player not found" to these with a creature selected.
TEST_CASE("the commands that want a player are about the character", "[mcp][gm]") {
    for (const char* command : {".learn 6603", ".LEARN all", ".unlearn 6603", ".levelup", ".maxskill", ".cooldown",
                                ".revive", ".additem 6948", ".quest add 783", ".save", ".combatstop",
                                ".modify money 100", ".mod honor 5", "!learn 1"}) {
        INFO(command);
        CHECK(gmCommandWantsPlayer(command));
        CHECK(gives(command));
    }
    // And these are about whatever is selected.
    for (const char* command : {".die", ".aura 12345", ".unaura all", ".morph 1", ".modify hp 100", ".modify speed 5",
                                ".npc info", ".gps", ".respawn", ".go xyz 1 2 3", ".", ". "}) {
        INFO(command);
        CHECK_FALSE(gmCommandWantsPlayer(command));
    }
}

TEST_CASE("what is typed into the chat box", "[mcp][guard]") {
    CHECK(judgeChatText("/sit", false).allowed());
    CHECK(judgeChatText("/cast Frostbolt", false).allowed());
    CHECK(judgeChatText("hello", false).allowed());  // the packet is what is refused
    CHECK(judgeChatText("12", false).allowed());
    for (const char* text : {"/run Quit()", "/script x=1", "  /RUN x=1", "/dump GetMoney()"}) {
        INFO(text);
        CHECK_FALSE(judgeChatText(text, false).allowed());
        CHECK(judgeChatText(text, true).allowed());
    }
    CHECK_FALSE(judgeChatText("/quit", true).allowed());
    CHECK_FALSE(judgeChatText("/exit", true).allowed());
}

TEST_CASE("the server's acts are the server's until a hand acts", "[mcp][origin]") {
    Origin o;
    CHECK_FALSE(o.isServer());
    o.actedByServer();
    CHECK(o.isServer());
    o.actedByHand();
    CHECK_FALSE(o.isServer());
    o.beginAct();
    o.actedByHand();  // while a key of the server's is down
    CHECK(o.isServer());
    o.endAct();
    CHECK(o.isServer());  // what comes of it later is the server's still
    o.actedByHand();
    CHECK_FALSE(o.isServer());
}

namespace {

Options parsed(std::vector<const char*> args) {
    args.insert(args.begin(), "wowee");
    return parseOptions(static_cast<int>(args.size()), args.data());
}

}  // namespace

TEST_CASE("without the option there is no server", "[mcp][options]") {
    CHECK_FALSE(parsed({}).enabled);
    CHECK_FALSE(parsed({"--fullscreen", "-psn_0_1", "", "mcp", "--mc", "--mcpx", "-mcp"}).enabled);
    CHECK_FALSE(parsed({"--mcp-lua", "--mcp-allow=all"}).enabled);
    CHECK_FALSE(parseOptions(0, nullptr).enabled);
}

TEST_CASE("the option, with a port and without", "[mcp][options]") {
    CHECK(parsed({"--mcp"}).enabled);
    CHECK(parsed({"--mcp"}).port == kDefaultPort);
    CHECK(parsed({"--mcp", "9000"}).port == 9000);
    CHECK(parsed({"--mcp=9001"}).port == 9001);
    CHECK(parsed({"--mcp=0"}).port == 0);
    CHECK(parsed({"--mcp", "--mcp-lua"}).lua);
    CHECK_FALSE(parsed({"--mcp"}).lua);
    CHECK(parsed({"--help"}).help);
    for (auto bad : {std::vector<const char*>{"--mcp=65536"}, {"--mcp=abc"}, {"--mcp="}, {"--mcp=-1"},
                     {"--mcp", "999999"}}) {
        const Options o = parsed(bad);
        INFO(bad.front());
        CHECK_FALSE(o.enabled);
        // "--mcp=" is no option of this server's, and has no problem to tell.
        if (std::string(bad.front()) != "--mcp=") CHECK_FALSE(o.problems.empty());
    }
}

TEST_CASE("what is allowed is read exactly, or nothing is", "[mcp][options]") {
    CHECK(parsed({"--mcp"}).allow == 0);
    CHECK(parsed({"--mcp", "--mcp-allow=chat,mail"}).allow ==
          (static_cast<uint32_t>(Social::Chat) | static_cast<uint32_t>(Social::Mail)));
    CHECK(parsed({"--mcp", "--mcp-allow", "duel"}).allow == static_cast<uint32_t>(Social::Duel));
    CHECK(parsed({"--mcp", "--mcp-allow=all"}).allow == kSocialAll);
    for (auto bad : {std::vector<const char*>{"--mcp", "--mcp-allow=everything"}, {"--mcp", "--mcp-allow=chat,"},
                     {"--mcp", "--mcp-allow"}, {"--mcp", "--mcp-allow", "--mcp-lua"}, {"--mcp", "--mcp-allow=Chat"}}) {
        const Options o = parsed(bad);
        CHECK_FALSE(o.enabled);
        CHECK_FALSE(o.problems.empty());
    }
}
