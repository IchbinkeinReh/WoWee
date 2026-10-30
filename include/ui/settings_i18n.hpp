#pragma once

/**
 * settings_i18n.hpp - the settings schema in the client's language.
 *
 * The schema's own words are English. A German client showed them as they
 * are: the options panels, the settings window and the login screen's
 * graphics sheet all read "View distance" beside the game's own "Sichtweite".
 *
 * Here every row has its German - its label, its hover text and its choices -
 * along with the categories and section headings. A row the original client
 * has an option for also names that option's GlobalStrings entries, and the
 * interface's own text is used when the interface is there to give it; the
 * German written here is the same text, for the login screen, which has no
 * interface yet.
 *
 * Another locale keeps the English: there is no translation for it, and the
 * English is what the schema is written in.
 */

#include <cstddef>
#include <functional>
#include <string>

#include "ui/settings_schema.hpp"

namespace wowee {
namespace ui {

/// One row in German.
struct SettingGerman {
    const char* key;
    const char* label;
    const char* tooltip;
    /// Enum rows: the choices, '|'-separated, as many and in the same order as
    /// the schema's. nullptr for other rows.
    const char* choices;
    /// The client's own GlobalStrings entry for the label, or nullptr where the
    /// option is this client's alone.
    const char* gsLabel;
    /// The same for the hover text (OPTION_TOOLTIP_*), or nullptr.
    const char* gsTooltip;
};

/// The German of a row, or nullptr for a key the table does not have.
const SettingGerman* germanSettingText(const std::string& key);
/// Every row of the table, in schema order; `count` set to how many.
const SettingGerman* germanSettingTable(std::size_t& count);

/// A category's or section heading's German, or nullptr.
const char* germanCategoryName(const std::string& category);
const char* germanSectionName(const std::string& section);

/// Whether the settings are shown in German for this client locale.
inline bool settingsLocaleGerman(const std::string& locale) { return locale == "deDE"; }

/// What one row says in the client's language.
struct SettingText {
    std::string label;
    std::string tooltip;
    std::string choices;
    std::string category;
    std::string section;
};

/// Answers a GlobalStrings name with the interface's text, or "" where there
/// is no interface or no such string.
using GlobalStringLookup = std::function<std::string(const char* name)>;

/// The row's words: the schema's for English, the table's for German - with
/// the interface's own text for the rows that have a GlobalStrings entry,
/// where `globals` answers one.
SettingText settingText(const SettingDesc& desc, bool german, const GlobalStringLookup& globals = {});

/// A category's or section heading's name in the client's language.
std::string categoryText(const std::string& category, bool german);
std::string sectionText(const std::string& section, bool german);

/// rayTracingIgnoredReason in the client's language, or nullptr.
const char* rayTracingIgnoredText(const std::string& key, bool german);

}  // namespace ui
}  // namespace wowee
