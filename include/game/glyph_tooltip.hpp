#pragma once

/// A glyph socket's tooltip, or a glyph link's (0x00622ba0): the glyph's name,
/// or what is the matter with the socket; major or minor; and what the glyph
/// does, or when the socket opens.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace wowee::game::glyph_tooltip {

/// The colours of the lines (0x00ad2d48, 54, 30, a4, 2c).
enum class Color : uint8_t { Red, Grey, White, Blue, Gold };

struct Line {
    std::string text;
    Color color = Color::White;
    bool wraps = false;
};

struct Socket {
    bool slotKnown = false;        ///< its GlyphSlot row is there
    uint32_t slotType = 0;         ///< the row's type: 0 a major glyph's
    uint32_t slotTooltip = 0;      ///< the row's number of GLYPH_SLOT_TOOLTIP
    bool enabled = false;          ///< its bit of PLAYER_GLYPHS_ENABLED
    bool filled = false;           ///< a GlyphProperties row is in it
    std::optional<std::string> spellName;   ///< the glyph's spell's, none for a spell that is not there
    std::string description;       ///< the spell's, its tokens filled
    bool link = false;             ///< a link's: not told how to take it out
};

using Text = std::function<std::string(const std::string& key)>;

[[nodiscard]] inline std::vector<Line> lines(const Socket& s, const Text& text) {
    std::vector<Line> out;
    const bool glyph = s.filled && s.spellName;
    if (!s.filled) {
        if (s.enabled) out.push_back({text("GLYPH_INACTIVE"), Color::Grey, false});
        else out.push_back({text("GLYPH_LOCKED"), Color::Red, false});
    } else if (glyph) {
        out.push_back({*s.spellName, Color::White, false});
    }
    out.push_back({text(s.slotKnown && s.slotType != 0 ? "MINOR_GLYPH" : "MAJOR_GLYPH"), Color::Blue, false});
    if (glyph) {
        if (s.description.empty()) return out;
        out.push_back({s.description, Color::Gold, true});
        if (!s.link) out.push_back({text("GLYPH_SLOT_REMOVE_TOOLTIP"), Color::Grey, false});
    } else if (s.enabled) {
        out.push_back({text("GLYPH_EMPTY_DESC"), Color::Gold, true});
    } else if (s.slotKnown) {
        out.push_back({text("GLYPH_SLOT_TOOLTIP" + std::to_string(s.slotTooltip)), Color::Gold, true});
    }
    return out;
}

}  // namespace wowee::game::glyph_tooltip
