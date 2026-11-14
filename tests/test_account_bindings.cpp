// The key bindings the realm keeps (account data 2, bindings-cache.wtf): read
// over the defaults as 0x005641c0 reads them, written as the keys moved off
// them as 0x0055f1b0 writes them.
#include <catch_amalgamated.hpp>

#include "addons/account_bindings.hpp"

using wowee::addons::BindingKeyMap;
using wowee::addons::bindingsCacheText;
using wowee::addons::bindingsFromCacheText;

namespace {

BindingKeyMap defaults() {
    return {
        {"TOGGLESHEATH", {"Z", ""}},
        {"TARGETSELF", {"F1", ""}},
        {"ACTIONBUTTON1", {"1", "PAD3"}},
        {"RAIDTARGET8", {"", ""}},
    };
}

}  // namespace

TEST_CASE("the original's copy leaves every other key on its default", "[account_bindings]") {
    // A realm copy as Wow.exe 12340 writes it after one rebind: F1 moved to
    // a raid target icon, and nothing else said.
    const BindingKeyMap keys = bindingsFromCacheText(defaults(), "BINDINGMODE 0\r\nbind F1 RAIDTARGET8\r\n");
    CHECK(keys.at("TOGGLESHEATH")[0] == "Z");
    CHECK(keys.at("RAIDTARGET8")[0] == "F1");
    CHECK(keys.at("TARGETSELF")[0].empty());
    CHECK(keys.at("ACTIONBUTTON1")[0] == "1");
    CHECK(keys.at("ACTIONBUTTON1")[1] == "PAD3");
}

TEST_CASE("NONE takes a key off its command", "[account_bindings]") {
    const BindingKeyMap keys = bindingsFromCacheText(defaults(), "BINDINGMODE 0\nbind Z NONE\n");
    CHECK(keys.at("TOGGLESHEATH")[0].empty());
    CHECK(keys.at("TARGETSELF")[0] == "F1");
}

TEST_CASE("a copy with no bind lines is the defaults", "[account_bindings]") {
    CHECK(bindingsFromCacheText(defaults(), "") == defaults());
    CHECK(bindingsFromCacheText(defaults(), "BINDINGMODE 0\r\n") == defaults());
}

TEST_CASE("a full list, as earlier builds sent up, reads back as itself", "[account_bindings]") {
    const std::string full =
        "BINDINGMODE 0\nbind 1 ACTIONBUTTON1\nbind PAD3 ACTIONBUTTON1\nbind F1 TARGETSELF\nbind Z TOGGLESHEATH\n";
    CHECK(bindingsFromCacheText(defaults(), full) == defaults());
}

TEST_CASE("only the keys off their default are written", "[account_bindings]") {
    CHECK(bindingsCacheText(defaults(), defaults()).empty());

    BindingKeyMap moved = defaults();
    moved["TARGETSELF"] = {"", ""};
    moved["RAIDTARGET8"] = {"F1", ""};
    moved["TOGGLESHEATH"] = {"", ""};
    const std::string text = bindingsCacheText(defaults(), moved);
    CHECK(text == "BINDINGMODE 0\r\nbind F1 RAIDTARGET8\r\nbind Z NONE\r\n");
    // And the original's reading of it gives the same keys back.
    CHECK(bindingsFromCacheText(defaults(), text) == moved);
}
