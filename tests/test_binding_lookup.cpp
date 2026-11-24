// Which key binding a press runs (addons/binding_lookup.hpp), and the client's
// default ones as WTF\DefaultBindings.wtf states them.
#include <catch_amalgamated.hpp>

#include "addons/account_bindings.hpp"
#include "addons/binding_lookup.hpp"

using namespace wowee::addons;
using Keys = std::vector<std::string>;

TEST_CASE("a press is looked up with its modifiers and then without", "[bindings]") {
    // 0x005622e0's tables.
    CHECK(bindingLookups("W") == Keys{"W"});
    CHECK(bindingLookups("SHIFT-W") == Keys{"SHIFT-W", "W"});
    CHECK(bindingLookups("CTRL-SHIFT-TAB") == Keys{"CTRL-SHIFT-TAB", "SHIFT-TAB", "CTRL-TAB", "TAB"});
    CHECK(bindingLookups("ALT-CTRL-SHIFT-1") == Keys{"ALT-CTRL-SHIFT-1", "CTRL-SHIFT-1", "ALT-SHIFT-1",
                                                     "ALT-CTRL-1", "SHIFT-1", "CTRL-1", "ALT-1", "1"});
}

TEST_CASE("a key of a dash is a key", "[bindings]") {
    CHECK(bindingLookups("-") == Keys{"-"});
    CHECK(bindingLookups("CTRL--") == Keys{"CTRL--", "-"});
}

TEST_CASE("the default bindings are the file's", "[bindings]") {
    // The client began with thirty-seven of its own and had the rest unbound.
    const BindingKeyMap keys = bindingsFromCacheText({}, "bind W MOVEFORWARD\r\n"
                                                         "bind UP MOVEFORWARD\r\n"
                                                         "bind SHIFT-1 ACTIONPAGE1\r\n"
                                                         "bind CTRL-- MASTERVOLUMEDOWN\r\n"
                                                         "bind F12 TOGGLEBACKPACK\r\n");
    CHECK(keys.at("MOVEFORWARD") == std::array<std::string, 2>{"W", "UP"});
    CHECK(keys.at("ACTIONPAGE1")[0] == "SHIFT-1");
    CHECK(keys.at("MASTERVOLUMEDOWN")[0] == "CTRL--");
    CHECK(keys.at("TOGGLEBACKPACK")[0] == "F12");
}

TEST_CASE("a binding's own modifiers are not held for its script", "[bindings]") {
    // 0x0055d740.
    constexpr uint32_t all = 0x3f;
    CHECK(modifiersLeftBy(kLeftAlt, "ALT-1") == 0);
    CHECK(modifiersLeftBy(kRightAlt, "ALT-1") == 0);
    // Nothing bound to ALT-1, and 1's binding ran: self cast.
    CHECK(modifiersLeftBy(kLeftAlt, "1") == kLeftAlt);
    CHECK(modifiersLeftBy(all, "CTRL-SHIFT-TAB") == (kLeftAlt | kRightAlt));
    CHECK(modifiersLeftBy(all, "LSHIFT-F") == (all & ~kLeftShift));
    CHECK(modifiersLeftBy(all, "ALT-CTRL-SHIFT--") == 0);
    CHECK(modifiersLeftBy(all, "") == all);
}

TEST_CASE("the client reads its own keys, and a command's script the others", "[bindings]") {
    CHECK(clientReadsKey("MOVEFORWARD", "UP"));
    CHECK_FALSE(clientReadsKey("MOVEFORWARD", "NUMPAD8"));
    CHECK(clientReadsKey("JUMP", "SPACE"));
    CHECK_FALSE(clientReadsKey("JUMP", "NUMPAD0"));
    CHECK_FALSE(clientReadsKey("TOGGLEAUTORUN", "BUTTON4"));
    CHECK_FALSE(clientReadsKey("ACTIONBUTTON1", "1"));
    CHECK_FALSE(clientReadsKey("TARGETNEARESTENEMY", "TAB"));
    CHECK(clientReadsKey("FRIENDNAMEPLATES", "SHIFT-V"));
    CHECK_FALSE(clientReadsKey("NAMEPLATES", "SHIFT-V"));
}
