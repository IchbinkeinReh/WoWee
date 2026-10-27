#include "game/chat_spam_filter.hpp"

#include "core/logger.hpp"

namespace wowee::game {

void ChatSpamFilter::compile(const std::vector<std::string>& patterns, std::vector<std::regex>& out) {
    out.clear();
    out.reserve(patterns.size());
    for (size_t i = 0; i < patterns.size(); ++i) {
        const std::string& p = patterns[i];
        if (p.empty()) continue;
        try {
            out.emplace_back(p, std::regex::ECMAScript | std::regex::icase | std::regex::optimize);
        } catch (const std::regex_error&) {
            LOG_WARNING("Invalid chat spam filter expression: \"", p, "\" (ID #: ", i, ")");
        }
    }
}

bool ChatSpamFilter::matches(const std::string& text) const {
    if (text.empty()) return false;
    for (const auto* list : {&client_, &server_}) {
        for (const std::regex& re : *list) {
            // A pattern the server sent can still blow up on a line
            // (error_complexity / error_stack); one such line is let through
            // rather than taking the chat handler down with it.
            try {
                if (std::regex_search(text, re)) return true;
            } catch (const std::regex_error&) {
            }
        }
    }
    return false;
}

}  // namespace wowee::game
