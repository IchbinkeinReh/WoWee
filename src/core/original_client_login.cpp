#include "core/original_client_login.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>

namespace wowee {
namespace core {

namespace fs = std::filesystem;

namespace {

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

/// One token the way Wow.exe's tokenizer (0x0076f1e0) takes it from `at`,
/// which is left after it: leading separators are skipped; when the
/// separators hold a quote, a token may be quoted and then runs to the
/// closing quote, separators and all; an unquoted token ends at a separator
/// (consumed) or at a quote (left for the next token). At most `limit` - 1
/// characters are kept, as the client's buffer would.
std::string nextToken(std::string_view text, std::size_t& at, std::string_view separators,
                      std::size_t limit) {
    const bool quotes = separators.find('"') != std::string_view::npos;
    auto isSeparator = [&](char c) { return separators.find(c) != std::string_view::npos; };
    bool quoted = false;
    while (at < text.size() && isSeparator(text[at])) {
        if (quotes && text[at] == '"') {
            quoted = true;
            ++at;
            break;
        }
        ++at;
    }
    std::string out;
    while (at < text.size()) {
        const char c = text[at];
        if (quotes && c == '"') {
            if (!out.empty() && !quoted) break;
            quoted = !quoted;
            ++at;
            if (!quoted) break;
            continue;
        }
        if (!quoted && isSeparator(c)) {
            ++at;
            break;
        }
        if (out.size() + 1 < limit) out.push_back(c);
        ++at;
    }
    return out;
}

/// The file `rel` (given with forward slashes) under `root`, each part
/// matched without regard to case as Windows would; empty when there is none.
fs::path findIgnoringCase(const fs::path& root, const fs::path& rel) {
    fs::path at = root;
    std::error_code ec;
    for (const fs::path& part : rel) {
        fs::path exact = at / part;
        if (fs::exists(exact, ec)) {
            at = exact;
            continue;
        }
        bool found = false;
        for (fs::directory_iterator it(at, ec), end; it != end && !ec; it.increment(ec)) {
            if (equalsIgnoreCase(it->path().filename().string(), part.string())) {
                at = it->path();
                found = true;
                break;
            }
        }
        if (!found) return {};
    }
    return at;
}

/// A .wtf file as 0x00766530 opens it: `rel` under the client's folder, else
/// under its WTF\. Returns whether one was opened.
bool readWtf(const fs::path& root, const fs::path& rel, std::string& text, fs::path& opened) {
    for (const fs::path& candidate : {rel, fs::path("WTF") / rel}) {
        const fs::path at = findIgnoringCase(root, candidate);
        std::error_code ec;
        if (at.empty() || !fs::is_regular_file(at, ec)) continue;
        std::ifstream in(at, std::ios::binary);
        if (!in) continue;
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        opened = at;
        return true;
    }
    return false;
}

// The locale table at 0x00ad3028 in Wow.exe 12340, in its order.
constexpr std::array<const char*, 12> kClientLocales = {"deDE", "enGB", "enUS", "esES", "frFR", "koKR",
                                                        "zhCN", "zhTW", "enCN", "enTW", "esMX", "ruRU"};

} // namespace

std::vector<std::pair<std::string, std::string>> parseWtfSetLines(std::string_view text) {
    std::vector<std::pair<std::string, std::string>> out;
    // 0x00766400: a UTF-8 byte order mark is passed over.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }
    std::size_t at = 0;
    while (at < text.size()) {
        const std::string line = nextToken(text, at, "\r\n", 0x800);
        // Only "SET " (any case) is run; anything else in the file is ignored.
        if (line.size() < 4 || !equalsIgnoreCase(std::string_view(line).substr(0, 4), "SET ")) continue;
        std::size_t argAt = 4;
        const std::string_view args(line);
        const std::string name = nextToken(args, argAt, " ,;\t\"\r\n", 0x40);
        const std::string value = nextToken(args, argAt, " ,;\t\"\r\n", 0x800);
        if (name.empty()) continue;
        out.emplace_back(name, value);
    }
    return out;
}

std::optional<std::string> findWtfSet(std::string_view text, std::string_view cvar) {
    std::optional<std::string> value;
    for (const auto& [name, v] : parseWtfSetLines(text)) {
        if (equalsIgnoreCase(name, cvar)) value = v;
    }
    return value;
}

std::optional<RealmAddress> splitRealmList(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    if (value.empty()) return std::nullopt;
    RealmAddress out;
    const std::size_t colon = value.rfind(':');
    std::string_view host = value;
    if (colon != std::string_view::npos) {
        const std::string_view digits = value.substr(colon + 1);
        host = value.substr(0, colon);
        int port = 0;
        bool ok = !digits.empty() && digits.size() <= 5;
        for (char c : digits) {
            if (c < '0' || c > '9') {
                ok = false;
                break;
            }
            port = port * 10 + (c - '0');
        }
        if (ok && port >= 1 && port <= 65535) out.port = port;
    }
    if (host.empty()) return std::nullopt;
    out.host = std::string(host);
    return out;
}

std::string settleClientLocale(const std::string& configured, const std::vector<std::string>& installed) {
    // "****" is the locale cvar's default and becomes enUS (0x004067f0).
    const std::string wanted = (configured.empty() || configured == "****") ? std::string("enUS") : configured;
    // 0x00635d90: the first four characters, in any case; unknown is enUS.
    std::size_t start = 2;
    for (std::size_t i = 0; i < kClientLocales.size(); ++i) {
        if (wanted.size() >= 4 && equalsIgnoreCase(std::string_view(wanted).substr(0, 4), kClientLocales[i])) {
            start = i;
            break;
        }
    }
    auto isInstalled = [&](const char* loc) {
        return std::any_of(installed.begin(), installed.end(),
                           [&](const std::string& have) { return equalsIgnoreCase(have, loc); });
    };
    for (std::size_t n = 0; n < kClientLocales.size(); ++n) {
        const char* loc = kClientLocales[(start + n) % kClientLocales.size()];
        if (isInstalled(loc)) return loc;
    }
    return wanted;
}

OriginalClientLogin readOriginalClientLogin(const fs::path& clientRoot) {
    OriginalClientLogin out;
    std::string realmList;
    std::string from;

    std::string config;
    fs::path opened;
    if (readWtf(clientRoot, "Config.wtf", config, opened)) {
        for (const auto& [name, value] : parseWtfSetLines(config)) {
            if (equalsIgnoreCase(name, "accountName")) out.accountName = value;
            else if (equalsIgnoreCase(name, "locale")) out.locale = value;
            else if (equalsIgnoreCase(name, "realmList")) {
                realmList = value;
                from = opened.string();
            }
        }
    }

    std::vector<std::string> installed;
    for (const char* loc : kClientLocales) {
        const fs::path archive = findIgnoringCase(
            clientRoot, fs::path("Data") / loc / (std::string("locale-") + loc + ".MPQ"));
        if (!archive.empty()) installed.emplace_back(loc);
    }
    out.locale = settleClientLocale(out.locale, installed);

    std::string text;
    if (readWtf(clientRoot, fs::path("Data") / out.locale / "realmlist.wtf", text, opened) ||
        readWtf(clientRoot, "realmlist.wtf", text, opened)) {
        for (const auto& [name, value] : parseWtfSetLines(text)) {
            if (!equalsIgnoreCase(name, "realmList")) continue;
            realmList = value;
            from = opened.string();
        }
    }
    out.realmList = realmList;
    out.realmListFrom = from;
    return out;
}

} // namespace core
} // namespace wowee
