#pragma once

/// A guild's tabard design, as SMSG_GUILD_QUERY_RESPONSE gives it and the
/// client's guild cache keeps it (+0x2e4..+0x2f4 of its record, 0x007eada0).

#include <cstdint>

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

}  // namespace wowee::game
