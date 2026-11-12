#pragma once

// The player's own addons' saved variables, taken over from the original
// client's WTF in drop-in mode.
//
// The original keeps them as (0x005f5620 writes them, 0x005f7xxx's loader
// reads them before "Loading add-on %s"):
//   WTF\Account\<ACCOUNT>\SavedVariables\<Addon>.lua                    account-wide
//   WTF\Account\<ACCOUNT>\<realm>\<character>\SavedVariables\<Addon>.lua per character
// <ACCOUNT> is the login name as the client keeps it (DAT_00c24418, set by
// 0x005f9080), upper case on disk.
//
// This client reads WTF and never writes it. Each file is copied into its
// own config folder under the same account/realm/character structure
//   <config>\AddOnsSaved\<ACCOUNT>\SavedVariables\<Addon>.lua
//   <config>\AddOnsSaved\<ACCOUNT>\<realm>\<character>\SavedVariables\<Addon>.lua
// when the copy is missing or the WTF file is newer, and is loaded and saved
// there. A copy written here since (newer than WTF) is left alone.

#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace wowee::addons::client_sv {

/// `name` under `base`, matched without regard to case (WTF folders are
/// named however the client happened to spell them); empty when absent.
inline std::filesystem::path findChild(const std::filesystem::path& base, const std::string& name) {
    std::error_code ec;
    if (base.empty() || name.empty()) return {};
    std::filesystem::path exact = base / name;
    if (std::filesystem::exists(exact, ec)) return exact;
    auto lower = [](std::string v) {
        for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return v;
    };
    const std::string wanted = lower(name);
    for (const auto& entry : std::filesystem::directory_iterator(base, ec)) {
        if (lower(entry.path().filename().string()) == wanted) return entry.path();
    }
    return {};
}

inline std::string upper(std::string v) {
    for (char& c : v) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return v;
}

/// Where this client keeps the file, relative to the config root.
/// Empty for the per-character file while no character is known.
inline std::filesystem::path copyPath(const std::string& configRoot, const std::string& account,
                                      const std::string& realm, const std::string& character,
                                      const std::string& addon, bool perCharacter) {
    std::filesystem::path p = std::filesystem::path(configRoot) / "AddOnsSaved" / upper(account);
    if (perCharacter) {
        if (realm.empty() || character.empty()) return {};
        p /= realm;
        p /= character;
    }
    return p / "SavedVariables" / (addon + ".lua");
}

/// The original's file, if it exists.
inline std::filesystem::path wtfPath(const std::string& clientRoot, const std::string& account,
                                     const std::string& realm, const std::string& character,
                                     const std::string& addon, bool perCharacter) {
    std::filesystem::path at = findChild(clientRoot, "WTF");
    at = findChild(at, "Account");
    at = findChild(at, upper(account));
    if (perCharacter) {
        if (realm.empty() || character.empty()) return {};
        at = findChild(at, realm);
        at = findChild(at, character);
    }
    at = findChild(at, "SavedVariables");
    at = findChild(at, addon + ".lua");
    std::error_code ec;
    if (at.empty() || !std::filesystem::is_regular_file(at, ec)) return {};
    return at;
}

/// Whether the account has a folder in the client's WTF at all.
inline bool hasAccountFolder(const std::string& clientRoot, const std::string& account) {
    if (account.empty()) return false;
    std::filesystem::path at = findChild(clientRoot, "WTF");
    at = findChild(at, "Account");
    at = findChild(at, upper(account));
    std::error_code ec;
    return !at.empty() && std::filesystem::is_directory(at, ec);
}

struct Copied {
    std::filesystem::path from;
    std::filesystem::path to;
};

/// Copy one file over when the copy is missing or `from` is newer. The copy
/// takes the source's time, so the next start copies again only when the
/// original has written the file since. Returns false on a failed copy.
inline bool copyIfNewer(const std::filesystem::path& from, const std::filesystem::path& to, bool& copied,
                        std::string& error) {
    namespace fs = std::filesystem;
    copied = false;
    std::error_code ec;
    const auto srcTime = fs::last_write_time(from, ec);
    if (ec) { error = "cannot read the time of " + from.string(); return false; }
    if (fs::exists(to, ec)) {
        const auto dstTime = fs::last_write_time(to, ec);
        if (!ec && dstTime >= srcTime) return true;
    }
    fs::create_directories(to.parent_path(), ec);
    ec.clear();
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) { error = "cannot copy " + from.string() + " to " + to.string() + ": " + ec.message(); return false; }
    fs::last_write_time(to, srcTime, ec);
    copied = true;
    return true;
}

/// Both files of one addon. Appends what was copied to `out`, failures to `errors`.
inline void syncAddon(const std::string& clientRoot, const std::string& configRoot, const std::string& account,
                      const std::string& realm, const std::string& character, const std::string& addon,
                      std::vector<Copied>& out, std::vector<std::string>& errors) {
    for (const bool perCharacter : {false, true}) {
        const auto from = wtfPath(clientRoot, account, realm, character, addon, perCharacter);
        if (from.empty()) continue;
        const auto to = copyPath(configRoot, account, realm, character, addon, perCharacter);
        if (to.empty()) continue;
        bool copied = false;
        std::string error;
        if (!copyIfNewer(from, to, copied, error)) errors.push_back(error);
        else if (copied) out.push_back({from, to});
    }
}

} // namespace wowee::addons::client_sv
