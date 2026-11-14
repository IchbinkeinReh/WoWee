// The original client's saved account name and realm list, read the way
// Wow.exe 12340 reads WTF\Config.wtf and realmlist.wtf (see
// include/core/original_client_login.hpp for the addresses).

#include <catch2/catch_amalgamated.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/original_client_login.hpp"

namespace fs = std::filesystem;
using namespace wowee::core;

namespace {

struct ClientFolder {
    fs::path root;
    ClientFolder() {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() /
               ("wowee_wtf_" + std::to_string(counter.fetch_add(1)) + "_" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(root);
    }
    ~ClientFolder() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    void write(const fs::path& rel, const std::string& text) {
        fs::create_directories((root / rel).parent_path());
        std::ofstream(root / rel, std::ios::binary) << text;
    }
    /// A locale as installed: its folder with its locale archive in it.
    void locale(const std::string& loc) { write(fs::path("Data") / loc / ("locale-" + loc + ".MPQ"), "MPQ"); }
};

} // namespace

TEST_CASE("wtf: SET lines with and without quotes, in any case") {
    const auto sets = parseWtfSetLines("SET accountName \"Tester\"\r\n"
                                       "set realmlist logon.example.test\r\n"
                                       "Set locale deDE\n");
    REQUIRE(sets.size() == 3);
    CHECK(sets[0] == std::pair<std::string, std::string>{"accountName", "Tester"});
    CHECK(sets[1] == std::pair<std::string, std::string>{"realmlist", "logon.example.test"});
    CHECK(sets[2] == std::pair<std::string, std::string>{"locale", "deDE"});
}

TEST_CASE("wtf: only lines starting with SET and a space are run") {
    const std::string text = "# SET accountName Commented\n"
                             "-- set realmlist commented.test\n"
                             "  SET accountName Indented\n"
                             "SET\taccountName Tabbed\n"
                             "SETX accountName Wrong\n"
                             "\n"
                             "SET accountName Real\n";
    CHECK(findWtfSet(text, "accountName") == std::optional<std::string>("Real"));
    CHECK_FALSE(findWtfSet(text, "realmList").has_value());
}

TEST_CASE("wtf: the value is one token; what follows it is ignored") {
    CHECK(findWtfSet("set realmlist host.test # my server\n", "realmList") == std::optional<std::string>("host.test"));
    CHECK(findWtfSet("set realmlist host.test;other\n", "realmList") == std::optional<std::string>("host.test"));
    CHECK(findWtfSet("SET accountName \"two words\" trailing\n", "accountName") ==
          std::optional<std::string>("two words"));
    CHECK(findWtfSet("SET accountName \"\"\n", "accountName") == std::optional<std::string>(""));
}

TEST_CASE("wtf: CRLF, a byte order mark, a missing final newline, the last SET wins") {
    const std::string text = "\xEF\xBB\xBFSET realmList first.test\r\nSET REALMLIST second.test:3725";
    CHECK(findWtfSet(text, "realmlist") == std::optional<std::string>("second.test:3725"));
    CHECK(findWtfSet("", "realmlist") == std::nullopt);
}

TEST_CASE("realm list: host with and without a port") {
    auto a = splitRealmList("logon.example.test");
    REQUIRE(a);
    CHECK(a->host == "logon.example.test");
    CHECK(a->port == 3724);
    auto b = splitRealmList("127.0.0.1:3725");
    REQUIRE(b);
    CHECK(b->host == "127.0.0.1");
    CHECK(b->port == 3725);
    auto c = splitRealmList("host.test:notaport");
    REQUIRE(c);
    CHECK(c->host == "host.test");
    CHECK(c->port == 3724);
    CHECK(splitRealmList("host.test:70000")->port == 3724);
    CHECK_FALSE(splitRealmList("").has_value());
    CHECK_FALSE(splitRealmList(":3724").has_value());
}

TEST_CASE("locale: the configured one when installed, else the next in the client's table") {
    CHECK(settleClientLocale("deDE", {"deDE"}) == "deDE");
    CHECK(settleClientLocale("dede", {"deDE"}) == "deDE");
    // Unset ("****" is the cvar's default) means enUS; not installed, so on
    // through the table: enUS esES frFR ... ruRU deDE.
    CHECK(settleClientLocale("", {"deDE"}) == "deDE");
    CHECK(settleClientLocale("****", {"frFR", "deDE"}) == "frFR");
    CHECK(settleClientLocale("enGB", {"enUS", "deDE"}) == "enUS");
    CHECK(settleClientLocale("frFR", {}) == "frFR");
    CHECK(settleClientLocale("", {}) == "enUS");
}

TEST_CASE("client folder: account from Config.wtf, server from Data\\<locale>\\realmlist.wtf") {
    ClientFolder c;
    c.locale("deDE");
    c.write("WTF/Config.wtf", "SET locale \"deDE\"\r\nSET accountName \"PrefillTester\"\r\n"
                              "SET realmList \"config.test\"\r\n");
    c.write("Data/deDE/realmlist.wtf", "set realmlist logon.prefill.test:3725\r\n");
    c.write("realmlist.wtf", "set realmlist root.test\r\n");
    c.write("Data/realmlist.wtf", "set realmlist data.test\r\n");
    const OriginalClientLogin login = readOriginalClientLogin(c.root);
    CHECK(login.locale == "deDE");
    CHECK(login.accountName == "PrefillTester");
    CHECK(login.realmList == "logon.prefill.test:3725");
    CHECK(fs::path(login.realmListFrom).filename() == "realmlist.wtf");
    CHECK(fs::path(login.realmListFrom).parent_path().filename() == "deDE");
}

TEST_CASE("client folder: realmlist.wtf beside the client only when the locale's is missing") {
    ClientFolder c;
    c.locale("deDE");
    c.write("WTF/Config.wtf", "SET locale \"deDE\"\n");
    c.write("realmlist.wtf", "set realmlist root.test\n");
    c.write("Data/realmlist.wtf", "set realmlist data.test\n");
    CHECK(readOriginalClientLogin(c.root).realmList == "root.test");
    // An empty locale realmlist.wtf still opens, so the one beside the client
    // is not run, and nothing sets the realm list.
    c.write("Data/deDE/realmlist.wtf", "");
    CHECK(readOriginalClientLogin(c.root).realmList.empty());
}

TEST_CASE("client folder: Config.wtf's realmList stands when no realmlist.wtf sets one") {
    ClientFolder c;
    c.locale("deDE");
    c.write("WTF/Config.wtf", "SET realmList \"config.test:1234\"\n");
    c.write("Data/deDE/realmlist.wtf", "# nothing here\n");
    const OriginalClientLogin login = readOriginalClientLogin(c.root);
    CHECK(login.realmList == "config.test:1234");
    CHECK(fs::path(login.realmListFrom).filename() == "Config.wtf");
    CHECK(login.accountName.empty());
}

TEST_CASE("client folder: the locale follows what is installed when Config.wtf names none") {
    ClientFolder c;
    c.locale("frFR");
    c.write("Data/frFR/realmlist.wtf", "set realmlist fr.test\n");
    c.write("Data/enUS/realmlist.wtf", "set realmlist us.test\n");  // a folder, but no archive
    const OriginalClientLogin login = readOriginalClientLogin(c.root);
    CHECK(login.locale == "frFR");
    CHECK(login.realmList == "fr.test");
}

TEST_CASE("client folder: nothing there") {
    ClientFolder c;
    const OriginalClientLogin login = readOriginalClientLogin(c.root);
    CHECK(login.accountName.empty());
    CHECK(login.realmList.empty());
    CHECK(login.realmListFrom.empty());
    CHECK(login.locale == "enUS");
    CHECK(readOriginalClientLogin(c.root / "does-not-exist").realmList.empty());
}
