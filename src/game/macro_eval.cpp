#include "game/macro_eval.hpp"

#include <cstdlib>
#include <cstring>
#include <cctype>

namespace wowee::game::macro_eval {

namespace {

/// A range of the text, as 0x005eea40 cuts one: leading spaces skipped, up to
/// the separator (or the end with none), trailing spaces dropped. `next` is
/// where the separator was.
struct Range {
    size_t begin = 0;
    size_t end = 0;
};
Range cut(const std::string& s, size_t begin, size_t end, char sep, size_t* next = nullptr) {
    Range r{begin, end};
    while (r.begin < end && s[r.begin] == ' ') ++r.begin;
    if (sep != '\0') {
        r.end = r.begin;
        while (r.end < end && s[r.end] != sep) ++r.end;
        if (next) *next = r.end;
    }
    while (r.end > r.begin && s[r.end - 1] == ' ') --r.end;
    return r;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

bool startsCaseless(const std::string& s, size_t at, const char* what) {
    const size_t n = std::strlen(what);
    if (s.size() < at + n) return false;
    for (size_t i = 0; i < n; ++i) {
        const auto a = static_cast<unsigned char>(s[at + i]);
        const auto b = static_cast<unsigned char>(what[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

}  // namespace

Parsed parse(const std::string& body, const std::function<int(const std::string&)>& slashKind) {
    Parsed out;
    size_t pos = 0;
    const size_t end = body.size();
    while (pos < end) {
        size_t lineEnd = end;
        const Range line = cut(body, pos, end, '\n', &lineEnd);
        pos = lineEnd + 1;
        size_t wordEnd = line.end;
        const Range word = cut(body, line.begin, line.end, ' ', &wordEnd);
        const size_t wordLen = word.end - word.begin;
        if (wordLen == 0) continue;
        int kind = 0;
        if (body[word.begin] != '/') {
            // "#show", and "#showtooltip" besides setting the tooltip flag.
            if (body[word.begin] != '#' || wordLen <= 4 || !startsCaseless(body, word.begin, "#show"))
                continue;
            if (wordLen > 11 && startsCaseless(body, word.begin + 5, "tooltip")) out.showTooltip = true;
            kind = kShow;
        } else {
            kind = slashKind ? slashKind(body.substr(word.begin, wordLen)) : 0;
            if (kind == 0) continue;
        }
        const Range rest = cut(body, wordEnd, line.end, '\0');
        if (rest.begin >= rest.end) continue;
        const std::string text = body.substr(rest.begin, rest.end - rest.begin);
        // "none" names nothing and ends the reading.
        if (text == "none") break;
        if (kind != kSequence && text[0] != '[') {
            size_t p = 0;
            if (kind == kEquipSlot) {
                while (p < text.size() && isDigit(text[p])) ++p;
                if (p < text.size()) {
                    while (p < text.size() && text[p] == ' ') ++p;
                }
            }
            // A slot number: what it holds can change.
            if (p < text.size() && isDigit(text[p])) out.dynamic = true;
        } else {
            out.dynamic = true;
        }
        out.commands.push_back({kind, text});
        if (kind == kShow || !out.dynamic) break;
    }
    return out;
}

Result evaluate(const Parsed& parsed, const Hooks& hooks) {
    Result r;
    for (const Command& cmd : parsed.commands) {
        std::string text = cmd.text;
        uint64_t target = 0;
        if (!text.empty() && text[0] == '[') {
            std::string chosen;
            if (!hooks.options || !hooks.options(text, chosen, target)) continue;
            const Range t = cut(chosen, 0, chosen.size(), '\0');
            if (t.begin >= t.end) continue;
            text = chosen.substr(t.begin, t.end - t.begin);
        }
        r.target = target;
        if (cmd.kind == kRandom) {
            const Range first = cut(text, 0, text.size(), ',');
            text = text.substr(first.begin, first.end - first.begin);
        } else if (cmd.kind == kSequence) {
            const std::string entry = hooks.castSequence ? hooks.castSequence(text) : std::string();
            if (!entry.empty()) text = entry;
        } else if (cmd.kind == kEquipSlot) {
            size_t p = 0;
            while (p < text.size() && isDigit(text[p])) ++p;
            if (p < text.size()) {
                while (p < text.size() && text[p] == ' ') ++p;
            }
            text = text.substr(p);
        }
        if (text.empty() || !isDigit(text[0])) {
            r.item = hooks.itemByName ? hooks.itemByName(text) : 0;
            if (r.item == 0 && cmd.kind != kEquip && cmd.kind != kEquipSlot && hooks.spellByName) {
                bool pet = false;
                r.spell = static_cast<int32_t>(hooks.spellByName(text, pet));
                r.petSpell = r.spell != 0 && pet;
            }
            if (r.item == 0 && r.spell == 0) r.spell = -1;
            return r;
        }
        // "/use 13" - an equipment or backpack slot; "/use 1 4" - a bag's.
        size_t next = 0;
        const Range a = cut(text, 0, text.size(), ' ', &next);
        const Range b = cut(text, next, text.size(), '\0');
        const int first = std::atoi(text.substr(a.begin, a.end - a.begin).c_str());
        uint32_t item = 0;
        if (b.begin >= b.end) {
            const int slot = first - 1;
            if (slot >= 0 && slot <= 0x16 && hooks.itemInSlot) item = hooks.itemInSlot(-1, slot);
        } else {
            const int second = std::atoi(text.substr(b.begin, b.end - b.begin).c_str());
            const int bag = first - 1;
            if (bag == -1) {
                const int slot = second - 1 + 0x17;
                if (slot >= 0x17 && slot <= 0x26 && hooks.itemInSlot) item = hooks.itemInSlot(-1, slot);
            } else if (bag >= 0 && bag < 4 && second >= 1 && hooks.itemInSlot) {
                item = hooks.itemInSlot(bag, second - 1);
            }
        }
        r.item = item;
        r.spell = item != 0 ? 0 : -1;
        return r;
    }
    return r;
}

}  // namespace wowee::game::macro_eval
