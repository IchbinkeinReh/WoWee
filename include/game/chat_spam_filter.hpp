#pragma once

// The chat spam filter (0x00509bd0, asked by the chat line's display at
// 0x00509dd0): two lists of regular expressions - SpamMessages.dbc's own
// (loaded by 0x007e2250) and the ones the server hands over in
// SMSG_EXPECTED_SPAM_RECORDS (0x00501c70) - compiled with PCRE as CASELESS |
// UTF8 (0x2801, 0x0083df20 -> 0x008418f0). A player line that any of them
// matches is dropped; a whisper that does is reported back with
// CMSG_CHAT_FILTERED. The patterns are Perl syntax (\s*, ( )?), so they are
// compiled with std::regex's ECMAScript grammar, the one of its grammars that
// reads them: all 133 of the 3.3.5a DBC's do.

#include <regex>
#include <string>
#include <vector>

namespace wowee::game {

class ChatSpamFilter {
public:
    /// SpamMessages.dbc's patterns (0x007e2250).
    void setClientPatterns(const std::vector<std::string>& patterns) { compile(patterns, client_); }
    /// SMSG_EXPECTED_SPAM_RECORDS' patterns, which replace the last set
    /// (0x00501c70).
    void setServerPatterns(const std::vector<std::string>& patterns) { compile(patterns, server_); }

    [[nodiscard]] size_t clientPatternCount() const { return client_.size(); }
    [[nodiscard]] size_t serverPatternCount() const { return server_.size(); }

    /// 0x00509bd0: whether any pattern of either list is found in \p text.
    [[nodiscard]] bool matches(const std::string& text) const;

private:
    /// One that does not compile is dropped and the rest kept, as the client
    /// logs "invalid chat spam filter expression" and goes on.
    static void compile(const std::vector<std::string>& patterns, std::vector<std::regex>& out);

    std::vector<std::regex> client_;
    std::vector<std::regex> server_;
};

}  // namespace wowee::game
