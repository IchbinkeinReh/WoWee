#include "mcp/redact.hpp"

#include <algorithm>
#include <cctype>

namespace wowee::mcp {

namespace {

constexpr std::string_view kBlank = "[redacted]";

char foldCase(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

/// An account name is a word, and may be a word of the game's as well: part
/// of a longer one it is left alone.
bool isWordChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

}  // namespace

void Redactor::add(std::string_view secret, bool anyCase) {
    if (secret.size() < kShortest) return;
    for (const auto& s : secrets_) {
        if (s.text == secret && s.anyCase == anyCase) return;
    }
    secrets_.push_back({.text = std::string(secret), .anyCase = anyCase});
    // Longest first, so that one secret inside another does not leave the
    // rest of the longer one standing.
    std::stable_sort(secrets_.begin(), secrets_.end(),
                     [](const Secret& a, const Secret& b) { return a.text.size() > b.text.size(); });
}

std::string Redactor::apply(std::string_view text) const {
    std::string out(text);
    for (const auto& s : secrets_) {
        size_t at = 0;
        while (true) {
            size_t found = std::string::npos;
            if (s.anyCase) {
                const auto it = std::search(out.begin() + static_cast<std::ptrdiff_t>(at), out.end(), s.text.begin(),
                                            s.text.end(), [](char a, char b) { return foldCase(a) == foldCase(b); });
                if (it != out.end()) found = static_cast<size_t>(it - out.begin());
            } else {
                found = out.find(s.text, at);
            }
            if (found == std::string::npos) break;
            const size_t end = found + s.text.size();
            if (s.anyCase && ((found > 0 && isWordChar(out[found - 1])) || (end < out.size() && isWordChar(out[end])))) {
                at = found + 1;
                continue;
            }
            out.replace(found, s.text.size(), kBlank);
            at = found + kBlank.size();
        }
    }
    return out;
}

void Redactor::apply(nlohmann::json& value) const {
    if (secrets_.empty()) return;
    if (value.is_string()) {
        value = apply(std::string_view(value.get_ref<const std::string&>()));
    } else if (value.is_array()) {
        for (auto& item : value) apply(item);
    } else if (value.is_object()) {
        // A name is written as it is read, so a member is moved only where
        // its name has a secret in it, which none of this server's do.
        nlohmann::json renamed = nlohmann::json::object();
        bool any = false;
        for (auto& [name, item] : value.items()) {
            apply(item);
            any = any || apply(std::string_view(name)) != name;
        }
        if (!any) return;
        for (auto& [name, item] : value.items()) renamed[apply(std::string_view(name))] = std::move(item);
        value = std::move(renamed);
    }
}

}  // namespace wowee::mcp
