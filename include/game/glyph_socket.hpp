#pragma once

// Whether the glyph a pending spell applies fits a socket, and whether it can
// go in: GlyphMatchesSocket (0x005b7410) and the placement check of
// PlaceGlyphInSocket (0x0080c3e0).
//
// A glyph is an item whose spell carries SPELL_EFFECT_APPLY_GLYPH (74) with a
// GlyphProperties.dbc id in its EffectMiscValue, and whose Targets word asks
// for a glyph socket (0x20000). The socket's GlyphSlot.dbc id is in
// PLAYER_FIELD_GLYPH_SLOTS_1; the two rows' types (GlyphProperties +0x08,
// GlyphSlot +0x04) must agree.

#include <array>
#include <cstdint>
#include <functional>

namespace wowee::game::glyph_socket {

constexpr uint32_t kEffectApplyGlyph = 74;
/// The outstanding target flag a glyph spell waits on (0x00d3f4e0 bit 17).
constexpr uint32_t kTargetFlagGlyph = 0x20000;
constexpr int kSockets = 6;

// The cast results the check reports (SpellCastResult).
constexpr uint8_t kResultOk = 0;
constexpr uint8_t kResultInvalidGlyph = 0xaf;
constexpr uint8_t kResultUniqueGlyph = 0xb0;
constexpr uint8_t kResultGlyphSocketLocked = 0xb1;

/// A row's type: GlyphProperties by id, GlyphSlot by id. False when there is
/// no such row.
using RowType = std::function<bool(uint32_t id, uint32_t& type)>;

/// The player's sockets, from the three update fields.
struct Sockets {
    uint32_t enabledMask = 0;               ///< PLAYER_GLYPHS_ENABLED
    std::array<uint32_t, kSockets> slotIds{};  ///< PLAYER_FIELD_GLYPH_SLOTS_1..6
    std::array<uint32_t, kSockets> glyphs{};   ///< PLAYER_FIELD_GLYPHS_1..6
};

/// 0x005b7410: the first glyph effect whose two rows exist decides.
inline bool matches(const uint32_t (&effects)[3], const uint32_t (&misc)[3], uint32_t slotId,
                    const RowType& glyphType, const RowType& slotType) {
    for (int i = 0; i < 3; ++i) {
        if (effects[i] != kEffectApplyGlyph) continue;
        uint32_t gt = 0, st = 0;
        if (!glyphType(misc[i], gt) || !slotType(slotId, st)) continue;
        return gt == st;
    }
    return false;
}

/// 0x0080c3e0: a locked socket, a glyph of the other kind, or one already in
/// a socket is refused with its cast result; kResultOk when it can go in.
inline uint8_t placement(const uint32_t (&effects)[3], const uint32_t (&misc)[3], int socket,
                         const Sockets& sockets, const RowType& glyphType,
                         const RowType& slotType) {
    if (socket < 0 || socket >= kSockets) return kResultGlyphSocketLocked;
    if ((sockets.enabledMask & (1u << static_cast<uint32_t>(socket))) == 0) {
        return kResultGlyphSocketLocked;
    }
    for (int i = 0; i < 3; ++i) {
        if (effects[i] != kEffectApplyGlyph) continue;
        uint32_t gt = 0, st = 0;
        if (!glyphType(misc[i], gt) || !slotType(sockets.slotIds[static_cast<size_t>(socket)], st)) {
            continue;
        }
        if (gt != st) return kResultInvalidGlyph;
        for (const uint32_t inSocket : sockets.glyphs) {
            if (inSocket == misc[i]) return kResultUniqueGlyph;
        }
        return kResultOk;
    }
    return kResultInvalidGlyph;
}

}  // namespace wowee::game::glyph_socket
