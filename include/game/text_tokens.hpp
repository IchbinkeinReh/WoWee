#pragma once

/**
 * text_tokens.hpp - the $-tokens WoW's text is written with.
 *
 * Quest text, gossip, item text and what an NPC says all arrive from the server
 * with the player left as a blank: "$N, I need your help" and "$gsir:madam;".
 * The client is what fills them in, which is why the same quest reads
 * differently to two people.
 *
 * This lives in the game layer rather than beside the chat window because the
 * chat handler has to resolve a monster's line before it reaches the interface,
 * and it cannot reach into the UI to do it.
 */

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "game/character.hpp"

namespace wowee {
namespace game {

class GameHandler;

/// Who the text is being written for. Everything resolveTextTokens needs to
/// know about the reader, and nothing else - so the substitution itself can be
/// exercised without a client attached to it.
struct TextSubject {
    std::string name = "Adventurer";
    /// In the client's language and the reader's gender (chr_names.hpp).
    std::string className = "Adventurer";
    std::string raceName = "Unknown";
    Gender gender = Gender::NONBINARY;
    /// The gender those two words are in, which a phrase may go by instead
    /// ($g male : female : c;): the reader's where unset.
    std::optional<Gender> classGender, raceGender;
    /// A world state's value, for the texts that count something; a count of
    /// days, hours or minutes ('D', 'H', 'M') in the client's words
    /// (INT_GENERAL_DURATION_*); and the server's time.
    std::function<uint32_t(int32_t)> worldState;
    std::function<std::string(char, int)> duration;
    int64_t serverTime = 0;
};

/// The substitution itself.
std::string resolveTextTokens(const std::string& text, const TextSubject& subject);

/// A character's class and race put into a subject whose gender is set: their
/// words in the client's language.
void nameForSubject(GameHandler& gameHandler, TextSubject& subject, uint8_t classId, uint8_t raceId);

/// The logged-in character, as the substitution sees them.
TextSubject textSubjectFor(GameHandler& gameHandler);

/// Fill in the $-tokens in a piece of server text for the logged-in character,
/// as the client's parser does (0x00579590 -> 0x00578f20).
///
/// $n/$N name, $C class, $R race - in small letters as $c and $r - $b/$B a
/// line break, $gmale:female; choosing by gender (with :c or :r behind it, the
/// class's or the race's word's), $<n>w, e, d and k a world state's value, its
/// negative's, a duration and a time left. Any other '$' stays.
/// A `|n` is a line break too. Text with no '$' in it comes straight back.
std::string resolveTextTokens(const std::string& text, GameHandler& gameHandler);

}  // namespace game
}  // namespace wowee
