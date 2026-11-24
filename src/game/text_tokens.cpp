#include "game/text_tokens.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace wowee {
namespace game {

namespace {

// __strlwr in the C locale: the ASCII letters alone.
std::string lowered(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

void skipSpaces(const std::string& text, size_t& at) {
    while (at < text.size() && text[at] == ' ') ++at;
}

/// `text` from `from` to `to`, without the spaces it ends with.
void appendPart(std::string& out, const std::string& text, size_t from, size_t to) {
    while (to > from && text[to - 1] == ' ') --to;
    if (to > from) out.append(text, from, to - from);
}

/// 0x00576cd0: `male : female ;` from `at`, and where a third part is there
/// the word whose gender decides - c the class's, r the race's. Left at the
/// semicolon; nothing is taken where a colon or it is missing.
void chooseByGender(std::string& out, const std::string& text, size_t& at, const TextSubject& subject) {
    skipSpaces(text, at);
    const size_t colon = at < text.size() ? text.find(':', at) : std::string::npos;
    if (colon == std::string::npos) return;
    const size_t end = text.find(';', colon);
    if (end == std::string::npos) return;
    size_t third = text.find(':', colon + 1);
    Gender gender = subject.gender;
    if (third != std::string::npos && third + 1 < end) {
        switch (text[third + 1]) {
            case 'C': case 'c': gender = subject.classGender.value_or(subject.gender); break;
            case 'R': case 'r': gender = subject.raceGender.value_or(subject.gender); break;
            default: break;
        }
    } else {
        third = std::string::npos;
    }
    if (gender == Gender::MALE) {
        appendPart(out, text, at, colon);
    } else {
        size_t from = colon + 1;
        skipSpaces(text, from);
        appendPart(out, text, from, third != std::string::npos ? third : end);
    }
    at = end;
}

uint32_t stateOf(const TextSubject& subject, int32_t id) {
    return subject.worldState ? subject.worldState(id) : 0u;
}

std::string durationOf(const TextSubject& subject, char unit, int count) {
    return subject.duration ? subject.duration(unit, count) : std::to_string(count);
}

/// 0x00578f20: the token at `at`, just past its '$'. False where it is none.
bool resolveToken(std::string& out, const std::string& text, size_t& at, const TextSubject& subject) {
    const size_t numberAt = at;
    while (at < text.size() && text[at] >= '0' && text[at] <= '9') ++at;
    const int32_t number =
        static_cast<int32_t>(std::strtol(text.substr(numberAt, at - numberAt).c_str(), nullptr, 10));
    const char code = at < text.size() ? text[at] : '\0';
    switch (code) {
        case 'A': case 'a':
            // The description of the spell the text goes with; these go with none.
            break;
        case 'B': case 'b': out += '\n'; break;
        case 'C': out += subject.className; break;
        case 'c': out += lowered(subject.className); break;
        case 'R': out += subject.raceName; break;
        case 'r': out += lowered(subject.raceName); break;
        case 'N': case 'n': out += subject.name; break;
        case 'D': case 'd': {
            // A world state's minutes, as a duration.
            const double v = static_cast<int32_t>(stateOf(subject, -number));
            if (v > 1440.0) {
                out += durationOf(subject, 'D', static_cast<int>(std::lround((v - 1.0) / 1440.0 + 1.0)));
            } else if (v > 60.0) {
                out += durationOf(subject, 'H',
                                  static_cast<int>(std::lround(v / 60.0 + (std::fmod(v, 60.0) >= 30.0 ? 1.0 : 0.0))));
            } else {
                out += durationOf(subject, 'M', static_cast<int>(std::lround(v)));
            }
            break;
        }
        case 'E': case 'e': out += std::to_string(static_cast<int32_t>(stateOf(subject, -number))); break;
        case 'W': case 'w': out += std::to_string(static_cast<int32_t>(stateOf(subject, number))); break;
        case 'K': case 'k': {
            // The time until what a world state says (0x00576e50).
            const int64_t left = std::max<int64_t>(0, static_cast<int64_t>(stateOf(subject, number)) - subject.serverTime);
            char buf[32];
            if (left / 3600 < 1) {
                std::snprintf(buf, sizeof(buf), "%02d:%02d", static_cast<int>(left % 3600 / 60),
                              static_cast<int>(left % 60));
            } else {
                std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", static_cast<int>(left / 3600),
                              static_cast<int>(left % 3600 / 60), static_cast<int>(left % 60));
            }
            out += buf;
            break;
        }
        case 'G': case 'g':
        case 'T': case 't':
            // $t is a rank of honour where the player has one, else the
            // text's own choice; nobody has one in this expansion.
            ++at;
            chooseByGender(out, text, at, subject);
            break;
        default:
            return false;
    }
    if (at < text.size()) ++at;
    return true;
}

}  // namespace

std::string resolveTextTokens(const std::string& text, const TextSubject& subject) {
    // The vast majority of chat messages contain no $ tokens.
    if (text.find('$') == std::string::npos) return text;

    // 0x00579590: what is no token leaves its '$' and goes on behind it.
    std::string result;
    result.reserve(text.size() + 32);
    size_t at = 0;
    while (at < text.size()) {
        const size_t dollar = text.find('$', at);
        if (dollar == std::string::npos) break;
        result.append(text, at, dollar - at);
        at = dollar + 1;
        if (!resolveToken(result, text, at, subject)) result += '$';
    }
    if (at < text.size()) result.append(text, at, std::string::npos);

    // WoW markup linebreak token.
    for (const char* mark : {"|n", "|N"}) {
        size_t pos = 0;
        while ((pos = result.find(mark, pos)) != std::string::npos) {
            result.replace(pos, 2, "\n");
            pos += 1;
        }
    }
    return result;
}

}  // namespace game
}  // namespace wowee
