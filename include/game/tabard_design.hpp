#pragma once

/// The tabard designer's design as CGTabardModelFrame keeps it (+0x380..
/// +0x390): emblem style, emblem colour, border style, border colour and
/// background colour, in the order SMSG_GUILD_QUERY_RESPONSE and
/// MSG_SAVE_GUILD_EMBLEM carry them.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

#include "game/guild_emblem.hpp"

namespace wowee::game::tabard {

/// How many of each there are to cycle through (0x00a14908).
inline constexpr std::array<int32_t, 5> kVariationCounts = {170, 17, 6, 17, 51};

/// What the tabard vendor charges for a design (0x007eae60).
inline constexpr uint32_t kCreationCost = 100000;

struct Design {
    std::array<int32_t, 5> values{};

    [[nodiscard]] GuildEmblem emblem() const {
        return {.emblemStyle = static_cast<uint32_t>(values[0]),
                .emblemColor = static_cast<uint32_t>(values[1]),
                .borderStyle = static_cast<uint32_t>(values[2]),
                .borderColor = static_cast<uint32_t>(values[3]),
                .backgroundColor = static_cast<uint32_t>(values[4])};
    }
    static Design of(const GuildEmblem& e) {
        return {{static_cast<int32_t>(e.emblemStyle), static_cast<int32_t>(e.emblemColor),
                 static_cast<int32_t>(e.borderStyle), static_cast<int32_t>(e.borderColor),
                 static_cast<int32_t>(e.backgroundColor)}};
    }
    bool operator==(const Design&) const = default;
};

/// CycleVariation (0x00599470): one of the five, by its index from 0, moved
/// by `delta` round its count - nothing for a step as large as the count.
inline void cycle(Design& d, size_t index, int32_t delta) {
    if (index >= kVariationCounts.size()) return;
    const int32_t count = kVariationCounts[index];
    if (std::abs(delta) >= count) return;
    d.values[index] = (d.values[index] + count + delta) % count;
}

/// InitializeTabardColors (0x00599670): the guild's design where all five
/// are known, otherwise each picked at random from its count (`pick(n)` in
/// [0, n)).
template <typename Pick>
Design initial(const std::optional<GuildEmblem>& guild, Pick&& pick) {
    if (guild && guild->complete()) return Design::of(*guild);
    Design d;
    for (size_t i = 0; i < kVariationCounts.size(); ++i) d.values[i] = pick(kVariationCounts[i]);
    return d;
}

/// A game error the designer raises (all four kinds to the red error text,
/// 0x00ac8af0 type 2), by its interface string and its English.
struct Error {
    const char* key;
    const char* enUS;
    bool operator==(const Error& o) const { return std::string_view(key) == o.key; }
};
inline constexpr Error kInvalidColors{"ERR_GUILDEMBLEM_INVALID_TABARD_COLORS", "Invalid Guild Emblem colors."};
inline constexpr Error kNoGuild{"ERR_GUILDEMBLEM_NOGUILD", "You are not part of a guild!"};
inline constexpr Error kSame{"ERR_GUILDEMBLEM_SAME", "Not saved, your tabard is already like that."};
inline constexpr Error kNotGuildMaster{"ERR_GUILDEMBLEM_NOTGUILDMASTER", "Only guild leaders can create emblems."};
inline constexpr Error kNotEnoughMoney{"ERR_NOT_ENOUGH_MONEY", "You don't have enough money."};
inline constexpr Error kSaved{"ERR_GUILDEMBLEM_SUCCESS", "Guild Emblem saved."};
inline constexpr Error kCannotAfford{"ERR_GUILDEMBLEM_NOTENOUGHMONEY", "You can't afford to do that."};

/// Why MSG_SAVE_GUILD_EMBLEM is not sent (0x006db320), in the order it asks:
/// a value out of range, no guild known, the guild's own design, not its
/// master, not the money.
struct SaveCheck {
    std::optional<GuildEmblem> guild;  ///< the player's guild's design, nullopt when not known
    bool guildMaster = false;          ///< PLAYER_GUILDRANK 0
    uint64_t money = 0;
};
[[nodiscard]] inline std::optional<Error> saveRefusal(const Design& d, const SaveCheck& c) {
    const auto& v = d.values;
    for (int32_t x : v) {
        if (x < 0) return kInvalidColors;
    }
    if (v[0] > 169 || v[1] > 16 || v[2] > 5 || v[3] > 16 || v[4] > 50) return kInvalidColors;
    if (!c.guild) return kNoGuild;
    if (Design::of(*c.guild) == d) return kSame;
    if (!c.guildMaster) return kNotGuildMaster;
    if (c.money < kCreationCost) return kNotEnoughMoney;
    return std::nullopt;
}

/// MSG_SAVE_GUILD_EMBLEM's answer (0x006d9230): the codes below 6 are read,
/// each with the game error it raises (0x00ada2c8) - 5, the vendor refused,
/// with none. Anything else is ignored.
[[nodiscard]] inline bool knownSaveResult(uint32_t code) { return code < 6; }
[[nodiscard]] inline std::optional<Error> saveResultError(uint32_t code) {
    switch (code) {
        case 0: return kSaved;
        case 1: return kInvalidColors;
        case 2: return kNoGuild;
        case 3: return kNotGuildMaster;
        case 4: return kCannotAfford;
        default: return std::nullopt;
    }
}

/// The two halves of a part, as the designer's file name getters build
/// them (0x004e81c0, 0x004e8210): "TU" the upper, "TL" the lower.
[[nodiscard]] inline std::string backgroundFile(int32_t color, bool upper) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "Textures\\GuildEmblems\\Background_%02d_%s_U", color, upper ? "TU" : "TL");
    return buf;
}
[[nodiscard]] inline std::string emblemFile(int32_t style, int32_t color, bool upper) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "Textures\\GuildEmblems\\Emblem_%02d_%02d_%s_U", style, color, upper ? "TU" : "TL");
    return buf;
}

}  // namespace wowee::game::tabard
