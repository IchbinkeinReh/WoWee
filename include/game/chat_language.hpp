#pragma once

// Speech in a language the listener does not know, as the reference client
// renders it (0x00505930, called from the chat handler 0x00509dd0).
//
// The realm sends what was said as it was said, with the language it was said
// in; the client is what turns "Well met, friend" into "Lon ash, nud" for a
// listener who does not speak Common. Each word is hashed (Storm's
// case-insensitive string hash, 0x0076f340); a listener whose comprehension
// (the language's skill, 300 for one the character speaks) is at or below the
// hash modulo 300 hears a LanguageWords.dbc word of the same byte length -
// picked by the same hash, taking the next shorter length where the language
// has none - written in the case pattern of the word it replaces. A run of
// anything between words collapses to one space. Links, colours and textures
// are stripped first (0x007e1680).

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace wowee::game::chat_language {

/// 0x0076f340 with its defaults: seed 0x7fed7fed, a-z folded to A-Z and '/'
/// to '\\'; never 0.
uint32_t stringHash(std::string_view s);

/// 0x007e1680: |c and its eight digits, |r, |H...|h link data, the brackets
/// of a link's shown text, |h and |T...|t textures go; any other |x stays.
std::string stripEscapes(std::string_view text);

/// The words a language has of exactly `length` bytes, in file order, or
/// null when it has none.
using WordsOfLength = std::function<const std::vector<std::string>*(uint32_t length)>;

/// What a listener with `comprehension` hears of `text` (0x00505930 as the
/// chat handler calls it: no <...> passthrough, separators collapsed). Above
/// 299 the text is untouched.
std::string garble(std::string_view text, uint32_t comprehension, const WordsOfLength& words);

}  // namespace wowee::game::chat_language
