#include "game/chat_spam_filter.hpp"

#include "core/logger.hpp"

namespace wowee::game {

void ChatSpamFilter::setPatterns(const std::vector<std::string>& patterns) {
    patterns_.clear();
    patterns_.reserve(patterns.size());
    for (size_t i = 0; i < patterns.size(); ++i) {
        const std::string& p = patterns[i];
        if (p.empty()) continue;
        try {
            // REG_EXTENDED | REG_ICASE, the flags 0x00501c70 compiles with.
            patterns_.emplace_back(p, std::regex::extended | std::regex::icase);
        } catch (const std::regex_error&) {
            LOG_WARNING("Invalid chat spam filter expression: \"", p, "\" (ID #: ", i, ")");
        }
    }
}

bool ChatSpamFilter::insideColorSpan(const std::string& text, size_t pos) {
    // 0x004fcf40: walk to pos, turned on by |c / |C and off again by |r / |R.
    bool inside = false;
    for (size_t i = 0; i + 1 < text.size() && i < pos;) {
        if (text[i] == '|') {
            const char c = text[i + 1];
            if (c == 'c' || c == 'C') {
                inside = true;
            } else if ((c == 'r' || c == 'R') && inside) {
                inside = false;
            }
            i += 2;
        } else {
            ++i;
        }
    }
    return inside;
}

bool ChatSpamFilter::censor(std::string& text) {
    if (patterns_.empty() || text.empty()) return false;
    static constexpr char kSymbols[] = "!@#$%^&*";
    bool masked = false;
    for (const std::regex& re : patterns_) {
        // The match ranges first: the masking keeps the length, so the offsets
        // stay good, and the string is not mutated under the iterator. A run
        // inside a colour span is left alone, as 0x00509dd0 skips it.
        std::vector<std::pair<size_t, size_t>> runs;
        for (auto it = std::sregex_iterator(text.begin(), text.end(), re), end = std::sregex_iterator();
             it != end; ++it) {
            if (it->length() <= 0) continue;
            runs.emplace_back(static_cast<size_t>(it->position()), static_cast<size_t>(it->length()));
        }
        for (const auto& [start, len] : runs) {
            if (insideColorSpan(text, start)) continue;
            for (size_t k = 0; k < len && start + k < text.size(); ++k) {
                text[start + k] = kSymbols[symbolIndex_];
                symbolIndex_ = (symbolIndex_ + 1) % 8;
            }
            masked = true;
        }
    }
    return masked;
}

}  // namespace wowee::game
