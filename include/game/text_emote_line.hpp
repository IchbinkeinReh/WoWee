#pragma once
// The chat line of a text emote, as 0x00503660 makes it from an EmotesText row.
//
// The row holds sixteen EmotesTextData ids (fields 3-18). Its slot is a set of
// bits: 1 the target is the player, 2 the sender is the player, 4 there is no
// target, 8 the sender is female. 0x00503660 tries the slot with the gender bit,
// then without it, then the same two with the "target is me" bit cleared and
// the no-target bit set; the first with a text is said, and when all four are
// empty no line is made at all.

#include <array>
#include <cctype>
#include <string>
#include <vector>

namespace wowee {
namespace game {

using EmoteTextSlots = std::array<std::string, 16>;

namespace text_emote {
constexpr unsigned kTargetIsMe = 1;
constexpr unsigned kSenderIsMe = 2;
constexpr unsigned kNoTarget = 4;
constexpr unsigned kFemale = 8;
}  // namespace text_emote

/// The bits 0x00503660 starts from. targetIsMe only counts for another sender's
/// emote: the player's own takes no look at the target's name.
inline unsigned textEmoteBits(bool senderIsMe, bool targetIsMe, bool noTarget) {
    unsigned bits = 0;
    if (senderIsMe) bits = text_emote::kSenderIsMe;
    else if (targetIsMe) bits = text_emote::kTargetIsMe;
    if (noTarget) bits |= text_emote::kNoTarget;
    return bits;
}

/// The slot whose text is said, or -1 for no line.
inline int textEmoteSlot(const EmoteTextSlots& slots, unsigned bits, bool female) {
    const unsigned g = female ? text_emote::kFemale : 0u;
    const auto has = [&](unsigned s) { return s < slots.size() && !slots[s].empty(); };
    if (has(g | bits)) return static_cast<int>(g | bits);
    if (has(bits)) return static_cast<int>(bits);
    bits = (bits & ~text_emote::kTargetIsMe) | text_emote::kNoTarget;
    if (has(g | bits)) return static_cast<int>(g | bits);
    if (has(bits)) return static_cast<int>(bits);
    return -1;
}

/// Fills an EmotesTextData template. The English client writes "%s waves at
/// %s." and takes the arguments in order; other locales reorder them with
/// positional specifiers ("%1$s nickt %2$s zu."). 'args' feeds both the bare %s
/// in turn and %N$s by index; a missing name fills with nothing.
inline std::string fillTextEmoteTemplate(const std::string& text,
                                         const std::vector<const std::string*>& args) {
    auto name = [&](size_t i) -> const std::string* { return i < args.size() ? args[i] : nullptr; };
    std::string out;
    out.reserve(text.size() + 16);
    size_t next = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%' || i + 1 >= text.size()) {
            out.push_back(text[i]);
            continue;
        }
        const char c = text[i + 1];
        if (c == '%') {
            out.push_back('%');
            ++i;
        } else if (c == 's') {
            if (const std::string* n = name(next++)) out += *n;
            ++i;
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            size_t j = i + 1;
            size_t index = 0;
            while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j]))) {
                index = index * 10 + static_cast<size_t>(text[j] - '0');
                ++j;
            }
            if (j + 1 < text.size() && text[j] == '$' && text[j + 1] == 's' && index >= 1) {
                if (const std::string* n = name(index - 1)) out += *n;
                i = j + 1;
            } else {
                out.push_back(text[i]);
            }
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

/// The whole line: the slot's text with the names 0x00503660 gives it - the
/// player's own emote the target alone, another's at the player the sender
/// alone, another's the sender and the target. "" when no slot has a text.
inline std::string textEmoteLine(const EmoteTextSlots& slots, bool senderIsMe,
                                 const std::string& senderName, const std::string& targetName,
                                 const std::string& playerName, bool female) {
    const bool targetIsMe = !senderIsMe && !targetName.empty() && targetName == playerName;
    const int slot = textEmoteSlot(slots, textEmoteBits(senderIsMe, targetIsMe, targetName.empty()), female);
    if (slot < 0) return {};
    const std::string& text = slots[static_cast<size_t>(slot)];
    if (senderIsMe) return fillTextEmoteTemplate(text, {&targetName});
    if (targetIsMe) return fillTextEmoteTemplate(text, {&senderName});
    return fillTextEmoteTemplate(text, {&senderName, &targetName});
}

}  // namespace game
}  // namespace wowee
