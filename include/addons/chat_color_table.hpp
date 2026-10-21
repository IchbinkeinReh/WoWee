#pragma once

// The client's chat type colour table (0x00b746e8): sixty-two fixed types and
// the ten numbered channels (0x00bcf0a4), each with a colour and whether names
// in it are coloured by class. ChangeChatColor, SetChatColorNameByClass and
// ResetChatColors change it (0x004feaf0, 0x004fedc0, 0x004fed10); the client
// keeps it in chat-cache.txt's COLORS section (0x004fd300) and tells the
// interface every entry when it loads (UPDATE_CHAT_COLOR and
// UPDATE_CHAT_COLOR_NAME_BY_CLASS, 0x00501f00).

#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace wowee::addons {

class ChatColorTable {
public:
    struct Entry {
        std::string name;        ///< +0x00
        uint8_t r = 0, g = 0, b = 0;  ///< +0x40
        bool nameByClass = false;     ///< +0x48
    };

    static constexpr int kFixed = 62;
    static constexpr int kChannels = 10;

    ChatColorTable() { setDefaults(); }

    /// 0x00501f00's colours: the table at 0x009fa558, and CHANNEL1..10 in
    /// CHANNEL's colour and class flag.
    void setDefaults() {
        entries_.clear();
        for (const auto& d : defaults()) entries_.push_back({d.name, d.r, d.g, d.b, false});
        const Entry channel = entries_[kChannelRow];
        for (int i = 1; i <= kChannels; ++i) {
            entries_.push_back({"CHANNEL" + std::to_string(i), channel.r, channel.g, channel.b,
                                channel.nameByClass});
        }
    }

    /// ResetChatColors (0x004fed10): every fixed type back to its colour, and
    /// the channels to CHANNEL's. The class flags are left as they are.
    void resetColors() {
        for (int i = 0; i < kFixed; ++i) {
            const auto& d = defaults()[static_cast<size_t>(i)];
            entries_[static_cast<size_t>(i)].r = d.r;
            entries_[static_cast<size_t>(i)].g = d.g;
            entries_[static_cast<size_t>(i)].b = d.b;
        }
        const Entry& channel = entries_[kChannelRow];
        for (size_t i = kFixed; i < entries_.size(); ++i) {
            entries_[i].r = channel.r;
            entries_[i].g = channel.g;
            entries_[i].b = channel.b;
        }
    }

    /// ChangeChatColor's write: the named type, case aside, takes the colour.
    /// False when no entry has that name (nothing is said then).
    bool setColor(const std::string& name, uint8_t r, uint8_t g, uint8_t b) {
        bool found = false;
        for (auto& e : entries_) {
            if (!sameName(e.name, name)) continue;
            e.r = r;
            e.g = g;
            e.b = b;
            found = true;
        }
        return found;
    }

    bool setNameByClass(const std::string& name, bool on) {
        bool found = false;
        for (auto& e : entries_) {
            if (!sameName(e.name, name)) continue;
            e.nameByClass = on;
            found = true;
        }
        return found;
    }

    [[nodiscard]] const std::vector<Entry>& entries() const { return entries_; }

    /// The COLORS section as 0x004fd300 writes it: "NAME r g b Y|N", one per
    /// entry, and END.
    [[nodiscard]] std::string serialize() const {
        std::string out = "COLORS\n";
        for (const auto& e : entries_) {
            out += e.name + " " + std::to_string(e.r) + " " + std::to_string(e.g) + " " +
                   std::to_string(e.b) + " " + (e.nameByClass ? "Y" : "N") + "\n";
        }
        out += "END\n";
        return out;
    }

    /// Read a COLORS section back over the defaults. Lines that name no entry
    /// are passed over.
    void parse(const std::string& text) {
        bool inColors = false;
        size_t pos = 0;
        while (pos < text.size()) {
            size_t eol = text.find('\n', pos);
            if (eol == std::string::npos) eol = text.size();
            std::string line = text.substr(pos, eol - pos);
            pos = eol + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line == "COLORS") { inColors = true; continue; }
            if (line == "END") { inColors = false; continue; }
            if (!inColors || line.empty()) continue;
            char name[65] = {};
            int r = 0, g = 0, b = 0;
            char flag = 'N';
            if (std::sscanf(line.c_str(), "%64s %d %d %d %c", name, &r, &g, &b, &flag) < 4) continue;
            setColor(name, static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b));
            setNameByClass(name, flag == 'Y');
        }
    }

private:
    struct Default { const char* name; uint8_t r, g, b; };
    static constexpr int kChannelRow = 17;

    static bool sameName(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            const auto la = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
            const auto lb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
            if (la != lb) return false;
        }
        return true;
    }

    /// 0x009fa558, read out of the 12340 binary: 0x4c bytes a row, the name
    /// and then the colour.
    static const std::array<Default, kFixed>& defaults() {
        static const std::array<Default, kFixed> kDefaults = {{
            {"SYSTEM", 255, 255, 0}, {"SAY", 255, 255, 255}, {"PARTY", 170, 170, 255},
            {"RAID", 255, 127, 0}, {"GUILD", 64, 255, 64}, {"OFFICER", 64, 192, 64},
            {"YELL", 255, 64, 64}, {"WHISPER", 255, 128, 255}, {"WHISPER_FOREIGN", 255, 128, 255},
            {"WHISPER_INFORM", 255, 128, 255}, {"EMOTE", 255, 128, 64}, {"TEXT_EMOTE", 255, 128, 64},
            {"MONSTER_SAY", 255, 255, 159}, {"MONSTER_PARTY", 170, 170, 255},
            {"MONSTER_YELL", 255, 64, 64}, {"MONSTER_WHISPER", 255, 181, 235},
            {"MONSTER_EMOTE", 255, 128, 64}, {"CHANNEL", 255, 192, 192},
            {"CHANNEL_JOIN", 192, 128, 128}, {"CHANNEL_LEAVE", 192, 128, 128},
            {"CHANNEL_LIST", 192, 128, 128}, {"CHANNEL_NOTICE", 192, 192, 192},
            {"CHANNEL_NOTICE_USER", 192, 192, 192}, {"AFK", 255, 128, 255}, {"DND", 255, 128, 255},
            {"IGNORED", 255, 0, 0}, {"SKILL", 85, 85, 255}, {"LOOT", 0, 170, 0},
            {"MONEY", 255, 255, 0}, {"OPENING", 128, 128, 255}, {"TRADESKILLS", 255, 255, 255},
            {"PET_INFO", 128, 128, 255}, {"COMBAT_MISC_INFO", 128, 128, 255},
            {"COMBAT_XP_GAIN", 111, 111, 255}, {"COMBAT_HONOR_GAIN", 224, 202, 10},
            {"COMBAT_FACTION_CHANGE", 128, 128, 255}, {"BG_SYSTEM_NEUTRAL", 255, 120, 10},
            {"BG_SYSTEM_ALLIANCE", 0, 174, 239}, {"BG_SYSTEM_HORDE", 255, 0, 0},
            {"RAID_LEADER", 255, 72, 9}, {"RAID_WARNING", 255, 72, 0},
            {"RAID_BOSS_EMOTE", 255, 221, 0}, {"RAID_BOSS_WHISPER", 255, 221, 0},
            {"FILTERED", 255, 0, 0}, {"BATTLEGROUND", 255, 127, 0},
            {"BATTLEGROUND_LEADER", 255, 219, 183}, {"RESTRICTED", 255, 0, 0},
            {"BATTLENET", 255, 255, 255}, {"ACHIEVEMENT", 255, 255, 0},
            {"GUILD_ACHIEVEMENT", 64, 255, 64}, {"ARENA_POINTS", 255, 255, 255},
            {"PARTY_LEADER", 118, 200, 255}, {"TARGETICONS", 255, 255, 0},
            {"BN_WHISPER", 0, 255, 246}, {"BN_WHISPER_INFORM", 0, 255, 246},
            {"BN_CONVERSATION", 0, 177, 240}, {"BN_CONVERSATION_NOTICE", 0, 177, 240},
            {"BN_CONVERSATION_LIST", 0, 177, 240}, {"BN_INLINE_TOAST_ALERT", 130, 197, 255},
            {"BN_INLINE_TOAST_BROADCAST", 130, 197, 255},
            {"BN_INLINE_TOAST_BROADCAST_INFORM", 130, 197, 255},
            {"BN_INLINE_TOAST_CONVERSATION", 130, 197, 255},
        }};
        return kDefaults;
    }

    std::vector<Entry> entries_;
};

/// The one table the interface's calls and the load and save share.
inline ChatColorTable& chatColorTable() {
    static ChatColorTable table;
    return table;
}

}  // namespace wowee::addons
