// Whether a glyph fits a socket and whether it can go in: GlyphMatchesSocket
// (0x005b7410) and PlaceGlyphInSocket's check (0x0080c3e0).

#include <catch_amalgamated.hpp>

#include "game/glyph_socket.hpp"

#include <map>

using namespace wowee::game::glyph_socket;

namespace {

// GlyphProperties: 600 major, 601 minor. GlyphSlot: 21 major, 22 minor.
const std::map<uint32_t, uint32_t> kGlyphTypes = {{600, 0}, {601, 1}};
const std::map<uint32_t, uint32_t> kSlotTypes = {{21, 0}, {22, 1}};

bool glyphType(uint32_t id, uint32_t& type) {
    auto it = kGlyphTypes.find(id);
    if (it == kGlyphTypes.end()) return false;
    type = it->second;
    return true;
}
bool slotType(uint32_t id, uint32_t& type) {
    auto it = kSlotTypes.find(id);
    if (it == kSlotTypes.end()) return false;
    type = it->second;
    return true;
}

Sockets sockets() {
    Sockets s;
    s.enabledMask = 0x3f;
    s.slotIds = {21, 22, 22, 21, 22, 21};
    return s;
}

}  // namespace

TEST_CASE("A glyph matches a socket of its own kind", "[glyph]") {
    const uint32_t effects[3] = {kEffectApplyGlyph, 0, 0};
    const uint32_t major[3] = {600, 0, 0};
    const uint32_t minor[3] = {601, 0, 0};
    CHECK(matches(effects, major, 21, glyphType, slotType));
    CHECK_FALSE(matches(effects, major, 22, glyphType, slotType));
    CHECK(matches(effects, minor, 22, glyphType, slotType));
    // No glyph effect, or rows that do not exist: no match.
    const uint32_t none[3] = {6, 0, 0};
    CHECK_FALSE(matches(none, major, 21, glyphType, slotType));
    const uint32_t unknown[3] = {999, 0, 0};
    CHECK_FALSE(matches(effects, unknown, 21, glyphType, slotType));
}

TEST_CASE("The first glyph effect with both rows decides", "[glyph]") {
    const uint32_t effects[3] = {kEffectApplyGlyph, kEffectApplyGlyph, 0};
    // The first names no GlyphProperties row and is passed over.
    const uint32_t misc[3] = {999, 601, 0};
    CHECK(matches(effects, misc, 22, glyphType, slotType));
    CHECK(placement(effects, misc, 1, sockets(), glyphType, slotType) == kResultOk);
}

TEST_CASE("A locked socket, the wrong kind and a duplicate are refused", "[glyph]") {
    const uint32_t effects[3] = {kEffectApplyGlyph, 0, 0};
    const uint32_t major[3] = {600, 0, 0};
    Sockets s = sockets();
    CHECK(placement(effects, major, 0, s, glyphType, slotType) == kResultOk);
    CHECK(placement(effects, major, 1, s, glyphType, slotType) == kResultInvalidGlyph);
    s.enabledMask = 0x3e;
    CHECK(placement(effects, major, 0, s, glyphType, slotType) == kResultGlyphSocketLocked);
    s.enabledMask = 0x3f;
    s.glyphs[5] = 600;
    CHECK(placement(effects, major, 0, s, glyphType, slotType) == kResultUniqueGlyph);
    // Nothing to place at all.
    const uint32_t none[3] = {0, 0, 0};
    CHECK(placement(none, major, 0, sockets(), glyphType, slotType) == kResultInvalidGlyph);
}
