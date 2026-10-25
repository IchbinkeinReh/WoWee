#pragma once

// The server's chat spam filter: SMSG_EXPECTED_SPAM_RECORDS (0x332, read by
// 0x00501c70) hands the client a list of regular expressions, which it compiles
// (regcomp, REG_EXTENDED | REG_ICASE) and runs over player chat, masking any
// matched run with rotating symbols (0x00509dd0). This is the gold-seller
// filter; before this the records were received and thrown away.

#include <regex>
#include <string>
#include <vector>

namespace wowee::game {

class ChatSpamFilter {
public:
    /// Replace the compiled set with these patterns. Each compiles case-
    /// insensitively as a POSIX extended expression; one that does not compile
    /// is dropped, as the client logs "invalid chat spam filter expression" and
    /// keeps the rest (0x00501c70).
    void setPatterns(const std::vector<std::string>& patterns);

    [[nodiscard]] size_t patternCount() const { return patterns_.size(); }
    [[nodiscard]] bool empty() const { return patterns_.empty(); }

    /// Mask every matched run in p text in place with the rotating symbols
    /// "!@#$%^&*", the way 0x00509dd0 does: one symbol per character, the index
    /// carried across calls, and a match inside a |c..|r colour span left alone
    /// (0x004fcf40). Returns true if anything was masked.
    bool censor(std::string& text);

    /// Whether the byte at p pos sits inside an unclosed |c colour code
    /// (0x004fcf40): such a run is a hyperlink's colour and is not masked.
    [[nodiscard]] static bool insideColorSpan(const std::string& text, size_t pos);

private:
    std::vector<std::regex> patterns_;
    /// 0x00bcfb84: the index into "!@#$%^&*", carried across messages.
    unsigned symbolIndex_ = 0;
};

}  // namespace wowee::game
