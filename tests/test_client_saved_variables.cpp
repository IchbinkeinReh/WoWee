// Drop-in mode: the player's own addons' saved variables are copied from the
// original client's WTF into this client's config folder, never written back.

#include <catch_amalgamated.hpp>

#include "addons/client_saved_variables.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace wowee::addons::client_sv;

namespace {

void writeFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary | std::ios::trunc) << text;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

struct Dirs {
    fs::path root;
    fs::path client;
    fs::path config;
    Dirs() {
        root = fs::temp_directory_path() / ("wowee_client_sv_" + std::to_string(std::chrono::steady_clock::now()
                                                                                    .time_since_epoch().count()));
        client = root / "client";
        config = root / "config";
        fs::create_directories(client);
        fs::create_directories(config);
    }
    ~Dirs() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

} // namespace

TEST_CASE("both WTF files are copied under the account/realm/character structure") {
    Dirs d;
    const fs::path acc = d.client / "WTF" / "Account" / "IBKR2";
    writeFile(acc / "SavedVariables" / "MyAddon.lua", "MyAddonDB = { a = 1 }\n");
    writeFile(acc / "Realm's Server" / "Locki" / "SavedVariables" / "MyAddon.lua", "MyAddonChar = 7\n");

    std::vector<Copied> out;
    std::vector<std::string> errors;
    // The login name as typed (lower case): the folder is the upper-case one.
    syncAddon(d.client.string(), d.config.string(), "ibkr2", "Realm's Server", "Locki", "MyAddon", out, errors);
    REQUIRE(errors.empty());
    REQUIRE(out.size() == 2);
    const fs::path accCopy = d.config / "AddOnsSaved" / "IBKR2" / "SavedVariables" / "MyAddon.lua";
    const fs::path charCopy =
        d.config / "AddOnsSaved" / "IBKR2" / "Realm's Server" / "Locki" / "SavedVariables" / "MyAddon.lua";
    REQUIRE(readFile(accCopy) == "MyAddonDB = { a = 1 }\n");
    REQUIRE(readFile(charCopy) == "MyAddonChar = 7\n");
    REQUIRE(copyPath(d.config.string(), "ibkr2", "Realm's Server", "Locki", "MyAddon", false) == accCopy);
    REQUIRE(copyPath(d.config.string(), "ibkr2", "Realm's Server", "Locki", "MyAddon", true) == charCopy);
}

TEST_CASE("a copy saved here since is kept; a newer WTF file replaces it") {
    Dirs d;
    const fs::path src = d.client / "WTF" / "Account" / "ACC" / "SavedVariables" / "X.lua";
    writeFile(src, "X = 1\n");
    std::vector<Copied> out;
    std::vector<std::string> errors;
    syncAddon(d.client.string(), d.config.string(), "acc", "", "", "X", out, errors);
    REQUIRE(out.size() == 1);
    const fs::path copy = out[0].to;
    // In parentheses so Catch does not decompose it: it would print the two
    // times, and a file time's count is __int128 under libc++, which no
    // ostream can print - the test failed to build on macOS and windows-arm64.
    REQUIRE((fs::last_write_time(copy) == fs::last_write_time(src)));

    // Second start, nothing changed: not copied again.
    out.clear();
    syncAddon(d.client.string(), d.config.string(), "acc", "", "", "X", out, errors);
    REQUIRE(out.empty());

    // This client saved its own state: newer than WTF, so kept.
    writeFile(copy, "X = 2\n");
    fs::last_write_time(copy, fs::last_write_time(src) + std::chrono::seconds(60));
    syncAddon(d.client.string(), d.config.string(), "acc", "", "", "X", out, errors);
    REQUIRE(out.empty());
    REQUIRE(readFile(copy) == "X = 2\n");

    // The original client wrote the file later still: it wins.
    writeFile(src, "X = 3\n");
    fs::last_write_time(src, fs::last_write_time(copy) + std::chrono::seconds(60));
    syncAddon(d.client.string(), d.config.string(), "acc", "", "", "X", out, errors);
    REQUIRE(out.size() == 1);
    REQUIRE(readFile(copy) == "X = 3\n");
    REQUIRE(readFile(src) == "X = 3\n");
    REQUIRE(errors.empty());
}

TEST_CASE("no WTF folder for the account: nothing copied") {
    Dirs d;
    writeFile(d.client / "WTF" / "Account" / "OTHER" / "SavedVariables" / "X.lua", "X = 1\n");
    REQUIRE_FALSE(hasAccountFolder(d.client.string(), "acc"));
    REQUIRE(hasAccountFolder(d.client.string(), "other"));
    std::vector<Copied> out;
    std::vector<std::string> errors;
    syncAddon(d.client.string(), d.config.string(), "acc", "R", "C", "X", out, errors);
    REQUIRE(out.empty());
    REQUIRE_FALSE(fs::exists(d.config / "AddOnsSaved"));
}

TEST_CASE("WTF folder names are matched without regard to case") {
    Dirs d;
    writeFile(d.client / "wtf" / "account" / "IBKR2" / "realm" / "locki" / "savedvariables" / "x.lua", "Y = 1\n");
    std::vector<Copied> out;
    std::vector<std::string> errors;
    syncAddon(d.client.string(), d.config.string(), "ibkr2", "Realm", "Locki", "X", out, errors);
    REQUIRE(out.size() == 1);
    REQUIRE(readFile(d.config / "AddOnsSaved" / "IBKR2" / "Realm" / "Locki" / "SavedVariables" / "X.lua") ==
            "Y = 1\n");
    // No character yet: the per-character file is not looked at.
    REQUIRE(copyPath(d.config.string(), "ibkr2", "Realm", "", "X", true).empty());
}
