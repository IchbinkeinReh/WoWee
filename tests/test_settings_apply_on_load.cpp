// A graphics setting read from the config file has to reach something.
//
// Loading parses each key into a pending field and stops there. Those fields
// are what the panel displays; the renderer learns a value only when
// applySettingSideEffects is called for it, which happened when a slider moved
// and when a preset was chosen, and never when settings were loaded.
//
// So a view distance chosen on the login screen was written to disk, read back
// into the panel at startup, shown correctly on the slider, and never applied.
// The panel looked like it did nothing.
//
// The oracle is the source: every graphics key applySettingSideEffects knows
// how to apply has to be in the list the loader walks, or it is a setting that
// only takes effect after being touched by hand.
#include <catch_amalgamated.hpp>

#include "test_support.hpp"

#include <fstream>
#include <regex>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <cctype>
#include <utility>
#include <vector>

#include "ui/graphics_defaults.hpp"
#include "ui/graphics_presets.hpp"
#include "ui/settings_schema.hpp"

namespace {



// The names between a `constexpr const char* <list>[] = { ... };`
std::set<std::string> namesIn(const std::string& source, const std::string& list) {
    const size_t start = source.find("kGraphics" + list);
    if (start == std::string::npos) return {};
    const size_t end = source.find("};", start);
    const std::string body = source.substr(start, end - start);
    std::set<std::string> out;
    const std::regex name(R"RX("([a-z]+)")RX");
    for (std::sregex_iterator it(body.begin(), body.end(), name), last;
         it != last; ++it) {
        out.insert((*it)[1].str());
    }
    return out;
}


// Whether a field is handed to something rather than merely mentioned.
//
// The field name appearing anywhere in the file is not enough: a comment, a
// (void) cast or a line that writes it back to the config all contain it, and
// a setting can be loaded, named and never applied. An occurrence counts when
// the previous non-space character is '(' or ',' - it is an argument - or '='
// with the field on the right, which assigns it into something live.
bool handedToSomething(const std::string& source, const std::string& field) {
    for (size_t at = source.find(field); at != std::string::npos;
         at = source.find(field, at + 1)) {
        const size_t lineStart = source.rfind('\n', at) + 1;
        const std::string before = source.substr(lineStart, at - lineStart);
        if (before.find("//") != std::string::npos) continue;  // in a comment

        size_t back = at;
        while (back > 0 && std::isspace(static_cast<unsigned char>(source[back - 1]))) --back;
        // The qualifier in front of the member is part of the expression, not
        // the thing that receives it.
        static const std::string kQualifier = "settingsPanel_.";
        if (back >= kQualifier.size() &&
            source.compare(back - kQualifier.size(), kQualifier.size(), kQualifier) == 0) {
            back -= kQualifier.size();
            while (back > 0 && std::isspace(static_cast<unsigned char>(source[back - 1]))) --back;
        }
        if (back == 0) continue;
        const char prev = source[back - 1];
        if (prev == '(' || prev == ',' || prev == '=') return true;
    }
    return false;
}

}  // namespace

TEST_CASE("the test can read the source it checks", "[settings]") {
    const std::string panel = wowee::test::slurp("src/ui/settings_panel.cpp");
    REQUIRE(panel.size() > 1000);
    REQUIRE(panel.find("kGraphicsApplyKeys") != std::string::npos);
}

TEST_CASE("every setting a preset sets is also applied on load", "[settings]") {
    // A preset walks its own key list to push values out. Loading has to push
    // at least the same set, or choosing a preset and restarting gives two
    // different pictures.
    const std::string panel = wowee::test::slurp("src/ui/settings_panel.cpp");
    const auto presetKeys = namesIn(panel, "PresetKeys");
    const auto applyKeys = namesIn(panel, "ApplyKeys");
    REQUIRE(presetKeys.size() >= 10);
    REQUIRE(applyKeys.size() >= presetKeys.size());

    for (const std::string& key : presetKeys) {
        INFO("preset key not applied on load: " << key);
        CHECK(applyKeys.count(key) == 1);
    }
}

TEST_CASE("the settings the user reported are on the applied list",
          "[settings]") {
    const auto applyKeys =
        namesIn(wowee::test::slurp("src/ui/settings_panel.cpp"), "ApplyKeys");
    for (const char* key : {"viewdistance", "groundclutter", "shadows",
                            "shadowdistance", "rtwater", "rtwaveamp", "rtwavefreq", "brightness"}) {
        INFO(key);
        CHECK(applyKeys.count(key) == 1);
    }
}

TEST_CASE("the graphics defaults have one home", "[settings]") {
    // The same two numbers were written in the settings panel, in the login
    // screen's copy of the same struct, and in the reset button's constant.
    CHECK(wowee::ui::kDefaultViewDistance == 1277.0f);
    CHECK(wowee::ui::kDefaultGroundClutter == 70);

    // And they have to be reachable on the sliders that show them, or the
    // panel opens on a value it cannot represent.
    CHECK(wowee::ui::kDefaultViewDistance >= 177.0f);
    CHECK(wowee::ui::kDefaultViewDistance <= 1277.0f);
    CHECK(wowee::ui::kDefaultGroundClutter >= 0);
    CHECK(wowee::ui::kDefaultGroundClutter <= 150);
}

TEST_CASE("no default is spelled out a second time", "[settings]") {
    // The literals themselves, so a new copy has to be a deliberate act.
    const std::string panelHeader = wowee::test::slurp("include/ui/settings_panel.hpp");
    const std::string authHeader = wowee::test::slurp("include/ui/auth_screen.hpp");
    REQUIRE(panelHeader.size() > 500);
    REQUIRE(authHeader.size() > 500);

    CHECK(panelHeader.find("kDefaultViewDistance") != std::string::npos);
    CHECK(panelHeader.find("kDefaultGroundClutter") != std::string::npos);
    // The old value, in either header, would mean a fourth copy came back.
    CHECK(panelHeader.find("1200.0f") == std::string::npos);
    CHECK(authHeader.find("1200.0f") == std::string::npos);
}

// ---------------------------------------------------------------------------
// The same question, asked of every setting rather than only the graphics ones.
//
// The list above covers what a preset sets, which is graphics. The rest of the
// settings are applied at startup too, but by hand, from five separate places -
// applyCameraControlSettings, a vsync check in setServices, a frame cap latch, a
// lens flare block, and the bag lines in loadSettings. Nothing checked that a
// setting had one of those, so field of view had none: loadSettings applied it
// behind `if (auto* renderer = services_.renderer)`, and loadSettings runs from
// the constructor, where services have not been injected yet. The camera is
// built asking for 60. The slider read the saved value, the world never did.

namespace {

/// key -> the pending field it reads, from the binding table that already
/// exists to map one to the other.
std::map<std::string, std::string> fieldBindings(const std::string& panel) {
    std::map<std::string, std::string> out;
    const std::regex bind(R"RX(\{\.key = "([a-z0-9_]+)",\s*\.as\w+\s*=\s*&SettingsPanel::(\w+))RX");
    for (std::sregex_iterator it(panel.begin(), panel.end(), bind), last;
         it != last; ++it) {
        out[(*it)[1].str()] = (*it)[2].str();
    }
    return out;
}

/// The keys applySettingSideEffects knows how to push somewhere.
std::set<std::string> sideEffectKeys(const std::string& panel) {
    const size_t start = panel.find("void SettingsPanel::applySettingSideEffects");
    REQUIRE(start != std::string::npos);
    const size_t end = panel.find("\n}\n", start);
    const std::string body = panel.substr(start, end - start);

    std::set<std::string> out;
    const std::regex key(R"RX(key == "([a-z0-9_]+)")RX");
    for (std::sregex_iterator it(body.begin(), body.end(), key), last;
         it != last; ++it) {
        out.insert((*it)[1].str());
    }
    return out;
}

}  // namespace

TEST_CASE("every setting reaches its target at startup", "[settings]") {
    const std::string panel = wowee::test::slurp("src/ui/settings_panel.cpp");
    const std::string screen = wowee::test::slurp("src/ui/game_screen.cpp");
    REQUIRE(panel.size() > 1000);
    REQUIRE(screen.size() > 1000);

    const auto bindings = fieldBindings(panel);
    const auto sideEffects = sideEffectKeys(panel);
    const auto applyKeys = namesIn(panel, "ApplyKeys");
    REQUIRE(bindings.size() >= 40);
    REQUIRE(sideEffects.size() >= 40);

    // The ones that reach their target another way. Each is here because it
    // does, not because it is allowed not to.
    const std::map<std::string, const char*> otherwise = {
        // The inventory screen is a member of GameScreen rather than a service
        // pointer, so it already exists while the constructor reads the file
        // and loadSettings can hand these straight to it.
        {"bagscale", "applied to the inventoryScreen member in loadSettings"},
        {"separatebags", "applied to the inventoryScreen member in loadSettings"},
        {"showkeyring", "applied to the inventoryScreen member in loadSettings"},
        // Applying a preset on load would push its values over the individual
        // settings that were just read.
        {"graphicspreset", "would overwrite the settings loaded beside it"},
        // Reached by a call rather than by reading the field.
        {"windowuiscale", "applyWindowUiScale() is called from GameScreen"},
        // Pulled rather than pushed: GetCVar("chatStyle") answers from this
        // setting, and the interface reads that every time it activates or
        // deactivates a chat box - including on its own way up, which is what
        // decides whether the box is on screen at login. The side effect is
        // for the tick mid-session, when nothing is about to ask.
        {"chatboxvisible", "the interface reads it through GetCVar(\"chatStyle\")"},
    };

    for (const std::string& key : sideEffects) {
        if (applyKeys.count(key)) continue;  // on the graphics load list

        auto explained = otherwise.find(key);
        if (explained != otherwise.end()) {
            INFO(key << " is excused because: " << explained->second);
            CHECK(true);
            continue;
        }

        auto bound = bindings.find(key);
        INFO(key << " has a side effect, is not applied on load, and has no"
                    " field binding to find it by");
        REQUIRE(bound != bindings.end());

        INFO(key << " is loaded into " << bound->second
                 << ", which nothing in game_screen.cpp hands to anything -"
                    " so the value is shown on the control and never applied");
        CHECK(handedToSomething(screen, bound->second));
    }
}

TEST_CASE("field of view is applied where the camera exists", "[settings]") {
    // The specific one this found. It is applied from applyCameraControlSettings
    // because that runs once the renderer has been injected; applying it while
    // the config is being read cannot work, and looked like it did.
    const std::string screen = wowee::test::slurp("src/ui/game_screen.cpp");
    const std::string loader = wowee::test::slurp("src/ui/game_screen_minimap.cpp");

    const size_t at = screen.find("void GameScreen::applyCameraControlSettings");
    REQUIRE(at != std::string::npos);
    const std::string body = screen.substr(at, screen.find("\n}\n", at) - at);
    INFO("applyCameraControlSettings no longer applies the saved fov");
    CHECK(body.find("setFov") != std::string::npos);
    CHECK(body.find("pendingFov") != std::string::npos);

    // And it is not applied again from the loader, where it cannot work.
    const size_t load = loader.find("void GameScreen::loadSettings");
    REQUIRE(load != std::string::npos);
    INFO("the loader applies fov again, from where there is no renderer");
    CHECK(loader.find("camera->setFov", load) == std::string::npos);
}

// ---------------------------------------------------------------------------
// A preset named High has to mean one thing.
//
// The settings window and the login screen each had their own copy of the
// preset values, and they had drifted in four of ten columns: High was 350
// yards of shadow with 4x MSAA and no FXAA in game, and 250 yards with 2x and
// FXAA on at the login screen. Medium disagreed about clutter and
// multisampling, and Low still turned shadows off there while the client
// drew the shadow pass whatever the setting said.

TEST_CASE("the presets are written once", "[settings]") {
    // The login screen had a copy of the preset table's application, and of
    // the settings it applied them to. It shows the game's own settings now,
    // so it has no preset of its own to get wrong.
    const std::string login = wowee::test::slurp("src/ui/auth_screen.cpp");
    REQUIRE(login.size() > 1000);
    CHECK(login.find("kGraphicsPresets") == std::string::npos);
    CHECK(login.find("applyPresetToState") == std::string::npos);
}

TEST_CASE("Low turns dynamic shadows off and the rest leave them on", "[settings]") {
    // The client's own Low quality level has ShadowQuality 0 and every level
    // from Good up has dynamic shadows. Off skips the shadow pass now, so the
    // column means what it says.
    REQUIRE(wowee::ui::kGraphicsPresetCount == 4);
    CHECK_FALSE(wowee::ui::kGraphicsPresets[0].shadows);
    for (int i = 1; i < wowee::ui::kGraphicsPresetCount; ++i) {
        INFO("preset index " << i << " turns shadows off");
        CHECK(wowee::ui::kGraphicsPresets[i].shadows);
    }
}

TEST_CASE("the presets carry the client's own detail levels", "[settings]") {
    // GraphicsQualityLevels.lua levels 1, 3, 4 and 5 - the rows the view
    // distances already come from: EnvironmentDetail, ClutterRadius,
    // ParticleDensity and WeatherIntensity, in this client's units.
    struct Row { int environment, clutterRadius, particles, weather; };
    constexpr Row kClient[] = {{50, 70, 10, 0}, {100, 120, 60, 2}, {125, 130, 80, 3}, {150, 140, 100, 3}};
    REQUIRE(wowee::ui::kGraphicsPresetCount == 4);
    for (int i = 0; i < wowee::ui::kGraphicsPresetCount; ++i) {
        const auto& p = wowee::ui::kGraphicsPresets[i];
        INFO("preset index " << i);
        CHECK(p.environmentDetail == kClient[i].environment);
        CHECK(p.groundClutterDistance == kClient[i].clutterRadius);
        CHECK(p.particleDensity == kClient[i].particles);
        CHECK(p.weatherDetail == kClient[i].weather);
    }
}

TEST_CASE("a preset applies the detail levels it carries", "[settings]") {
    // A column in the table that the panel does not assign is how Low came to
    // leave the doodad distance at 100%.
    const std::string panel = wowee::test::slurp("src/ui/settings_panel.cpp");
    const size_t at = panel.find("void SettingsPanel::applyGraphicsPreset");
    REQUIRE(at != std::string::npos);
    const std::string body = panel.substr(at, panel.find("\n}\n", at) - at);
    for (const char* column : {"p.environmentDetail", "p.groundClutterDistance",
                               "p.particleDensity", "p.weatherDetail", "p.shadows"}) {
        INFO(column << " is not applied by applyGraphicsPreset");
        CHECK(body.find(column) != std::string::npos);
    }
    const auto presetKeys = namesIn(panel, "PresetKeys");
    for (const char* key : {"environmentdetail", "groundclutterdistance", "particledensity",
                            "weatherdetail"}) {
        INFO(key << " is not pushed out by a preset");
        CHECK(presetKeys.count(key) == 1);
    }
}

TEST_CASE("every preset leaves the client's missing extras off", "[settings]") {
    // Lens flare, sharp stars, sun shafts, light shafts and sky-tinted fog are
    // not in the original client. They stay as opt-in
    // extras; no preset turns one on, and picking a preset turns each off.
    for (int i = 0; i < wowee::ui::kGraphicsPresetCount; ++i) {
        const auto& p = wowee::ui::kGraphicsPresets[i];
        INFO("preset index " << i);
        CHECK(p.lensFlare == 0.0f);
        CHECK_FALSE(p.sharpStars);
        CHECK_FALSE(p.sunShafts);
        CHECK(p.lightShafts == 0);
        CHECK(p.fogSkyBlend == 0.0f);
    }
}

TEST_CASE("the presets climb", "[settings]") {
    // Each step up is meant to ask for more than the one below it. A column
    // that goes backwards is a preset that improves something by turning it
    // down, which is how the two copies were spotted disagreeing.
    for (int i = 1; i < wowee::ui::kGraphicsPresetCount; ++i) {
        const auto& lo = wowee::ui::kGraphicsPresets[i - 1];
        const auto& hi = wowee::ui::kGraphicsPresets[i];
        INFO("preset " << i << " asks for less than " << (i - 1));
        CHECK(hi.viewDistance >= lo.viewDistance);
        CHECK(hi.shadowDistance >= lo.shadowDistance);
        CHECK(hi.antiAliasing >= lo.antiAliasing);
        CHECK(hi.groundClutter >= lo.groundClutter);
    }
}

// ---------------------------------------------------------------------------
// The login screen once kept its own copy of eighteen graphics settings, with
// its own loader, ranges, defaults and preset table. Each of those disagreed
// with the game's at some point - a dropdown with two entries of a three-entry
// scale, four keys with no clamp, a floor of 50 where the schema said 40, a
// preset table four columns stale. Its sheet now draws the schema's graphics
// pages over the game's own settings, so there is nothing of its own left to
// drift.

TEST_CASE("the login sheet keeps no settings of its own", "[settings]") {
    const std::string login = wowee::test::slurp("src/ui/auth_screen.cpp");
    const std::string header = wowee::test::slurp("include/ui/auth_screen.hpp");
    REQUIRE(login.size() > 1000);
    REQUIRE(header.size() > 500);

    CHECK(header.find("LoginGraphicsState") == std::string::npos);
    CHECK(login.find("loginGfx_") == std::string::npos);
    CHECK(login.find("saveLoginGraphicsState") == std::string::npos);
    // Built from the schema's graphics pages, through the game's settings.
    CHECK(login.find("clientSettingsSchema") != std::string::npos);
    CHECK(login.find("isGraphicsCategory") != std::string::npos);
    CHECK(login.find("settingsAccess_.set") != std::string::npos);
    CHECK(login.find("settingEnabled(") != std::string::npos);
}

// ---------------------------------------------------------------------------
// A setting that never reaches the file resets every run.
//
// From the player's seat that is the same complaint as the latch bug - "it does
// not remember what I set" - but the cause is at the other end: the value is
// applied, it works for the session, and nothing writes it down. Nothing
// checked that a setting the panel binds is written at all, so a new row could
// be added to the schema, bound, applied, and still be gone at the next login.

namespace {

/// Every identifier that appears on the right of a `out << "key=" << ... <<
/// "\n"` line, whatever shape the expression takes. A bool is written as
/// `(settingsPanel_.pendingX ? 1 : 0)` and a number as `settingsPanel_.pendingX`,
/// and a pattern that only knows the second reports the thirty-five bools as
/// unsaved.
std::set<std::string> identifiersWritten(const std::string& saver) {
    std::set<std::string> out;
    const std::regex line(R"RX(out << "[a-z0-9_]+=" << ([^\n]+?) << "\\n")RX");
    const std::regex word(R"RX(\b(\w+)\b)RX");
    for (std::sregex_iterator it(saver.begin(), saver.end(), line), last; it != last; ++it) {
        const std::string expr = (*it)[1].str();
        for (std::sregex_iterator w(expr.begin(), expr.end(), word), wlast; w != wlast; ++w) {
            out.insert((*w)[1].str());
        }
    }
    return out;
}

}  // namespace

TEST_CASE("every setting the panel binds is written down and read back", "[settings]") {
    const std::string panel = wowee::test::slurp("src/ui/settings_panel.cpp");
    const std::string saver = wowee::test::slurp("src/ui/game_screen_minimap.cpp");
    REQUIRE(panel.size() > 1000);
    REQUIRE(saver.size() > 1000);

    const auto bindings = fieldBindings(panel);
    const auto written = identifiersWritten(saver);
    REQUIRE(bindings.size() >= 70);

    // Without this the sweep can report nothing wrong by matching nothing at
    // all, which is how the thirty-five bools came to look unsaved.
    INFO("the save lines stopped matching, so nothing below was checked");
    REQUIRE(written.size() >= 60);

    std::set<std::string> read;
    const std::regex assign(R"RX(settingsPanel_\.(\w+)\s*=)RX");
    for (std::sregex_iterator it(saver.begin(), saver.end(), assign), last; it != last; ++it) {
        read.insert((*it)[1].str());
    }
    REQUIRE(read.size() >= 60);

    // Kept for the session on purpose: a debug view saved came back at the
    // next start and was taken for a lighting bug (the "Probe light ratio"
    // view drawing a night outdoors flat grey).
    const std::set<std::string> sessionOnly = {"dbgview"};
    for (const auto& [key, field] : bindings) {
        if (sessionOnly.count(key)) {
            INFO(key << " is meant to last only for the session, but is saved");
            CHECK(written.count(field) == 0);
            continue;
        }
        INFO(key << " is bound to " << field
                 << ", which nothing writes to the config - it will work for the"
                    " session and be gone at the next login");
        CHECK(written.count(field) == 1);

        INFO(key << " is bound to " << field
                 << ", which the loader never assigns - the file may hold it and"
                    " the panel will still open on its default");
        CHECK(read.count(field) == 1);
    }
}

// ---------------------------------------------------------------------------
// Two things about the schema that the checks above cannot see.
//
// The persistence check walks the bindings that point at SettingsPanel fields.
// Five settings do not: the chat channels bind to ChatSettings members, which
// ChatPanel exposes again as references under different names, and it is those
// names the saver writes. That chain is what makes them persist and nothing
// tested any link in it.
//
// And a schema row with no binding at all is a control the options panels and
// the addon API can offer, describe, and fail to set - setSettingValue answers
// false and the caller has nowhere to put the value.

TEST_CASE("every setting in the schema can be set", "[settings]") {
    const std::string panel = wowee::test::slurp("src/ui/settings_panel.cpp");
    REQUIRE(panel.size() > 1000);

    const auto bindings = fieldBindings(panel);
    std::set<std::string> reachable;
    for (const auto& [key, field] : bindings) reachable.insert(key);

    const std::regex chat(R"RX(\{"([a-z0-9_]+)",\s*&ChatSettings::(\w+)\})RX");
    for (std::sregex_iterator it(panel.begin(), panel.end(), chat), last; it != last; ++it) {
        reachable.insert((*it)[1].str());
    }
    // Handled by a branch of its own, because choosing a preset sets the other
    // settings rather than a field.
    reachable.insert("graphicspreset");
    REQUIRE(reachable.size() >= 70);

    std::size_t count = 0;
    const wowee::ui::SettingDesc* rows = wowee::ui::clientSettingsSchema(count);
    REQUIRE(count > 0);
    for (std::size_t i = 0; i < count; ++i) {
        // A row on the game's own store is set through that store, not
        // through setSettingValue; see SettingDesc::store.
        if (rows[i].store[0] != '\0') continue;
        INFO(rows[i].key << " is offered by the schema and setSettingValue has"
                            " nowhere to put it - the control answers false");
        CHECK(reachable.count(rows[i].key) == 1);
    }
}

TEST_CASE("the chat channel settings reach the file", "[settings]") {
    const std::string panel = wowee::test::slurp("src/ui/settings_panel.cpp");
    const std::string chatPanel = wowee::test::slurp("include/ui/chat_panel.hpp");
    const std::string saver = wowee::test::slurp("src/ui/game_screen_minimap.cpp");
    REQUIRE(chatPanel.size() > 500);

    const std::regex chat(R"RX(\{"([a-z0-9_]+)",\s*&ChatSettings::(\w+)\})RX");
    int checked = 0;
    for (std::sregex_iterator it(panel.begin(), panel.end(), chat), last; it != last; ++it) {
        const std::string key = (*it)[1].str();
        const std::string member = (*it)[2].str();

        // ChatPanel names the same bool again as a reference. That alias is
        // what the saver and loader use, so a binding whose member lost its
        // alias is a setting that stops being written without anything moving.
        const std::regex alias(R"RX(bool&\s+(\w+)\s*=\s*settings\.)RX" + member + R"RX(\s*;)RX");
        std::smatch m;
        INFO(key << " binds to ChatSettings::" << member
                 << ", which ChatPanel does not alias - so the saver, which"
                    " writes the aliases, cannot be writing this one");
        REQUIRE(std::regex_search(chatPanel, m, alias));

        const std::string aliasName = m[1].str();
        INFO(key << " is stored in " << aliasName << ", which nothing writes to the config");
        CHECK(saver.find("<< (chatPanel_." + aliasName) != std::string::npos);
        INFO(key << " is stored in " << aliasName << ", which the loader never assigns");
        CHECK(saver.find("chatPanel_." + aliasName + " =") != std::string::npos);
        ++checked;
    }
    INFO("no ChatSettings binding was found, so this checked nothing");
    CHECK(checked >= 5);
}

// ---------------------------------------------------------------------------
// A live value and the setting it comes from move together, or neither means
// anything.
//
// Five settings are kept twice: a pending field, which is what the file is
// written from and what the panel shows, and a live member, which is what the
// client reads while it runs. applySettingSideEffects copies one to the other,
// so anything that writes just one of them is a value that works now and is
// wrong later - and every item in the minimap's context menu had a version of
// it. Two told the minimap and not the setting at all; the third wrote the live
// member, so the toggle held for the session, saved as whatever it had been,
// and was put back by the next apply.

namespace {

/// live member -> the pending field it belongs to, paired by the name they
/// already share: pendingMinimapSquare and minimapSquare_.
///
/// Derived from the header rather than from the copies applySettingSideEffects
/// makes, which was the first version of this and covered five of the seven.
/// It missed minimapRotate_, because rotation has no schema row and so no copy
/// there - and rotation is where this whole fault was first found. It missed
/// uiOpacity_ as well, whose two halves are not even the same type.
std::map<std::string, std::string> livePendingPairs(const std::string& header) {
    std::set<std::string> members;
    const std::regex member(R"RX(\b(\w+_)\s*(?:=|;))RX");
    for (std::sregex_iterator it(header.begin(), header.end(), member), last; it != last; ++it) {
        members.insert((*it)[1].str());
    }

    std::map<std::string, std::string> out;
    const std::regex pending(R"RX(\b(pending[A-Z]\w*)\s*(?:=|;))RX");
    for (std::sregex_iterator it(header.begin(), header.end(), pending), last; it != last; ++it) {
        const std::string name = (*it)[1].str();
        std::string live = name.substr(std::string("pending").size());
        live[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(live[0])));
        live += "_";
        if (members.count(live)) out[live] = name;
    }
    return out;
}

}  // namespace

TEST_CASE("nothing writes a live setting without the field it is saved from", "[settings]") {
    const auto pairs = livePendingPairs(wowee::test::slurp("include/ui/settings_panel.hpp"));
    INFO("no live/pending pairs were found, so this checked nothing");
    REQUIRE(pairs.size() >= 7);

    // Where the toggles are. The settings panel is excluded because it owns
    // both sides; the loader sets both together and is inside this file.
    for (const char* file : {"src/ui/game_screen_minimap.cpp", "src/ui/game_screen.cpp",
                             "src/ui/game_screen_hud.cpp"}) {
        const std::string src = wowee::test::slurp(file);
        REQUIRE(src.size() > 500);

        std::vector<std::string> lines;
        for (size_t pos = 0; pos <= src.size();) {
            const size_t eol = src.find('\n', pos);
            lines.push_back(src.substr(pos, (eol == std::string::npos ? src.size() : eol) - pos));
            if (eol == std::string::npos) break;
            pos = eol + 1;
        }

        for (size_t i = 0; i < lines.size(); ++i) {
            for (const auto& [live, pending] : pairs) {
                const std::regex write(R"RX(\b)RX" + live + R"RX(\s*=[^=])RX");
                if (!std::regex_search(lines[i], write)) continue;

                // Its twin has to be set with it. Six lines either way covers
                // the loader's two-line form and a menu item's body.
                std::string window;
                for (size_t j = (i > 6 ? i - 6 : 0); j < lines.size() && j <= i + 6; ++j) {
                    window += lines[j];
                    window += "\n";
                }
                INFO(file << ":" << (i + 1) << " sets " << live << " and not "
                          << pending << ", which is the one the file is written"
                             " from - so this holds for the session and is put"
                             " back by the next apply");
                CHECK(window.find(pending) != std::string::npos);
            }
        }
    }
}

TEST_CASE("no preset turns the client's specular option on", "[settings]") {
    // The client's cvar defaults to "0" (0x0078e1a0) and the exe holds no
    // quality presets that would set it; only VideoHardware.dbc rows do.
    for (int i = 0; i < wowee::ui::kGraphicsPresetCount; ++i) {
        INFO("preset index " << i);
        CHECK_FALSE(wowee::ui::kGraphicsPresets[i].specular);
    }
    const auto applyKeys =
        namesIn(wowee::test::slurp("src/ui/settings_panel.cpp"), "ApplyKeys");
    CHECK(applyKeys.count("specular") == 1);
}

TEST_CASE("the enhanced water is gone, and a file that names it still loads", "[settings]") {
    // No row, no field, no switch on the login sheet: its water is the ray
    // traced water's now, and without ray tracing the client's own.
    const std::string schema = wowee::test::slurp("src/ui/settings_schema.cpp");
    CHECK(schema.find("\"enhancedwater\"") == std::string::npos);
    CHECK(wowee::test::slurp("include/ui/settings_panel.hpp").find("pendingEnhancedWater") == std::string::npos);
    CHECK(wowee::test::slurp("include/ui/graphics_presets.hpp").find("enhancedWater") == std::string::npos);
    // The game's loader reads past the old key and its saver no longer
    // writes it. The login sheet saves through that saver, which writes the
    // whole file, so the key goes from there too.
    const std::string files = wowee::test::slurp("src/ui/game_screen_minimap.cpp");
    CHECK(files.find("else if (key == \"enhanced_water\") {}") != std::string::npos);
    CHECK(files.find("out << \"enhanced_water=") == std::string::npos);
    CHECK(files.find("out << \"rt_wave_amp=") != std::string::npos);
    CHECK(files.find("out << \"rt_wave_freq=") != std::string::npos);
    CHECK(wowee::test::slurp("src/ui/auth_screen.cpp").find("enhanced_water") == std::string::npos);
}

TEST_CASE("resetting the graphics resets the resolution to the first-run size", "[settings]") {
    // The login sheet's Reset and the settings window's Restore Video
    // Defaults both put the resolution back, and to the size a first run
    // opens at - the window's own - not to a number written beside it.
    const std::string panel = wowee::test::slurp("src/ui/settings_panel.cpp");
    REQUIRE(panel.size() > 1000);
    const size_t at = panel.find("void SettingsPanel::restoreGraphicsDefaults");
    REQUIRE(at != std::string::npos);
    const std::string body = panel.substr(at, panel.find("\n}\n", at) - at);
    CHECK(body.find("restoreDefaultResolution()") != std::string::npos);

    const size_t res = panel.find("void SettingsPanel::restoreDefaultResolution");
    REQUIRE(res != std::string::npos);
    const std::string resBody = panel.substr(res, panel.find("\n}\n", res) - res);
    CHECK(resBody.find("getFirstRunWidth") != std::string::npos);
    CHECK(resBody.find("applyResolution") != std::string::npos);
    CHECK(panel.find("kDefaultResW") == std::string::npos);
}

TEST_CASE("Restore Video Defaults puts back every graphics page but the Debug ones", "[settings]") {
    // Its list of pages was written out by hand and left Grass off it. It is
    // read off the schema now, so a page added there is restored here too.
    const std::string panel = wowee::test::slurp("src/ui/settings_panel.cpp");
    REQUIRE(panel.size() > 1000);
    const size_t button = panel.find("\"Restore Video Defaults\"");
    REQUIRE(button != std::string::npos);
    const std::string body = panel.substr(button, 1200);
    CHECK(body.find("videoDefaultCategories()") != std::string::npos);
    CHECK(body.find("\"Shadows\"") == std::string::npos);

    const size_t fn = panel.find("SettingsPanel::videoDefaultCategories()");
    REQUIRE(fn != std::string::npos);
    const std::string fnBody = panel.substr(fn, panel.find("\n}\n", fn) - fn);
    CHECK(fnBody.find("isGraphicsCategory") != std::string::npos);
    CHECK(fnBody.find("\"Debug\"") != std::string::npos);

    // And Grass is a graphics page, with its four rows.
    CHECK(wowee::ui::isGraphicsCategory("Grass"));
    std::size_t count = 0;
    const wowee::ui::SettingDesc* rows = wowee::ui::clientSettingsSchema(count);
    int grass = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (std::string(rows[i].category) == "Grass") ++grass;
    }
    CHECK(grass == 4);
}
