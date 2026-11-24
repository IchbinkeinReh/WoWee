#pragma once

/// The unit a token or a name stands for (0x0060abf0): a base - player,
/// vehicle, pet, target, party1, partypet1, raid1, raidpet1, boss1, arena1,
/// arenapet1, commentator1, mouseover, focus, npc, questnpc, none - and then
/// any number of "target" and "pet", with or without a "-" before each
/// (0x0060aaa0). What is none of these is the name of a member of the group,
/// or of a member's pet (0x0060a630).

#include "game/spell_unit_target.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace wowee::addons::unit_token {

[[nodiscard]] constexpr char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

/// Whether the text begins with the word (0x0076e780); it is taken off then.
[[nodiscard]] constexpr bool take(std::string_view& text, std::string_view word) {
    if (text.size() < word.size()) return false;
    for (size_t i = 0; i < word.size(); ++i)
        if (lower(text[i]) != word[i]) return false;
    text.remove_prefix(word.size());
    return true;
}

/// The number at the front, taken off (0x0076f190).
[[nodiscard]] constexpr int number(std::string_view& text) {
    int n = 0;
    while (!text.empty() && text[0] >= '0' && text[0] <= '9') {
        if (n < 1000000) n = n * 10 + (text[0] - '0');
        text.remove_prefix(1);
    }
    return n;
}

/// The targets and pets behind a unit (0x0060aaa0): whether all of the text is
/// that. A pet is asked of the player and of the group's members alone.
template <typename World>
[[nodiscard]] bool follow(std::string_view text, uint64_t& guid, const World& world) {
    while (!text.empty()) {
        if (text[0] == '-') text.remove_prefix(1);
        if (take(text, "target")) {
            guid = world.targetOf(guid);
        } else if (take(text, "pet") && world.petsKnown(guid)) {
            guid = world.petOf(guid);
        } else {
            guid = 0;
            return false;
        }
    }
    return true;
}

/// The name in a text: what is before "-target" or "-pet" at its end, or
/// before either and another "-" (0x0060a630).
[[nodiscard]] constexpr std::string_view nameIn(std::string_view text) {
    size_t n = 0;
    while (n < text.size() && n < 0x30) {
        if (text[n] == '-') {
            std::string_view rest = text.substr(n);
            std::string_view t = rest, p = rest;
            if (take(t, "-target") && (t.empty() || t[0] == '-')) break;
            if (take(p, "-pet") && (p.empty() || p[0] == '-')) break;
        }
        ++n;
    }
    return text.substr(0, n);
}

/// The unit: none for a text that is no unit's, kAskedNobody for a token that
/// names nobody now, kAskedNone for "none".
template <typename World>
[[nodiscard]] std::optional<uint64_t> resolve(std::string_view text, const World& world) {
    using game::unit_target::kAskedNobody;
    using game::unit_target::kAskedNone;
    std::string_view rest = text;
    uint64_t guid = 0;
    if (take(rest, "player")) guid = world.player();
    else if (take(rest, "vehicle")) guid = world.vehicle();
    else if (take(rest, "pet")) guid = world.pet();
    else if (take(rest, "target")) guid = world.target();
    else if (take(rest, "partypet")) guid = world.partyPet(number(rest));
    else if (take(rest, "party")) guid = world.party(number(rest));
    else if (take(rest, "raidpet")) guid = world.raidPet(number(rest));
    else if (take(rest, "raid")) guid = world.raid(number(rest));
    else if (take(rest, "boss")) guid = world.boss(number(rest));
    else if (take(rest, "arenapet")) guid = world.arenaPet(number(rest));
    else if (take(rest, "arena")) guid = world.arena(number(rest));
    else if (take(rest, "commentator")) guid = world.commentator(number(rest));
    else if (take(rest, "mouseover")) guid = world.mouseover();
    else if (take(rest, "focus")) guid = world.focus();
    else if (rest.size() == 3 && take(rest, "npc")) guid = world.npc();
    else if (rest.size() == 8 && take(rest, "questnpc")) guid = world.questNpc();
    else if (rest.size() == 4 && take(rest, "none")) guid = kAskedNone;
    if ((guid != 0 || rest.empty()) && follow(rest, guid, world)) return guid != 0 ? guid : kAskedNobody;
    if (guid != 0) return std::nullopt;
    const std::string_view name = nameIn(text);
    guid = world.named(name);
    if (guid == 0 || !follow(text.substr(name.size()), guid, world)) return std::nullopt;
    return guid != 0 ? guid : kAskedNobody;
}

}  // namespace wowee::addons::unit_token
