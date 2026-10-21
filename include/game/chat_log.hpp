#pragma once

/// A chat line as LoggingChat writes it to Logs\WoWChatLog.txt (ChatFrame.cpp
/// 0x00509dd0's end, and 0x00503660 for a text emote): by the line's type,
/// mostly the channel in brackets, the type's CHAT_<TYPE>_GET header with the
/// sender, a language header, and the text with its links made plain.

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace wowee::game::chat_log {

/// The client's chat type names (0x009fa558, 0x4c apart), by type.
inline constexpr std::array<const char*, 62> kTypeNames = {
    "SYSTEM", "SAY", "PARTY", "RAID", "GUILD", "OFFICER", "YELL", "WHISPER", "WHISPER_FOREIGN",
    "WHISPER_INFORM", "EMOTE", "TEXT_EMOTE", "MONSTER_SAY", "MONSTER_PARTY", "MONSTER_YELL",
    "MONSTER_WHISPER", "MONSTER_EMOTE", "CHANNEL", "CHANNEL_JOIN", "CHANNEL_LEAVE", "CHANNEL_LIST",
    "CHANNEL_NOTICE", "CHANNEL_NOTICE_USER", "AFK", "DND", "IGNORED", "SKILL", "LOOT", "MONEY", "OPENING",
    "TRADESKILLS", "PET_INFO", "COMBAT_MISC_INFO", "COMBAT_XP_GAIN", "COMBAT_HONOR_GAIN",
    "COMBAT_FACTION_CHANGE", "BG_SYSTEM_NEUTRAL", "BG_SYSTEM_ALLIANCE", "BG_SYSTEM_HORDE", "RAID_LEADER",
    "RAID_WARNING", "RAID_BOSS_EMOTE", "RAID_BOSS_WHISPER", "FILTERED", "BATTLEGROUND",
    "BATTLEGROUND_LEADER", "RESTRICTED", "BATTLENET", "ACHIEVEMENT", "GUILD_ACHIEVEMENT", "ARENA_POINTS",
    "PARTY_LEADER", "TARGETICONS", "BN_WHISPER", "BN_WHISPER_INFORM", "BN_CONVERSATION",
    "BN_CONVERSATION_NOTICE", "BN_CONVERSATION_LIST", "BN_INLINE_TOAST_ALERT", "BN_INLINE_TOAST_BROADCAST",
    "BN_INLINE_TOAST_BROADCAST_INFORM", "BN_INLINE_TOAST_CONVERSATION",
};

namespace type {
inline constexpr uint32_t kTextEmote = 0x0b;
inline constexpr uint32_t kChannelList = 0x14;
inline constexpr uint32_t kChannelNotice = 0x15;
inline constexpr uint32_t kChannelNoticeUser = 0x16;
inline constexpr uint32_t kIgnored = 0x19;
inline constexpr uint32_t kFiltered = 0x2b;
inline constexpr uint32_t kRestricted = 0x2e;
inline constexpr uint32_t kBnConversationNotice = 0x38;
}  // namespace type

struct Line {
    uint32_t type = 0;
    std::string text;
    std::string sender;
    /// The channel by name, and its number where the player has it (0 for
    /// one not joined): "%d. %s", or the name alone.
    std::string channel;
    int channelNumber = 0;
    /// The line's language by name where it is spoken in one the player does
    /// not take for granted, empty otherwise.
    std::string language;
    /// The second name of a channel notice.
    std::string target;
};

/// An interface string with its arguments put in, "" for one there is none of.
using Format = std::function<std::string(const std::string& key, const std::vector<std::string>& args)>;

/// The line's channel as 0x00509dd0 numbers it.
inline std::string numberedChannel(const Line& l) {
    if (l.channel.empty()) return {};
    if (l.channelNumber <= 0) return l.channel;
    return std::to_string(l.channelNumber) + ". " + l.channel;
}

/// The file writer's printf with the sender as its one argument
/// (0x00775a90): "%s" takes it, "%%" is a '%', anything else stays.
inline std::string withSender(const std::string& fmt, const std::string& sender) {
    std::string out;
    bool used = false;
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] == '%' && i + 1 < fmt.size()) {
            if (fmt[i + 1] == '%') {
                out += '%';
                ++i;
                continue;
            }
            if (fmt[i + 1] == 's' && !used) {
                out += sender;
                used = true;
                ++i;
                continue;
            }
        }
        out += fmt[i];
    }
    return out;
}

/// The types whose text keeps its '%' for the sender to go in (monster and
/// boss speech and emotes).
inline bool keepsPercent(uint32_t t) {
    return t == 0x0c || t == 0x0e || t == 0x29 || t == 0x2a || t == 0x10 || t == 0x0f || t == 0x0d;
}

/// The text with its links made plain - "|Hitem:...|h[Name]|h|r" becomes
/// "Name", "||" a '|' - and its '%' doubled unless the type keeps them.
inline std::string plainText(const std::string& text, uint32_t t) {
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '|') {
            if (i + 1 < text.size() && text[i + 1] == '|') {
                out += '|';
                i += 2;
                continue;
            }
            // From the next '[' to its ']', the brackets left out, and the
            // "|h" and "|r" after it.
            size_t open = text.find('[', i);
            if (open == std::string::npos) break;
            size_t j = open;
            for (; j < text.size() && text[j] != ']'; ++j) {
                if (text[j] != '[') out += text[j];
            }
            if (j >= text.size()) break;
            i = j + 1;
            if (text.compare(i, 2, "|h") == 0) i += 2;
            if (text.compare(i, 2, "|r") == 0) i += 2;
            continue;
        }
        if (c == '%' && !keepsPercent(t)) out += "%%";
        else out += c;
        ++i;
    }
    return out;
}

/// The line written to the file, nullopt for one not written.
inline std::optional<std::string> line(const Line& l, const Format& fmt) {
    const std::string channel = numberedChannel(l);
    switch (l.type) {
        case type::kTextEmote:
            // 0x00503660: the emote's own words.
            return l.text;
        case type::kChannelList: {
            const std::string head =
                channel.empty() ? std::string() : fmt("CHAT_CHANNEL_LIST_GET", {std::to_string(l.channelNumber), l.channel});
            return head + l.text;
        }
        case type::kChannelNotice:
        case type::kChannelNoticeUser:
            return fmt("CHAT_" + l.text + "_NOTICE", {channel, l.sender, l.target});
        case type::kIgnored:
            return fmt("CHAT_IGNORED", {l.sender});
        case type::kFiltered:
            return withSender(fmt("CHAT_FILTERED", {l.sender}), l.sender);
        case type::kRestricted:
            return fmt("CHAT_RESTRICTED", {});
        case type::kBnConversationNotice:
            return std::nullopt;
        default:
            break;
    }
    std::string prefix;
    if (!channel.empty()) {
        // "[1. General] ", the zone after " - " left off.
        prefix = "[" + channel + "] ";
        if (const size_t dash = prefix.find(" - "); dash != std::string::npos) prefix = prefix.substr(0, dash) + "] ";
    }
    const char* name = l.type < kTypeNames.size() ? kTypeNames[l.type] : "SYSTEM";
    std::string head = fmt(std::string("CHAT_") + name + "_GET", {l.sender});
    if (!l.language.empty()) head += "[" + l.language + "] ";
    return withSender(prefix + head + plainText(l.text, l.type), l.sender);
}

}  // namespace wowee::game::chat_log
