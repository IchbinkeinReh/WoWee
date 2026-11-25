// The client's own key defaults against 3.3.5's DefaultBindings.wtf (patch-3.MPQ),
// which binds I to TOGGLELFGPARENT - the Dungeon Finder - and opens every bag on
// SHIFT-B (OPENALLBAGS). This client had the Dungeon Finder on J, a key the
// original leaves unbound, and I on a bag toggle of its own; and every settings
// file it wrote spells those defaults out, so the file has to be moved as well.
#include <catch_amalgamated.hpp>

#include "test_support.hpp"
#include "ui/keybinding_manager.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using wowee::ui::KeybindingManager;
using Action = KeybindingManager::Action;

namespace {

std::string writeConfig(const char* name, const std::string& body) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream(path) << body;
    return path.string();
}

}  // namespace

TEST_CASE("I opens the Dungeon Finder, as 3.3.5 binds it", "[keybindings]") {
    auto& k = KeybindingManager::getInstance();
    k.resetToDefaults();
    CHECK(k.getKeyForAction(Action::TOGGLE_DUNGEON_FINDER) == ImGuiKey_I);
    // Nothing else on I: two actions on one key fire together.
    CHECK(k.getKeyForAction(Action::TOGGLE_INVENTORY) == ImGuiKey_None);
    CHECK(k.getKeyForAction(Action::TOGGLE_BAGS) == ImGuiKey_B);
    CHECK(k.getKeyForAction(Action::TOGGLE_ACHIEVEMENTS) == ImGuiKey_Y);
}

TEST_CASE("a settings file holding the old defaults is moved to the client's",
          "[keybindings]") {
    auto& k = KeybindingManager::getInstance();
    k.resetToDefaults();
    const std::string path = writeConfig("wowee_kb_old_defaults.cfg",
        "[Keybindings]\n"
        "toggle_character_screen=C\n"
        "toggle_inventory=I\n"
        "toggle_bags=B\n"
        "toggle_dungeon_finder=J\n"
        "toggle_achievements=Y\n");
    k.loadFromConfigFile(path);
    CHECK(k.getKeyForAction(Action::TOGGLE_DUNGEON_FINDER) == ImGuiKey_I);
    CHECK(k.getKeyForAction(Action::TOGGLE_INVENTORY) == ImGuiKey_None);
    std::remove(path.c_str());
}

TEST_CASE("a key the player chose is kept", "[keybindings]") {
    auto& k = KeybindingManager::getInstance();
    k.resetToDefaults();
    const std::string path = writeConfig("wowee_kb_chosen.cfg",
        "[Keybindings]\n"
        "toggle_inventory=I\n"
        "toggle_dungeon_finder=G\n");
    k.loadFromConfigFile(path);
    CHECK(k.getKeyForAction(Action::TOGGLE_DUNGEON_FINDER) == ImGuiKey_G);
    CHECK(k.getKeyForAction(Action::TOGGLE_INVENTORY) == ImGuiKey_I);
    std::remove(path.c_str());
    k.resetToDefaults();
}

// P68: a key a frame takes goes nowhere else (0x004943c0). The world map closes on its M in
// OnKeyDown, and the press opened it again; only Escape, Enter and Tab were said to be taken.
TEST_CASE("a key taken by a frame is taken whatever key it is", "[keybindings]") {
    using namespace wowee::ui;
    clearInterfaceConsumedKeys();
    CHECK_FALSE(interfaceConsumedKey(ImGuiKey_M));
    noteInterfaceConsumedKeycode('m');
    CHECK(interfaceConsumedKey(ImGuiKey_M));
    CHECK_FALSE(interfaceConsumedKey(ImGuiKey_N));
    clearInterfaceConsumedKeys();
    noteInterfaceConsumedKeycode(0);
    CHECK_FALSE(interfaceConsumedKey(ImGuiKey_None));
    CHECK(imguiKeyOf('0') == ImGuiKey_0);
    CHECK(imguiKeyOf(27) == ImGuiKey_Escape);
    CHECK(imguiKeyOf('\r') == ImGuiKey_Enter);
    CHECK(imguiKeyOf(0x40000058) == ImGuiKey_Enter);
    CHECK(imguiKeyOf(0x4000003A) == ImGuiKey_F1);
    CHECK(imguiKeyOf(0x40000045) == ImGuiKey_F12);
    CHECK(imguiKeyOf(0x4000004F) == ImGuiKey_RightArrow);
    CHECK(imguiKeyOf(0x7F) == ImGuiKey_Delete);
    CHECK(imguiKeyOf('-') == ImGuiKey_Minus);

    // Both ways the pump has of taking one say so.
    const std::string src = wowee::test::slurp("src/core/application.cpp");
    const auto takes = [&src](const std::string& call) {
        const auto at = src.find(call);
        REQUIRE(at != std::string::npos);
        const auto end = src.find("continue;", at);
        return src.substr(at, end - at).find("noteInterfaceConsumedKeycode(") != std::string::npos;
    };
    CHECK(takes("engine->dispatchFrameKey(event.key.key, true)"));
    CHECK(takes("engine->dispatchBindingKey(\n                                event.key.key,\n"));
}
