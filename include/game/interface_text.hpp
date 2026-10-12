#pragma once

/**
 * interface_text.hpp - the client's own lines, worded by the interface.
 *
 * The real client words what it prints about loot, money and rolls in C, from
 * the interface's GlobalStrings (FrameScript_GetText, 0x00819d40), and hands
 * the finished line to the chat frame as a CHAT_MSG_* event (0x00509dd0). This
 * client printed its own English instead - "Looted: 5c", "Received item:" -
 * on a German client. The pieces here pick the same strings the original
 * picks, from the same packet fields, so the words are the interface's and
 * only the choosing is ours.
 *
 * The choices are kept apart from the lookup so they can be tested without an
 * interface: every function takes the text source as an argument.
 */

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "game/chat_channels.hpp"

namespace wowee {
namespace game {

/// One of the interface's global strings, or `enUS` where there is no
/// interface or no such string (FrameScript_GetText, 0x00819d40).
std::string interfaceString(const char* key, const char* enUS);

/// That string with its arguments put in, as string.format would.
inline std::string interfaceFormat(const char* key, const char* enUS,
                                   const std::vector<std::string>& args) {
    return chat_channels::luaFormat(interfaceString(key, enUS), args);
}

namespace interface_text {

/// Where a format comes from: a key and its enUS wording, answered with the
/// localized text.
using TextSource = std::function<std::string(const char* key, const char* enUS)>;

/// An amount of money as 0x007e7d80 writes it: GOLD_AMOUNT, SILVER_AMOUNT and
/// COPPER_AMOUNT for the coins there are, highest first, joined by ", " - "1
/// Gold, 5 Kupfer". A coin of zero is left out; nothing at all is empty.
inline std::string moneyText(uint64_t copper, const TextSource& text) {
    const uint64_t gold = copper / 10000;
    const uint64_t silver = (copper / 100) % 100;
    const uint64_t cop = copper % 100;
    std::string out;
    auto add = [&](uint64_t n, const char* key, const char* enUS) {
        if (n == 0) return;
        if (!out.empty()) out += ", ";
        out += chat_channels::luaFormat(text(key, enUS), {std::to_string(n)});
    };
    add(gold, "GOLD_AMOUNT", "%d Gold");
    add(silver, "SILVER_AMOUNT", "%d Silver");
    add(cop, "COPPER_AMOUNT", "%d Copper");
    return out;
}

/// SMSG_LOOT_MONEY_NOTIFY's line (0x006ceca0): YOU_LOOT_MONEY for the one
/// looter, LOOT_MONEY_SPLIT for a share of a group's - CHAT_MSG_MONEY.
inline std::string lootMoneyLine(uint64_t copper, bool soleLooter, const TextSource& text) {
    const std::string amount = moneyText(copper, text);
    if (amount.empty()) return {};
    return soleLooter
        ? chat_channels::luaFormat(text("YOU_LOOT_MONEY", "You loot %s"), {amount})
        : chat_channels::luaFormat(text("LOOT_MONEY_SPLIT", "Your share of the loot is %s."), {amount});
}

/// SMSG_ITEM_PUSH_RESULT's fields as 0x00523900 reads them.
struct ItemPush {
    bool self = true;          ///< the item went to this player
    bool received = false;     ///< from an NPC or a quest rather than looted
    bool created = false;      ///< made by a spell - a craft, a conjure
    bool showInChat = true;    ///< the server's say on whether to print it at all
    uint32_t count = 1;
};

/// The line 0x00523900 prints for an item push, CHAT_MSG_LOOT, or empty when
/// it prints none. `link` is the item's chat link, `who` the receiver's name
/// for someone else's.
///
/// For this player: LOOT_ITEM_SELF for loot, LOOT_ITEM_PUSHED_SELF for an item
/// handed over, LOOT_ITEM_CREATED_SELF for one made, each _MULTIPLE for more
/// than one. For a party member only loot and creations are announced -
/// LOOT_ITEM or CREATED_ITEM - and an item handed to them is not.
inline std::string itemPushLine(const ItemPush& p, const std::string& link, const std::string& who,
                                const TextSource& text) {
    if (!p.showInChat) return {};
    const bool many = p.count >= 2;
    const std::string n = std::to_string(p.count);
    if (p.self) {
        if (p.created) {
            return many ? chat_channels::luaFormat(text("LOOT_ITEM_CREATED_SELF_MULTIPLE", "You create: %sx%d."), {link, n})
                        : chat_channels::luaFormat(text("LOOT_ITEM_CREATED_SELF", "You create: %s."), {link});
        }
        if (p.received) {
            return many ? chat_channels::luaFormat(text("LOOT_ITEM_PUSHED_SELF_MULTIPLE", "You receive item: %sx%d."), {link, n})
                        : chat_channels::luaFormat(text("LOOT_ITEM_PUSHED_SELF", "You receive item: %s."), {link});
        }
        return many ? chat_channels::luaFormat(text("LOOT_ITEM_SELF_MULTIPLE", "You receive loot: %sx%d."), {link, n})
                    : chat_channels::luaFormat(text("LOOT_ITEM_SELF", "You receive loot: %s."), {link});
    }
    if (p.received && !p.created) return {};
    if (p.created) {
        return many ? chat_channels::luaFormat(text("CREATED_ITEM_MULTIPLE", "%s creates: %sx%d."), {who, link, n})
                    : chat_channels::luaFormat(text("CREATED_ITEM", "%s creates: %s."), {who, link});
    }
    return many ? chat_channels::luaFormat(text("LOOT_ITEM_MULTIPLE", "%s receives loot: %sx%d."), {who, link, n})
                : chat_channels::luaFormat(text("LOOT_ITEM", "%s receives loot: %s."), {who, link});
}

/// One SMSG_LOOT_ROLL as 0x006fa840 reads it. The server sends one when a
/// player chooses - need as type 0 with roll 0, pass as type 0 with a
/// negative roll, greed and disenchant as their type with a negative roll -
/// and one per die cast once everyone has, with the type the die was for.
struct LootRoll {
    bool self = false;
    int8_t rollNumber = 0;
    uint8_t rollType = 0;      ///< 0 pass or need, 1 need, 2 greed, 3 disenchant
    bool autoPass = false;
    bool female = false;       ///< the roller, for LOOT_ROLL_PASSED_AUTO_FEMALE
    bool lootSpam = true;      ///< showLootSpam
};

/// The CHAT_MSG_LOOT line 0x006fa840 prints for a roll, or empty. Without
/// showLootSpam only this player's own automatic pass is said.
inline std::string lootRollLine(const LootRoll& r, const std::string& link, const std::string& who,
                                const TextSource& text) {
    if (!r.lootSpam && !(r.self && r.autoPass)) return {};
    const auto f = [&](const char* key, const char* enUS, const std::vector<std::string>& a) {
        return chat_channels::luaFormat(text(key, enUS), a);
    };
    const bool choice = r.rollNumber < 0;
    if (choice || r.rollType == 0) {
        if (r.rollType == 2 && choice) {
            return r.self ? f("LOOT_ROLL_GREED_SELF", "You have selected Greed for: %s", {link})
                          : f("LOOT_ROLL_GREED", "%s has selected Greed for: %s", {who, link});
        }
        if (r.rollType == 3 && choice) {
            return r.self ? f("LOOT_ROLL_DISENCHANT_SELF", "You have selected Disenchant for: %s", {link})
                          : f("LOOT_ROLL_DISENCHANT", "%s has selected Disenchant for: %s", {who, link});
        }
        if (r.rollType == 0 && !choice) {
            return r.self ? f("LOOT_ROLL_NEED_SELF", "You have selected Need for: %s", {link})
                          : f("LOOT_ROLL_NEED", "%s has selected Need for: %s", {who, link});
        }
        if (choice) {
            if (r.self) {
                return r.autoPass
                    ? f("LOOT_ROLL_PASSED_SELF_AUTO", "You automatically passed on: %s because you cannot loot that item.", {link})
                    : f("LOOT_ROLL_PASSED_SELF", "You passed on: %s", {link});
            }
            if (r.autoPass) {
                return r.female
                    ? f("LOOT_ROLL_PASSED_AUTO_FEMALE", "%1$s automatically passed on: %2$s because she cannot loot that item.", {who, link})
                    : f("LOOT_ROLL_PASSED_AUTO", "%1$s automatically passed on: %2$s because he cannot loot that item.", {who, link});
            }
            return f("LOOT_ROLL_PASSED", "%s passed on: %s", {who, link});
        }
    }
    const std::string n = std::to_string(static_cast<int>(r.rollNumber));
    if (r.rollType == 2) return f("LOOT_ROLL_ROLLED_GREED", "Greed Roll - %d for %s by %s", {n, link, who});
    if (r.rollType == 3) return f("LOOT_ROLL_ROLLED_DE", "Disenchant Roll - %d for %s by %s", {n, link, who});
    return f("LOOT_ROLL_ROLLED_NEED", "Need Roll - %d for %s by %s", {n, link, who});
}

/// SMSG_LOOT_ROLL_WON's line (0x006fb700). With showLootSpam the plain
/// LOOT_ROLL_WON / LOOT_ROLL_YOU_WON; without it the _NO_SPAM line that names
/// the roll that won, and nothing for a roll type that has none.
inline std::string lootRollWonLine(bool self, int rollNumber, uint8_t rollType, bool lootSpam,
                                   const std::string& link, const std::string& who,
                                   const TextSource& text) {
    const auto f = [&](const char* key, const char* enUS, const std::vector<std::string>& a) {
        return chat_channels::luaFormat(text(key, enUS), a);
    };
    if (lootSpam) {
        return self ? f("LOOT_ROLL_YOU_WON", "You won: %s", {link})
                    : f("LOOT_ROLL_WON", "%s won: %s", {who, link});
    }
    const std::string n = std::to_string(rollNumber);
    if (self) {
        switch (rollType) {
            case 1: return f("LOOT_ROLL_YOU_WON_NO_SPAM_NEED", "You won: %2$s |cff818181(Need - %1$d)|r", {n, link});
            case 2: return f("LOOT_ROLL_YOU_WON_NO_SPAM_GREED", "You won: %2$s |cff818181(Greed - %1$d)|r", {n, link});
            case 3: return f("LOOT_ROLL_YOU_WON_NO_SPAM_DE", "You won: %2$s |cff818181(Disenchant - %1$d)|r", {n, link});
            default: return {};
        }
    }
    switch (rollType) {
        case 1: return f("LOOT_ROLL_WON_NO_SPAM_NEED", "%1$s won: %3$s |cff818181(Need - %2$d)|r", {who, n, link});
        case 2: return f("LOOT_ROLL_WON_NO_SPAM_GREED", "%1$s won: %3$s |cff818181(Greed - %2$d)|r", {who, n, link});
        case 3: return f("LOOT_ROLL_WON_NO_SPAM_DE", "%1$s won: %3$s |cff818181(Disenchant - %2$d)|r", {who, n, link});
        default: return {};
    }
}

}  // namespace interface_text
}  // namespace game
}  // namespace wowee
