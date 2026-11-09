// The same libstdc++ <regex> misfire chat_spam_filter.cpp silences: GCC 13's
// -Wmaybe-uninitialized under the sanitizers, in the library's instantiations
// emitted at the end of the unit, so it has to hold for the whole file.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

#include "game/chat_profanity.hpp"

#include <array>

namespace wowee::game::chat_profanity {

int localeIndex(const std::string& locale) {
    static constexpr std::array<const char*, 9> kLocales = {"enUS", "koKR", "frFR", "deDE", "zhCN",
                                                            "zhTW", "esES", "esMX", "ruRU"};
    if (locale == "enGB") return 0;
    for (size_t i = 0; i < kLocales.size(); ++i)
        if (locale == kLocales[i]) return static_cast<int>(i);
    return 0;
}

std::string toEcmaScript(const std::string& spencer) {
    std::string out;
    out.reserve(spencer.size());
    for (size_t i = 0; i < spencer.size(); ++i) {
        if (spencer[i] == '\\' && i + 1 < spencer.size() && (spencer[i + 1] == '<' || spencer[i + 1] == '>')) {
            out += "\\b";
            ++i;
            continue;
        }
        if (spencer[i] == '\\' && i + 1 < spencer.size()) {
            out += spencer[i];
            out += spencer[++i];
            continue;
        }
        out += spencer[i];
    }
    return out;
}

bool insideColour(const std::string& text, size_t offset) {
    bool coloured = false;
    size_t i = 0;
    size_t left = offset;
    while (left > 0 && i < text.size()) {
        const char c = text[i++];
        --left;
        if (c != '|') continue;
        if (left == 0 || i >= text.size()) break;
        const char k = text[i];
        if (k == 'c' || k == 'C') coloured = true;
        else if ((k == 'r' || k == 'R') && coloured) coloured = false;
        ++i;
        --left;
    }
    return coloured;
}

void Filter::load(const std::vector<std::string>& expressions) {
    expressions_.clear();
    for (const auto& e : expressions) {
        try {
            expressions_.emplace_back(toEcmaScript(e), std::regex::ECMAScript | std::regex::icase |
                                                         std::regex::optimize);
        } catch (const std::regex_error&) {
            // "invalid profane names filter expression" - skipped.
        }
    }
}

bool Filter::apply(std::string& text) {
    static constexpr char kMask[] = "!@#$%^&*";
    bool masked = false;
    for (const auto& re : expressions_) {
        size_t from = 0;   // pcVar3: the search goes on from the last match's start
        for (;;) {
            std::smatch m;
            const std::string rest = text.substr(from);
            if (!std::regex_search(rest, m, re)) break;
            const size_t start = static_cast<size_t>(m.position(0));
            const size_t length = static_cast<size_t>(m.length(0));
            // The client asks of the line's start with the offset into the
            // rest (0x004fcf40(line, offset)); asked the same way here.
            if (insideColour(text, start)) break;
            if (length == 0) break;
            for (size_t k = 0; k < length; ++k) {
                text[from + start + k] = kMask[cursor_];
                if (++cursor_ > 7) cursor_ = 0;
            }
            masked = true;
            from += start;
        }
    }
    return masked;
}

}  // namespace wowee::game::chat_profanity
