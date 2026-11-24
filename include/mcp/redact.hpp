#pragma once

// Keeping what nobody asked for out of what the server says.
//
// No tool reads a file or a credential, but what they do read - the log, a
// chat line, an edit box - can have one in it. Every string on its way out is
// held against the secrets this process knows: the account and its password,
// the session key, the streaming key and the server's own token.

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace wowee::mcp {

class Redactor {
public:
    /// Shorter ones are left out: three letters are in every second word, and
    /// a text with those blanked says nothing about the secret and nothing
    /// else either.
    static constexpr size_t kShortest = 4;

    void clear() { secrets_.clear(); }
    /// `anyCase`: matched without regard to case, as an account name is
    /// written upper case in one place and as typed in another.
    void add(std::string_view secret, bool anyCase = false);

    [[nodiscard]] std::string apply(std::string_view text) const;
    /// Every string in it, names of members included.
    void apply(nlohmann::json& value) const;

private:
    struct Secret {
        std::string text;
        bool anyCase = false;
    };
    std::vector<Secret> secrets_;
};

}  // namespace wowee::mcp
