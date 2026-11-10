#pragma once

// The mature-language filter, as the reference client runs it on a chat line
// (0x00509dd0, the same loop as 0x00509a50).
//
// ChatProfanity.dbc holds the expressions, each for one locale (its Language
// column, the client's locale index - 0 enUS, 1 koKR, 2 frFR, 3 deDE, 4 zhCN,
// 5 zhTW, 6 esES, 7 esMX, 8 ruRU) or for all (-1). With profanityFilter on,
// every expression of the client's locale is matched against the line without
// regard to case, and each match outside a coloured run (|c...|r, 0x004fcf40)
// is overwritten character for character with "!@#$%^&*", the place in that
// string carried from one replacement to the next and from one line to the
// next (0x00bcfb84).

#include <cstdint>
#include <regex>
#include <string>
#include <vector>

namespace wowee::game::chat_profanity {

/// The client's locale index for a locale name (0x00405dd0, the table at
/// 0x00ad2fe0; enGB counts as enUS); 0 for one the client does not list.
/// Shared by everything that picks a DBC row by locale.
int localeIndex(const std::string& locale);

/// A Spencer expression in ECMAScript form: \< and \> (start and end of a
/// word) become \b, which is the same wherever the character beside them in
/// the expression is a word character.
std::string toEcmaScript(const std::string& spencer);

/// 0x004fcf40: whether `offset` characters into `text` is inside |c...|r.
bool insideColour(const std::string& text, size_t offset);

class Filter {
public:
    /// Expressions that do not compile are left out (the client warns of
    /// them and goes on).
    void load(const std::vector<std::string>& expressions);
    [[nodiscard]] size_t size() const { return expressions_.size(); }
    /// Masks every match in `text`; true where anything was masked.
    bool apply(std::string& text);

private:
    std::vector<std::regex> expressions_;
    uint32_t cursor_ = 0;   // 0x00bcfb84
};

}  // namespace wowee::game::chat_profanity
