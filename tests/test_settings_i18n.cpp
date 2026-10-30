// The settings in German.
//
// A German client showed every setting this client adds in English, beside
// the game's own German options. Each row of the schema now has its German in
// settings_i18n.cpp - or the name of the client's own GlobalStrings entry for
// it - and these check that none was left out, so a row added to the schema
// without one fails here rather than showing up in English on a German client.
#include <catch_amalgamated.hpp>

#include <set>
#include <string>
#include <vector>

#include "ui/settings_i18n.hpp"
#include "ui/settings_schema.hpp"

using namespace wowee::ui;

namespace {

std::size_t choiceCount(const std::string& choices) {
    if (choices.empty()) return 0;
    std::size_t n = 1;
    for (char c : choices) n += c == '|' ? 1 : 0;
    return n;
}

bool anyEmptyChoice(const std::string& choices) {
    std::size_t at = 0;
    while (true) {
        const std::size_t bar = choices.find('|', at);
        if ((bar == std::string::npos ? choices.size() : bar) == at) return true;
        if (bar == std::string::npos) return false;
        at = bar + 1;
    }
}

}  // namespace

TEST_CASE("every setting has a German label", "[settings][i18n]") {
    std::size_t count = 0;
    const SettingDesc* rows = clientSettingsSchema(count);
    REQUIRE(count > 200);

    for (std::size_t i = 0; i < count; ++i) {
        const SettingDesc& d = rows[i];
        INFO(d.key << " has no German");
        const SettingGerman* de = germanSettingText(d.key);
        REQUIRE(de != nullptr);
        INFO(d.key << " has neither a German label nor a GlobalStrings one");
        CHECK(((de->label && *de->label) || (de->gsLabel && *de->gsLabel)));
        // Its own text too, whatever the GlobalStrings say: the login screen
        // has no interface to ask.
        CHECK((de->label && *de->label));
        INFO(d.key << " has a tooltip in English and none in German");
        CHECK((d.tooltip[0] == '\0' || (de->tooltip && *de->tooltip)));
    }
}

TEST_CASE("German labels fit their columns", "[settings][i18n]") {
    // The options pages have columns some 190 wide; a slider shows its label
    // and value on one line above it, and a check box its label beside the
    // box. German runs longer than the English, and the first German labels
    // ran under the next column. Counted in characters, not bytes.
    const auto characters = [](const char* s) {
        std::size_t n = 0;
        for (; *s; ++s) n += (static_cast<unsigned char>(*s) & 0xC0) != 0x80 ? 1 : 0;
        return n;
    };
    std::size_t count = 0;
    const SettingDesc* rows = clientSettingsSchema(count);
    for (std::size_t i = 0; i < count; ++i) {
        const SettingDesc& d = rows[i];
        const SettingGerman* de = germanSettingText(d.key);
        REQUIRE(de != nullptr);
        const std::size_t limit = d.kind == SettingKind::Bool ? 28 : d.kind == SettingKind::Enum ? 34 : 24;
        INFO(d.key << ": \"" << de->label << "\" is longer than " << limit);
        CHECK(characters(de->label) <= limit);
    }
}

TEST_CASE("the German table has the schema's rows and no others", "[settings][i18n]") {
    std::size_t count = 0;
    const SettingDesc* rows = clientSettingsSchema(count);
    std::set<std::string> keys;
    for (std::size_t i = 0; i < count; ++i) keys.insert(rows[i].key);

    // The rows a release build leaves out of the schema (#ifndef NDEBUG),
    // which a debug build shows and so has the German of.
    const std::set<std::string> debugOnly = {"fsrjittersign"};

    std::size_t germanCount = 0;
    const SettingGerman* german = germanSettingTable(germanCount);
    std::set<std::string> seen;
    for (std::size_t i = 0; i < germanCount; ++i) {
        INFO(german[i].key << " is in the German table and not in the schema");
        CHECK((keys.count(german[i].key) == 1 || debugOnly.count(german[i].key) == 1));
        INFO(german[i].key << " is in the German table twice");
        CHECK(seen.insert(german[i].key).second);
    }
}

TEST_CASE("German choices match the English ones", "[settings][i18n]") {
    // The index is what is saved, so a German list one short names the wrong
    // choice for every index after the gap.
    std::size_t count = 0;
    const SettingDesc* rows = clientSettingsSchema(count);
    for (std::size_t i = 0; i < count; ++i) {
        const SettingDesc& d = rows[i];
        const SettingGerman* de = germanSettingText(d.key);
        REQUIRE(de != nullptr);
        const std::string en = d.choices;
        if (d.kind != SettingKind::Enum) {
            INFO(d.key << " is not a dropdown and has German choices");
            CHECK(de->choices == nullptr);
            continue;
        }
        INFO(d.key << " is a dropdown with no German choices");
        REQUIRE(de->choices != nullptr);
        INFO(d.key << ": " << en << "  /  " << de->choices);
        CHECK(choiceCount(de->choices) == choiceCount(en));
        CHECK_FALSE(anyEmptyChoice(de->choices));
    }
}

TEST_CASE("every page and heading has a German name", "[settings][i18n]") {
    std::size_t count = 0;
    const SettingDesc* rows = clientSettingsSchema(count);
    for (std::size_t i = 0; i < count; ++i) {
        INFO("category " << rows[i].category);
        CHECK(germanCategoryName(rows[i].category) != nullptr);
        if (rows[i].section[0] != '\0') {
            INFO("section " << rows[i].section);
            CHECK(germanSectionName(rows[i].section) != nullptr);
        }
    }
}

TEST_CASE("the reasons a row is greyed are in German", "[settings][i18n]") {
    for (const std::string& key : rayTracingIgnoredKeys()) {
        INFO(key);
        const char* en = rayTracingIgnoredText(key, false);
        const char* de = rayTracingIgnoredText(key, true);
        REQUIRE(en != nullptr);
        REQUIRE(de != nullptr);
        CHECK(std::string(en) != std::string(de));
    }
}

TEST_CASE("settingText takes the locale and the interface's words", "[settings][i18n]") {
    std::size_t count = 0;
    const SettingDesc* rows = clientSettingsSchema(count);
    const SettingDesc* farclip = nullptr;
    const SettingDesc* fxaa = nullptr;
    for (std::size_t i = 0; i < count; ++i) {
        if (std::string(rows[i].key) == "viewdistance") farclip = &rows[i];
        if (std::string(rows[i].key) == "fxaa") fxaa = &rows[i];
    }
    REQUIRE(farclip != nullptr);
    REQUIRE(fxaa != nullptr);

    // English is the schema, untouched.
    const SettingText en = settingText(*farclip, false);
    CHECK(en.label == farclip->label);
    CHECK(en.category == farclip->category);

    // German without an interface: the table's own text.
    const SettingText de = settingText(*farclip, true);
    CHECK(de.label == germanSettingText("viewdistance")->label);
    CHECK(de.category == germanCategoryName(farclip->category));

    // With one, the client's own string for the options it has.
    const GlobalStringLookup globals = [](const char* name) {
        return std::string(name) == "FARCLIP" ? std::string("Sichtweite (GS)") : std::string();
    };
    CHECK(settingText(*farclip, true, globals).label == "Sichtweite (GS)");
    // And the table's where it has none, or answers nothing.
    CHECK(settingText(*fxaa, true, globals).label == germanSettingText("fxaa")->label);
    // An English client never asks.
    CHECK(settingText(*farclip, false, globals).label == farclip->label);
}

TEST_CASE("only a German client is German", "[settings][i18n]") {
    CHECK(settingsLocaleGerman("deDE"));
    CHECK_FALSE(settingsLocaleGerman("enUS"));
    CHECK_FALSE(settingsLocaleGerman("frFR"));
    CHECK_FALSE(settingsLocaleGerman(""));
}
