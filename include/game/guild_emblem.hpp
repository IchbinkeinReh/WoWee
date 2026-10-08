#pragma once

/// A guild's tabard design, as SMSG_GUILD_QUERY_RESPONSE gives it and the
/// client's guild cache keeps it (+0x2e4..+0x2f4 of its record, 0x007eada0).

#include <cstdint>
#include <optional>

namespace wowee::game {

struct GuildEmblem {
    uint32_t emblemStyle = 0xFFFFFFFFu;
    uint32_t emblemColor = 0xFFFFFFFFu;
    uint32_t borderStyle = 0xFFFFFFFFu;
    uint32_t borderColor = 0xFFFFFFFFu;
    uint32_t backgroundColor = 0xFFFFFFFFu;

    /// 0x007eada0 gives the design only when none of the five is -1, the
    /// cache record's value before the answer.
    [[nodiscard]] constexpr bool complete() const {
        return emblemStyle != 0xFFFFFFFFu && emblemColor != 0xFFFFFFFFu && borderStyle != 0xFFFFFFFFu &&
               borderColor != 0xFFFFFFFFu && backgroundColor != 0xFFFFFFFFu;
    }
    bool operator==(const GuildEmblem&) const = default;
};

/// Whether a tabard painted for `paintedGuild` with `paintedEmblem` is
/// painted again: its wearer's guild has changed (0x006e1bb0), or the
/// guild's design is known and is not the one it wears (0x006d2840, and
/// 0x006e1b40 once a guild asked for again has answered).
constexpr bool tabardNeedsRepaint(uint32_t paintedGuild, const std::optional<GuildEmblem>& paintedEmblem,
                                  uint32_t guild, const std::optional<GuildEmblem>& emblem) {
    if (guild != paintedGuild) return true;
    return guild != 0 && emblem.has_value() && emblem != paintedEmblem;
}

}  // namespace wowee::game
