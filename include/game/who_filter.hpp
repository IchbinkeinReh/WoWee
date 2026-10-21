#pragma once

/// What /who, SendWho("filter") and the Who window's search ask the server,
/// read out of the filter text as the client reads it (0x006b5630), and the
/// CMSG_WHO it becomes. Pure: the tags and the zone, race and class names
/// come in from the caller (GlobalStrings, AreaTable, ChrRaces, ChrClasses).

#include <cstdint>
#include <string>
#include <vector>

namespace wowee::game::who_filter {

/// CMSG_WHO's fields (0x006b5630 writes them in this order).
struct Request {
    uint32_t minLevel = 0;
    uint32_t maxLevel = 100;
    std::string name;   ///< the name tag's text, at most 47 bytes
    std::string guild;  ///< the guild tag's text, at most 47 bytes
    uint32_t raceMask = 0xFFFFFFFFu;
    uint32_t classMask = 0xFFFFFFFFu;
    std::vector<uint32_t> zones;         ///< at most 10
    std::vector<std::string> strings;    ///< the words left over, at most 4
};

/// WHO_TAG_NAME, _GUILD, _ZONE, _RACE and _CLASS: "n-", "g-", "z-", "r-",
/// "c-" in English, the client's own language's otherwise.
struct Tags {
    std::string name = "n-";
    std::string guild = "g-";
    std::string zone = "z-";
    std::string race = "r-";
    std::string cls = "c-";
};

/// A row of AreaTable (a zone: no parent), ChrRaces or ChrClasses by its
/// localized name.
struct Named {
    uint32_t id = 0;
    std::string name;
};

/// Lower case for the comparisons, as the client's (0x0076ea40): ASCII and
/// the Latin-1 letters UTF-8 writes as 0xC3 0x80-0x9E, which is where German
/// and French put theirs.
inline std::string fold(const std::string& s) {
    std::string out = s;
    for (size_t i = 0; i < out.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(out[i]);
        if (c >= 'A' && c <= 'Z') {
            out[i] = static_cast<char>(c + 32);
        } else if (c == 0xC3 && i + 1 < out.size()) {
            unsigned char n = static_cast<unsigned char>(out[i + 1]);
            if (n >= 0x80 && n <= 0x9E && n != 0x97) out[i + 1] = static_cast<char>(n + 0x20);
            ++i;
        }
    }
    return out;
}

/// Whether `text` starts with `prefix`, case folded.
inline bool startsWithFolded(const std::string& text, const std::string& prefix) {
    if (prefix.empty() || text.size() < prefix.size()) return false;
    return fold(text.substr(0, prefix.size())) == fold(prefix);
}

/// Whether `needle` is somewhere in `hay`, case folded (0x0076f7e0).
inline bool containsFolded(const std::string& hay, const std::string& needle) {
    return fold(hay).find(fold(needle)) != std::string::npos;
}

/// The filter as 0x006b5630 reads it. Words split at white space, a quote
/// keeping spaces in one; each word, at most 127 bytes:
///   - a tag: its text after the tag. A zone takes every zone whose name
///     holds it (up to ten), or zone 0 - none - where none does; a race or a
///     class every row whose name holds it, its bit 1 << id;
///   - a level: "80" is 80 to 80, "70-80" 70 to 80, "70-" 70 to 100,
///     "-80" 0 to 80;
///   - anything else a word for the server to look for in a name, guild or
///     zone. The fourth such word ends the filter: the rest is not read.
inline Request parse(const std::string& text, const Tags& tags, const std::vector<Named>& zones,
                     const std::vector<Named>& races, const std::vector<Named>& classes) {
    constexpr size_t kWordMax = 127;   // a word's 128-byte slot
    constexpr size_t kNameMax = 47;    // the name's and guild's 48
    constexpr size_t kMaxZones = 10;
    constexpr size_t kMaxStrings = 4;
    Request r;
    const auto isSpace = [](char c) { return c == ' ' || (c >= '\t' && c <= '\r'); };
    const auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
    const auto number = [&](const std::string& s, size_t at) {
        uint32_t v = 0;
        while (at < s.size() && isDigit(s[at])) v = v * 10 + static_cast<uint32_t>(s[at++] - '0');
        return v;
    };
    // The words, quotes keeping spaces in one and dropped themselves.
    std::vector<std::string> words;
    {
        std::string word;
        bool quoted = false, any = false;
        for (char c : text + " ") {
            if (c == '"') { quoted = !quoted; any = true; continue; }
            if (!quoted && isSpace(c)) {
                if (any) words.push_back(word.substr(0, kWordMax));
                word.clear();
                any = false;
                continue;
            }
            word += c;
            any = true;
        }
    }
    const auto byName = [&](const std::vector<Named>& rows, const std::string& part, uint32_t& mask) {
        if (mask == 0xFFFFFFFFu) mask = 0;
        for (const auto& row : rows) {
            if (containsFolded(row.name, part)) mask |= 1u << (row.id & 31u);
        }
    };
    for (const std::string& w : words) {
        if (startsWithFolded(w, tags.name)) {
            r.name = w.substr(tags.name.size(), kNameMax);
        } else if (startsWithFolded(w, tags.guild)) {
            r.guild = w.substr(tags.guild.size(), kNameMax);
        } else if (startsWithFolded(w, tags.zone)) {
            const std::string part = w.substr(tags.zone.size());
            const size_t before = r.zones.size();
            for (const auto& z : zones) {
                if (r.zones.size() >= kMaxZones) break;
                if (containsFolded(z.name, part)) r.zones.push_back(z.id);
            }
            if (r.zones.empty() && before == 0) r.zones.push_back(0);
        } else if (startsWithFolded(w, tags.race)) {
            byName(races, w.substr(tags.race.size()), r.raceMask);
        } else if (startsWithFolded(w, tags.cls)) {
            byName(classes, w.substr(tags.cls.size()), r.classMask);
        } else if (!w.empty() && (isDigit(w[0]) || w[0] == '-')) {
            // "80", "70-80", "70-", "-80"; "-" with no number after it is a
            // word like any other.
            size_t at = 1;
            bool level = false;
            if (isDigit(w[0])) {
                r.minLevel = number(w, 0);
                r.maxLevel = r.minLevel;
                level = true;
                while (at < w.size() && isDigit(w[at])) ++at;
                if (at < w.size() && w[at] == '-') {
                    r.maxLevel = 100;
                    ++at;
                }
            }
            if (at < w.size() && isDigit(w[at])) {
                r.maxLevel = number(w, at);
                level = true;
            }
            if (!level) {
                r.strings.push_back(w);
                if (r.strings.size() >= kMaxStrings) break;
            }
        } else {
            r.strings.push_back(w);
            if (r.strings.size() >= kMaxStrings) break;
        }
    }
    return r;
}

}  // namespace wowee::game::who_filter
